
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
#include "nvme.hpp"
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

	std::string nvmeMbusId;
	auto startNvmeOf = [&] {
		if(auto e = bindServer("block-nvme", nvmeMbusId); !e)
			std::println("init: Failed to start NVMe-oF: {}", strerror(e.error()));
	};

	// netserver's TCP does not retransmit, so a SYN sent before a link is up would be lost.
	std::optional<NvmeOfLinkWait> nvmeLinkWait;
	if(config.nvmeOverFabric) {
		auto filter = mbus_ng::Conjunction{{
			mbus_ng::EqualsFilter{"class", "netserver"}
		}};

		auto enumerator = mbus_ng::Instance::global().enumerate(filter);
		auto [_, events] = async::run(enumerator.nextEvents(), helix::currentDispatcher).unwrap();
		assert(events.size() == 1);

		nvmeMbusId = std::to_string(events[0].id);
		in_addr server{};
		if(inet_pton(AF_INET, config.netserverServer.c_str(), &server) == 1) {
			std::println("init: Waiting for the network link to {} for NVMe-oF", config.netserverServer);
			nvmeLinkWait.emplace(server);
		} else {
			std::println("init: Invalid netserver.server '{}', starting NVMe-oF without waiting", config.netserverServer);
			startNvmeOf();
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

	using Clock = std::chrono::steady_clock;
	constexpr auto rootReportInterval = std::chrono::seconds{10};
	auto nextRootReport = Clock::now() + rootReportInterval;

	std::optional<std::string> rootPath;
	// MBUS_IDs of the PCI devices that we already launched block-nvme for.
	std::unordered_set<std::string> nvmeDevices;

	UeventEngine ueventEngine;
	std::println("init: Looking for the root partition {}", rootDescription);

	ueventEngine.init();
	ueventEngine.trigger();

	while (!rootPath || !uefiNetDevpathResolved || nvmeLinkWait) {
		if(nvmeLinkWait && nvmeLinkWait->check()) {
			nvmeLinkWait.reset();
			startNvmeOf();
			continue;
		}

		int timeoutMs = nvmeLinkWait ? nvmeLinkWait->timeoutMs() : -1;
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

		auto nextUevent = ueventEngine.nextUevent(timeoutMs);
		if (!nextUevent) {
			std::println("init: Failed to receive uevent");
			abort();
		}
		// Timed out to poll the NVMe-oF link or to report that the root is still missing.
		if (!*nextUevent)
			continue;
		auto &uevent = *nextUevent;

		if(logDiscovery) {
			std::println("init: Received uevent");
			for (const auto &kv : *uevent)
				std::println("    {}={}", kv.first, kv.second);
		}

		// Note: DEVPATH is unconditionally inserted by generic uevent code.
		const auto &devpath = uevent->at("DEVPATH");
		auto subsystemIt = uevent->find("SUBSYSTEM");

		// Only launch one block-nvme per device, even if the device is reported more than once.
		if(subsystemIt != uevent->end() && subsystemIt->second == "pci" && uevent->contains("PCI_CLASS") && uevent->at("PCI_CLASS") == "10802"
				&& nvmeDevices.insert(uevent->at("MBUS_ID")).second) {
			if(auto e = bindServer("block-nvme", uevent->at("MBUS_ID")); !e)
				std::println("init: Failed to bind block-nvme: {}", strerror(e.error()));
		}

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

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	if(auto e = runServer("uhci"); !e)
		std::println("init: Failed to run uhci: {}", strerror(e.error()));
#endif

	std::println("init: Mounting {}", *rootPath);
	if(mount(rootPath->data(), "/realfs", "ext2", 0, ""))
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

