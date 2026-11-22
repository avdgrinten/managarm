
#include <assert.h>
#include <cctype>
#include <core/cmdline.hpp>
#include <core/device-path.hpp>
#include <fcntl.h>
#include <format>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <arpa/inet.h>
#include <async/algorithm.hpp>
#include <async/scope.hpp>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <print>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <ranges>
#include <filesystem>
#include <optional>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <expected>

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

// Yields std::nullopt if no uevent arrives within timeoutMs; a negative timeout waits indefinitely.
async::result<std::optional<Uevent>>
nextUevent(Reactor &reactor, UeventStream &uevents, int timeoutMs) {
	if(timeoutMs < 0)
		co_return co_await uevents.next();

	std::optional<Uevent> uevent;
	co_await async::race_and_cancel(
		async::lambda([&] (async::cancellation_token ct) -> async::result<void> {
			uevent = co_await uevents.next(ct);
		}),
		async::lambda([&] (async::cancellation_token ct) -> async::result<void> {
			co_await reactor.sleepFor(std::chrono::milliseconds{timeoutMs}, ct);
		})
	);
	co_return uevent;
}

// Completes once the root partition is found and netserver is bound to its NIC.
// Stores the device node of the root partition in devNode.
async::result<void>
waitForBootDevices(Reactor &reactor, const Config &config, std::string &devNode) {
	std::optional<DevicePathParser> uefiNetDp;
	std::filesystem::path dpSysfsPath{};
	bool uefiNetDevpathResolved = config.netserverDevice.empty();

	if(!uefiNetDevpathResolved) {
		auto dp_res = DevicePathParser::fromString(config.netserverDevice);
		if(dp_res) {
			uefiNetDp = dp_res.value();
		} else {
			std::println("init: failed to parse device path '{}'", config.netserverDevice);
			uefiNetDevpathResolved = true;
		}
	}

	// Without root=, the first partition with the Managarm root type is used.
	std::optional<std::string> rootPartuuid;
	if(config.root.size()) {
		std::string_view spec{config.root};
		if(spec.starts_with("PARTUUID="))
			rootPartuuid = toLower(spec.substr(std::string_view{"PARTUUID="}.size()));
		else
			std::println("init: Unsupported root={}, falling back to the partition type", spec);
	}
	std::string rootDescription = rootPartuuid
			? std::format("PARTUUID={}", *rootPartuuid)
			: std::string{"with the Managarm root type"};

	constexpr auto rootReportInterval = std::chrono::seconds{10};
	auto nextRootReport = Clock::now() + rootReportInterval;

	std::optional<std::string> rootPath;

	std::println("init: Looking for the root partition {}", rootDescription);

	UeventStream uevents{reactor};
	triggerColdplug();

	while (!rootPath || !uefiNetDevpathResolved) {
		int timeoutMs = -1;
		if(!rootPath) {
			auto now = Clock::now();
			if(now >= nextRootReport) {
				std::println("init: Still waiting for the root partition {}", rootDescription);
				nextRootReport = now + rootReportInterval;
			}
			auto left = std::chrono::ceil<std::chrono::milliseconds>(nextRootReport - now);
			int reportMs = std::max<int>(left.count(), 0);
			timeoutMs = timeoutMs < 0 ? reportMs : std::min(timeoutMs, reportMs);
		}

		auto uevent = co_await nextUevent(reactor, uevents, timeoutMs);
		// Timed out to report that the root is still missing.
		if (!uevent)
			continue;

		// Note: DEVPATH is unconditionally inserted by generic uevent code.
		const auto &devpath = uevent->at("DEVPATH");
		auto subsystemIt = uevent->find("SUBSYSTEM");

		if (!rootPath && subsystemIt != uevent->end() && subsystemIt->second == "block") {
			auto device = "/sys" + devpath;
			if(logDiscovery)
				std::println("init: Considering device {}", device);

			bool isRoot;
			if(rootPartuuid) {
				// Like Linux, root=PARTUUID= ignores the partition type.
				auto partuuidIt = uevent->find("PARTUUID");
				isRoot = partuuidIt != uevent->end() && toLower(partuuidIt->second) == *rootPartuuid;
			} else {
				isRoot = hasManagarmRootAttribute(device);
			}
			if(isRoot)
				rootPath = findDevNode(device);
		}

		if(!uefiNetDevpathResolved && dpSysfsPath.empty()) {
			auto sysfs = uefiNetDp->sysfs();
			if(sysfs) {
				dpSysfsPath = sysfs.value();
			} else if(sysfs.error() == DevicePathParser::Error::broken) {
				std::println("init: device path '{}' cannot be resolved, not starting netserver",
						config.netserverDevice);
				uefiNetDevpathResolved = true;
			}
		}

		if(!uefiNetDevpathResolved && !dpSysfsPath.empty()) {
			if("/sys" + devpath == dpSysfsPath.string()) {
				if(auto e = bindServer("netserver", uevent->at("MBUS_ID")); !e)
					std::println("init: Failed to bind netserver: {}", strerror(e.error()));
				uefiNetDevpathResolved = true;
			}
		}
	}

	devNode = *rootPath;
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
				spawn(waitForBootDevices(reactor, config, devNode));
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

