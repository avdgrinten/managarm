
#include <assert.h>
#include <cctype>
#include <core/cmdline.hpp>
#include <core/device-path.hpp>
#include <fcntl.h>
#include <format>
#include <helix/ipc.hpp>
#include <frg/cmdline.hpp>
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
#include <iostream>
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

#include "log.hpp"
#include "nvme.hpp"
#include "root.hpp"
#include "uevent.hpp"

bool logDiscovery = false;

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	std::cout <<"init: Entering first stage" << std::endl;

#if defined (__x86_64__)
	auto uart = fork();
	if(!uart) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/uart.bin", nullptr);
	}else assert(uart != -1);
#endif

	// Start essential bus and storage drivers.
#if defined (__x86_64__)
	auto ehci = fork();
	if(!ehci) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/ehci.bin", nullptr);
	}else assert(ehci != -1);
#endif

	auto xhci = fork();
	if(!xhci) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/xhci.bin", nullptr);
	}else assert(xhci != -1);

	auto virtio = fork();
	if(!virtio) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/virtio-block.bin", nullptr);
	}else assert(virtio != -1);

#if defined (__x86_64__)
	auto block_ata = fork();
	if(!block_ata) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/block-ata.bin", nullptr);
	}else assert(block_ata != -1);
#endif

	auto block_ahci = fork();
	if(!block_ahci) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/block-ahci.bin", nullptr);
	}else assert(block_ahci != -1);

	auto block_nvme = fork();
	if(!block_nvme) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/block-nvme.bin", nullptr);
	}else assert(block_nvme != -1);

	auto block_usb = fork();
	if(!block_usb) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/usb-storage.bin", nullptr);
	}else assert(block_usb != -1);

	auto snd_hda = fork();
	if(!snd_hda) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/snd-hda.bin", nullptr);
	}else assert(snd_hda != -1);

	Cmdline cmdlineHelper{};
	auto cmdline = async::run(cmdlineHelper.get(), helix::currentDispatcher);

	frg::string_view uefiNetDevpath = "";
	bool nvmeOverFabric = false;
	frg::string_view nvmeServer = "";
	frg::string_view rootSpec = "";

	frg::array args = {
		frg::option{"netserver.device", frg::as_string_view(uefiNetDevpath)},
		frg::option{"nvme.over-fabric", frg::store_true(nvmeOverFabric)},
		frg::option{"netserver.server", frg::as_string_view(nvmeServer)},
		frg::option{"root", frg::as_string_view(rootSpec)},
	};
	frg::parse_arguments(cmdline.c_str(), args);

	std::optional<DevicePathParser> uefiNetDp;
	std::filesystem::path dpSysfsPath{};
	bool uefiNetDevpathResolved = uefiNetDevpath.size() == 0;

	if(!uefiNetDevpathResolved) {
		auto dp_res = DevicePathParser::fromString(std::string{uefiNetDevpath.data(), uefiNetDevpath.size()});
		if(dp_res) {
			uefiNetDp = dp_res.value();
		} else {
			std::cout << std::format("init: failed to parse device path '{}'", std::string{uefiNetDevpath.data(), uefiNetDevpath.size()}) << std::endl;
			uefiNetDevpathResolved = true;
		}
	}

	std::string nvmeMbusStr;
	auto startNvmeOf = [&] {
		auto nvme = fork();
		if(!nvme) {
			const char *env[] = { nvmeMbusStr.c_str(), nullptr };
			execle("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/block-nvme.bin", nullptr, env);
		}else assert(nvme != -1);
	};

	// netserver's TCP does not retransmit, so a SYN sent before a link is up would be lost.
	std::optional<NvmeOfLinkWait> nvmeLinkWait;
	if(nvmeOverFabric) {
		auto filter = mbus_ng::Conjunction{{
			mbus_ng::EqualsFilter{"class", "netserver"}
		}};

		auto enumerator = mbus_ng::Instance::global().enumerate(filter);
		auto [_, events] = async::run(enumerator.nextEvents(), helix::currentDispatcher).unwrap();
		assert(events.size() == 1);

		nvmeMbusStr = std::format("MBUS_ID={}", events[0].id);
		std::string serverStr{nvmeServer.data(), nvmeServer.size()};
		in_addr server{};
		if(inet_pton(AF_INET, serverStr.c_str(), &server) == 1) {
			std::cout << std::format("init: Waiting for the network link to {} for NVMe-oF", serverStr) << std::endl;
			nvmeLinkWait.emplace(server);
		} else {
			std::cout << std::format("init: Invalid netserver.server '{}', starting NVMe-oF without waiting", serverStr) << std::endl;
			startNvmeOf();
		}
	}

	// Without root=, the first partition with the Managarm root type is used.
	std::optional<std::string> rootPartuuid;
	if(rootSpec.size()) {
		std::string_view spec{rootSpec.data(), rootSpec.size()};
		if(spec.starts_with("PARTUUID="))
			rootPartuuid = toLower(spec.substr(std::string_view{"PARTUUID="}.size()));
		else
			std::cout << std::format("init: Unsupported root={}, falling back to the partition type", spec) << std::endl;
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
	std::cout << "init: Looking for the root partition " << rootDescription << std::endl;

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
				std::cout << "init: Still waiting for the root partition " << rootDescription << std::endl;
				nextRootReport = now + rootReportInterval;
			}
			auto left = std::chrono::ceil<std::chrono::milliseconds>(nextRootReport - now);
			int reportMs = std::max<int>(left.count(), 0);
			timeoutMs = timeoutMs < 0 ? reportMs : std::min(timeoutMs, reportMs);
		}

		auto nextUevent = ueventEngine.nextUevent(timeoutMs);
		if (!nextUevent) {
			std::cout << "Failed to receive uevent" << std::endl;
			abort();
		}
		// Timed out to poll the NVMe-oF link or to report that the root is still missing.
		if (!*nextUevent)
			continue;
		auto &uevent = *nextUevent;

		if(logDiscovery) {
			std::cout << "init: Received uevent";
			for (const auto &kv : *uevent)
				std::cout << "\n    " << kv.first << "=" << kv.second;
			std::cout << std::endl;
		}

		// Note: DEVPATH is unconditionally inserted by generic uevent code.
		const auto &devpath = uevent->at("DEVPATH");
		auto subsystemIt = uevent->find("SUBSYSTEM");

		// Only launch one block-nvme per device, even if the device is reported more than once.
		if(subsystemIt != uevent->end() && subsystemIt->second == "pci" && uevent->contains("PCI_CLASS") && uevent->at("PCI_CLASS") == "10802"
				&& nvmeDevices.insert(uevent->at("MBUS_ID")).second) {
			auto nvme_server = fork();
			if(!nvme_server) {
				setenv("MBUS_ID", uevent->at("MBUS_ID").c_str(), 1);
				execl("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/block-nvme.bin", nullptr);
			}else assert(nvme_server != -1);
		}

		if (!rootPath && subsystemIt != uevent->end() && subsystemIt->second == "block") {
			auto device = "/sys" + devpath;
			if(logDiscovery)
				std::cout << "init: Considering device " << device << std::endl;

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
				std::cout << std::format("init: device path '{}' cannot be resolved, not starting netserver",
						std::string{uefiNetDevpath.data(), uefiNetDevpath.size()}) << std::endl;
				uefiNetDevpathResolved = true;
			}
		}

		if(!uefiNetDevpathResolved && !dpSysfsPath.empty()) {
			if("/sys" + devpath == dpSysfsPath.string()) {
				auto netserver = fork();
				if(!netserver) {
					setenv("MBUS_ID", uevent->at("MBUS_ID").c_str(), 1);
					execl("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/netserver.bin", nullptr);
				}else assert(netserver != -1);
				uefiNetDevpathResolved = true;
			}
		}
	}

#if defined (__x86_64__)
	// Hack: Start UHCI only after EHCI devices are ready.
	auto uhci = fork();
	if(!uhci) {
		execl("/usr/bin/runsvr", "/usr/bin/runsvr", "run", "/usr/lib/managarm/server/uhci.bin", nullptr);
	}else assert(uhci != -1);
#endif

	std::cout << "init: Mounting " << *rootPath << std::endl;
	if(mount(rootPath->data(), "/realfs", "ext2", 0, ""))
		throw std::runtime_error("mount() failed");

	if(chroot("/realfs"))
		throw std::runtime_error("chroot() failed");
	// Some programs, e.g. bash with its builtin getcwd() cannot deal with CWD outside of /.
	if(chdir("/"))
		throw std::runtime_error("chdir() failed");

	std::cout << "init: On /realfs" << std::endl;

	if(access("/etc/machine-id", F_OK)) {
		auto machineIdPid = fork();
		if(!machineIdPid) {
			execl("/usr/bin/systemd-machine-id-setup", "systemd-machine-id-setup", nullptr);
		}else assert(machineIdPid != -1);

		waitpid(machineIdPid, nullptr, 0);
	}

	execl("/usr/lib/systemd/systemd", "systemd", nullptr);
	std::cout << "init: Failed to execve() systemd" << std::endl;
	abort();
}

