#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <async/scope.hpp>
#include <core/cmdline.hpp>
#include <fcntl.h>
#include <frg/std_compat.hpp>
#include <helix/ipc.hpp>
#include <optional>
#include <print>
#include <stdexcept>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <sys/mount.h>
#include <unistd.h>

#include "config.hpp"
#include "log.hpp"
#include "netserver.hpp"
#include "nvme.hpp"
#include "reactor.hpp"
#include "root.hpp"
#include "spawn.hpp"
#include "uevent.hpp"

bool logDiscovery = false;

namespace {

// The debugging UART, essential bus and storage drivers, and the HDA sound driver.
constexpr std::string_view earlyServers[] = {
#if defined (__x86_64__)
	"uart",
	"ehci",
#endif
	"xhci",
	"virtio-block",
#if defined (__x86_64__)
	"block-ata",
#endif
	"block-ahci",
	"block-nvme",
	"usb-storage",
	"snd-hda",
};

// Lets findRoot() run in a scope of coroutines that do not return values.
async::result<void> findRootInto(Reactor &reactor, std::string rootSpec, std::string &devNode) {
	devNode = co_await findRoot(reactor, std::move(rootSpec));
}

// Brings up the drivers that are needed to access the root partition.
// Returns the device node of the root partition.
async::result<std::string> discoverRoot(Reactor &reactor, const Config &config,
		std::optional<mbus_ng::EntityId> netserver) {
	std::string devNode;
	co_await async::race_and_cancel(
		// NVMe controllers can show up at any time, so this only completes by cancellation.
		[&] (async::cancellation_token ct) { return bindNvmePciDevices(reactor, ct); },
		// Init needs to wait for all of these before it can enter the root file system.
		[&] (async::cancellation_token) {
			return async::dynamic_scope<async::result<void>>(frg::stl_allocator{}, [&] (auto &spawn) {
				spawn(findRootInto(reactor, config.root, devNode));
				if(!config.netserverDevice.empty())
					spawn(bindNetserver(reactor, config.netserverDevice));
				if(netserver)
					spawn(startNvmeOf(reactor, std::to_string(*netserver), config.netserverServer));
			});
		}
	);
	co_return devNode;
}

} // namespace

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	std::println("init: Entering first stage");

	for(auto name : earlyServers) {
		if(auto e = runServer(name); !e)
			std::println("init: Failed to run {}: {}", name, strerror(e.error()));
	}

	Cmdline cmdlineHelper{};
	auto config = parseConfig(async::run(cmdlineHelper.get(), helix::currentDispatcher));
	logDiscovery = config.logDiscovery;

	// This blocks on mbus, so it cannot be done from a coroutine that runs on the Reactor.
	// It is done before the uevent socket exists, such that uevents do not pile up meanwhile.
	std::optional<mbus_ng::EntityId> netserver;
	if(config.nvmeOverFabric)
		netserver = findNetserver();

	auto socket = UeventSocket::create();
	if(!socket) {
		std::println("init: Failed to create the uevent socket: {}", strerror(socket.error()));
		abort();
	}
	Reactor reactor{*socket};

	// The uevents are queued in the socket until the Reactor runs.
	triggerColdplug();
	auto rootPath = reactor.run(discoverRoot(reactor, config, netserver));

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	if(auto e = runServer("uhci"); !e)
		std::println("init: Failed to run uhci: {}", strerror(e.error()));
#endif

	std::println("init: Mounting {}", rootPath);
	if(mount(rootPath.c_str(), "/realfs", "ext2", 0, ""))
		throw std::runtime_error("mount() failed");

	if(chroot("/realfs"))
		throw std::runtime_error("chroot() failed");
	// Some programs, e.g. bash with its builtin getcwd() cannot deal with CWD outside of /.
	if(chdir("/"))
		throw std::runtime_error("chdir() failed");

	std::println("init: On /realfs");

	if(access("/etc/machine-id", F_OK)) {
		std::string args[] = {"/usr/bin/systemd-machine-id-setup"};
		auto process = Process::spawn(args, {});
		if(process)
			process->wait();
		else
			std::println("init: Failed to run systemd-machine-id-setup: {}", strerror(process.error()));
	}

	execl("/usr/lib/systemd/systemd", "systemd", nullptr);
	std::println("init: Failed to execve() systemd");
	abort();
}

