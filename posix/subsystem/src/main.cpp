#include <memory>
#include <mutex>

#include <linux/vt.h>

#include <async/oneshot-event.hpp>
#include <helix/dispatcher-pool.hpp>
#include <protocols/mbus/client.hpp>

#include "net.hpp"
#include <core/clock.hpp>
#include <core/memory-info.hpp>
#include "devserver.hpp"
#include "netlink/nl-socket.hpp"
#include "devices/full.hpp"
#include "devices/helout.hpp"
#include "devices/kmsg.hpp"
#include "devices/null.hpp"
#include "devices/random.hpp"
#include "devices/tty.hpp"
#include "devices/tty0.hpp"
#include "devices/ttyn.hpp"
#include "devices/urandom.hpp"
#include "devices/zero.hpp"
#include "pts.hpp"
#include "requests.hpp"
#include "observations.hpp"
#include "ostrace.hpp"
#include "swap.hpp"

#include <bragi/helpers-std.hpp>
#include <kerncfg.bragi.hpp>
#include <posix.bragi.hpp>

#include "debug-options.hpp"

// Protects globalCredentialsMap.
std::mutex globalCredentialsMutex;

// Entries are removed before serve() drops its reference to the Process.
std::map<
	std::array<char, 16>,
	Process *
> globalCredentialsMap;

std::optional<std::shared_ptr<Process>> findProcessWithCredentials(helix_ng::CredentialsView credentials) {
	std::array<char, 16> creds;
	memcpy(creds.data(), credentials.data(), 16);
	std::lock_guard lock{globalCredentialsMutex};
	auto it = globalCredentialsMap.find(creds);
	if(it == globalCredentialsMap.end())
		return std::nullopt;
	return it->second->shared_from_this();
}

void registerProcessCredentials(Process *process) {
	std::array<char, 16> creds;
	memcpy(creds.data(), process->credentials().data(), 16);
	std::lock_guard lock{globalCredentialsMutex};
	auto res = globalCredentialsMap.insert({creds, process});
	assert(res.second);
}

async::result<void> serveSignals(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	auto thread = self->threadDescriptor();
	async::cancellation_token cancellation = generation->cancelServe;

	uint64_t sequence = 1;
	while(true) {
		if(cancellation.is_cancellation_requested())
			break;
		//std::cout << "Waiting for raise in " << self->pid() << std::endl;
		auto result = co_await self->pollSignal(sequence, UINT64_C(-1), cancellation);
		sequence = std::get<0>(result);
		//std::cout << "Calling helInterruptThread on " << self->pid() << std::endl;
		HEL_CHECK(helInterruptThread(thread.getHandle()));
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Exiting serveSignals()\e[39m" << std::endl;
	generation->signalsDone.raise();
}

async::result<void> serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation) {
	auto thread = self->threadDescriptor();

	std::array<char, 16> creds;
	HEL_CHECK(helGetCredentials(thread.getHandle(), 0, creds.data()));

	co_await async::when_all(
		observeThread(self, generation),
		serveSignals(self, generation),
		serveRequests(self, generation)
	);

	std::lock_guard lock{globalCredentialsMutex};
	globalCredentialsMap.erase(creds);
}

// --------------------------------------------------------

namespace {
	helix::UniqueLane kerncfgLane;
	helix::UniqueLane pmLane;
	async::oneshot_event foundPm;
	size_t affinityMaskSize = 0;
	size_t totalMemory = 0;
};

helix::UniqueLane &getKerncfgLane() {
	return kerncfgLane;
}

async::result<helix::BorrowedLane> getPmLane() {
	co_await foundPm.wait();
	co_return pmLane;
}

size_t getAffinityMaskSize() {
	assert(affinityMaskSize);
	return affinityMaskSize;
}

size_t getTotalMemory() {
	assert(totalMemory);
	return totalMemory;
}

struct CmdlineNode final : public procfs::RegularNode {
	async::result<std::expected<std::string, Error>> show(Process *) override {
		managarm::kerncfg::GetCmdlineRequest req;

		auto [offer, sendReq, recvResp] =
			co_await helix_ng::exchangeMsgs(
				kerncfgLane,
				helix_ng::offer(
					helix_ng::want_lane,
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		auto resp = *bragi::parse_head_only<managarm::kerncfg::SvrResponse>(recvResp);
		recvResp.reset();
		assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

		std::vector<char> recvCmdline(resp.size());
		auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(recvCmdline.data(), recvCmdline.size())
		);

		HEL_CHECK(recv_tail.error());

		co_return std::string{(const char *)recvCmdline.data(), recvCmdline.size()} + '\n';
	}

	async::result<void> store(std::string) override {
		throw std::runtime_error("Cannot store to /proc/cmdline");
	}
};

async::result<void> enumerateKerncfg() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "kerncfg"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	kerncfgLane = (co_await entity.getRemoteLane()).unwrap();

	// Determine the size of the affinity masks that thor accepts, i.e., one bit per CPU.
	managarm::kerncfg::GetNumCpuRequest numCpuReq;
	auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
		kerncfgLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(numCpuReq, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto numCpuResp = bragi::parse_head_only<managarm::kerncfg::GetNumCpuResponse>(recvResp);
	affinityMaskSize = (numCpuResp->num_cpu() + 7) / 8;

	totalMemory = (co_await core::getMemoryInformation()).totalMemory;

	auto procfsRoot = smarter::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	procfsRoot->directMkregular(getProcfs().get(), "cmdline", makeFsShared<CmdlineNode>());
}

async::result<void> enumeratePm() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "pm-interface"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	pmLane = (co_await entity.getRemoteLane()).unwrap();
	foundPm.raise();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

async::detached runInit() {
	co_await posix::ostContext.create();
	co_await enumerateKerncfg();
	co_await enumerateSwapserver();
	co_await devserver::enumerate();
	async::detach(enumeratePm());
	async::detach(net::enumerateNetserver());
	co_await populateRootView();
	co_await Process::init("usr/bin/posix-init");
}

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;

	helix::DispatcherPool::global().blockOn(clk::enumerateTracker());

//	HEL_CHECK(helSetPriority(kHelThisThread, 1));

	netlink::nl_socket::setupProtocols();

	charRegistry.install(createHeloutDevice());
	charRegistry.install(pts::createMasterDevice());
	charRegistry.install(createNullDevice());
	charRegistry.install(createFullDevice());
	charRegistry.install(createRandomDevice());
	charRegistry.install(createUrandomDevice());
	charRegistry.install(createZeroDevice());
	charRegistry.install(createKmsgDevice());
	charRegistry.install(createTtyDevice());
	charRegistry.install(createTTY0Device());
	for(int i = 1; i <= MAX_NR_CONSOLES; i++)
		charRegistry.install(createTTYNDevice(i));

	runInit();

	helix::DispatcherPool::global().blockOn(
			async::suspend_indefinitely(async::cancellation_token{}));
}
