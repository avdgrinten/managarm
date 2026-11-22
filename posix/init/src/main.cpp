#include <async/result.hpp>
#include <async/scope.hpp>
#include <core/cmdline.hpp>
#include <core/device-path.hpp>
#include <core/logging.hpp>
#include <errno.h>
#include <fcntl.h>
#include <frg/std_compat.hpp>
#include <helix/ipc.hpp>
#include <optional>
#include <print>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

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

// Brings up the drivers that are needed to access the root partition.
// Returns the device node of the root partition.
async::result<std::string> discoverRoot(Reactor &reactor, const Config &config,
		RootSearch &rootSearch, std::optional<NetserverBinder> &netserverBinder,
		std::optional<mbus_ng::EntityId> netserver) {
	// Init needs to wait for all of these before it can enter the root file system.
	co_await async::dynamic_scope<async::result<void>>(frg::stl_allocator{}, [&] (auto &spawn) {
		spawn(rootSearch.wait());
		if(netserverBinder)
			spawn(netserverBinder->wait());
		if(netserver)
			spawn(startNvmeOf(reactor, std::to_string(*netserver), config.netserverServer));
	});
	co_return rootSearch.devNode();
}

void setupConsole() {
	// Errors cannot be reported before the console is set up.
	int fd = open("/dev/helout", O_WRONLY);
	if(fd < 0)
		abort();
	if(dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
		abort();
}

// Runs discoverRoot() on a Reactor. The uevent socket is closed when this returns.
std::string runDiscovery(const Config &config) {
	// This blocks on mbus, so it cannot be done from a coroutine that runs on the Reactor.
	// It is done before the uevent socket exists, such that uevents do not pile up meanwhile.
	std::optional<mbus_ng::EntityId> netserver;
	if(config.nvmeOverFabric) {
		auto entity = findNetserver();
		if(entity)
			netserver = *entity;
		else
			std::println("init: Failed to find netserver on mbus (error {}), not starting NVMe-oF",
					std::to_underlying(entity.error()));
	}

	auto socket = UeventSocket::create();
	if(!socket)
		logPanic("init: Failed to create the uevent socket: {}", strerror(socket.error()));
	Reactor reactor{*socket};

	// The rules exist before the coldplug, so they handle the uevents of all devices.
	NvmeBinder nvmeBinder{reactor};
	RootSearch rootSearch{reactor, config.root};
	std::optional<NetserverBinder> netserverBinder;
	if(!config.netserverDevice.empty()) {
		auto parser = DevicePathParser::fromString(config.netserverDevice);
		if(parser)
			netserverBinder.emplace(reactor, config.netserverDevice, std::move(*parser));
		else
			std::println("init: failed to parse device path '{}'", config.netserverDevice);
	}

	// The uevents are queued in the socket until the Reactor runs.
	triggerColdplug();
	return reactor.run(discoverRoot(reactor, config, rootSearch, netserverBinder, netserver));
}

void enterRoot(const std::string &devNode) {
	std::println("init: Mounting {}", devNode);
	if(mount(devNode.c_str(), "/realfs", "ext2", 0, ""))
		logPanic("init: Failed to mount {} on /realfs: {}", devNode, strerror(errno));

	if(chroot("/realfs"))
		logPanic("init: chroot() to /realfs failed: {}", strerror(errno));
	// Some programs, e.g. bash with its builtin getcwd() cannot deal with CWD outside of /.
	if(chdir("/"))
		logPanic("init: chdir() to / failed: {}", strerror(errno));

	std::println("init: On /realfs");
}

void setupMachineId() {
	if(!access("/etc/machine-id", F_OK))
		return;

	std::string args[] = {"/usr/bin/systemd-machine-id-setup"};
	auto process = Process::spawn(args, {});
	if(!process) {
		std::println("init: Failed to run systemd-machine-id-setup: {}", strerror(process.error()));
		return;
	}

	auto status = process->wait();
	if(!status)
		std::println("init: Failed to wait for systemd-machine-id-setup: {}", strerror(status.error()));
	else if(!WIFEXITED(*status) || WEXITSTATUS(*status))
		std::println("init: systemd-machine-id-setup failed with wait status {:#x}", *status);
}

} // namespace

int main() {
	setupConsole();

	std::println("init: Entering first stage");

	for(auto name : earlyServers) {
		if(auto e = runServer(name); !e)
			std::println("init: Failed to run {}: {}", name, strerror(e.error()));
	}

	Cmdline cmdlineHelper{};
	auto config = parseConfig(async::run(cmdlineHelper.get(), helix::currentDispatcher));
	logDiscovery = config.logDiscovery;

	auto devNode = runDiscovery(config);

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	if(auto e = runServer("uhci"); !e)
		std::println("init: Failed to run uhci: {}", strerror(e.error()));
#endif

	enterRoot(devNode);
	setupMachineId();

	execl("/usr/lib/systemd/systemd", "systemd", nullptr);
	logPanic("init: Failed to execute systemd: {}", strerror(errno));
}
