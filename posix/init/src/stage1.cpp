
#include <algorithm>
#include <assert.h>
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
#include <linux/netlink.h>
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

bool logDiscovery = false;
bool systemd = true;

using Uevent = std::unordered_map<std::string, std::string>;

// This class implements a udevd-like mechanism to discover devices via netlink uevents.
class UeventEngine {
public:
	void init() {
		int ret;

		nlFd_ = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
		if(nlFd_ < 0) {
			std::cout << "init: socket(AF_NETLINK) failed! errno: " << strerror(errno) << std::endl;
			return;
		}

		struct sockaddr_nl sa;
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = getpid();
		sa.nl_groups = 1;

		ret = bind(nlFd_, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
		if(ret < 0) {
			std::cout << "init: bind(nlFd) failed! errno: " << strerror(errno) << std::endl;
			return;
		}
	}

	// Trigger synthetic uevents that are handled by nextUevent().
	void trigger() {
		for(auto dev : std::filesystem::recursive_directory_iterator{"/sys/devices/"}) {
			if (!dev.is_directory())
				continue;
			auto ueventPath = "/sys/devices/" / dev.path() / "uevent";
			if (!std::filesystem::exists(ueventPath))
				continue;
			if (logDiscovery)
				std::cout << "Triggering " << ueventPath << std::endl;

			int fd = open(ueventPath.c_str(), O_WRONLY);
			if (fd < 0) {
				std::cout << "Failed open " << ueventPath << " to trigger uevent" << std::endl;
				continue;
			}
			std::string_view s = "add";
			if (write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()))
				std::cout << "Failed to write to uevent file " << ueventPath << std::endl;
			close(fd);
		}
	}

	std::optional<Uevent> nextUevent() {
		int ret;

		while(true) {
			std::string buf;
			buf.resize(16384);

			ret = read(nlFd_, buf.data(), buf.size());
			if(ret < 0) {
				std::cout << "init: read(nlFd) failed! errno: " << strerror(errno) << std::endl;
				return std::nullopt;
			}

			// Parse the uevent message
			Uevent uevent;

			const char *cur = buf.data();
			while(cur < buf.data() + ret) {
				std::string_view line{cur};
				auto split = line | std::views::split('=');
				auto it = split.begin();

				// TODO(qookie): C++23 would let us do std::string_view{*it}
				std::string_view name{(*it).begin(), (*it).end()};
				it++;
				std::string_view value{(*it).begin(), (*it).end()};

				uevent[std::string(name)] = value;

				cur += line.size() + 1;
			}

			const auto &action = uevent.at("ACTION");
			const auto &devpath = uevent.at("DEVPATH");
			if (action != "add")
				continue;
			if (knownDevices_.contains(devpath))
				continue;
			knownDevices_.insert(devpath);
			return uevent;
		}
	}

private:
	int nlFd_{-1};
	std::unordered_set<std::string> knownDevices_;
};

// NICs that netserver binds to in the first stage, such that we can netboot through them.
constexpr std::pair<std::string_view, std::string_view> earlyNics[] = {
	{"0x10ec", "0x8126"}, // RTL8126
};

std::string readAttribute(const std::string &device, const char *name) {
	std::ifstream ifs{device + "/" + name};
	std::string value;
	std::getline(ifs, value);
	return value;
}

bool isEarlyNic(const std::string &device) {
	auto vendor = readAttribute(device, "vendor");
	auto deviceId = readAttribute(device, "device");
	return std::ranges::any_of(earlyNics, [&] (const auto &nic) {
		return vendor == nic.first && deviceId == nic.second;
	});
}

std::optional<std::string> checkRootDevice(std::string device) {
	if(logDiscovery)
		std::cout << "init: Considering device " << device << std::endl;

	auto rootAttr = device + "/managarm-root";

	// Check if the managarm-root attribute exists
	if(access(rootAttr.data(), R_OK)) {
		assert(errno == ENOENT);
		if(logDiscovery)
			std::cout << "init: Not the root filesystem" << std::endl;
		return std::nullopt;
	}

	// Figure out the device's major:minor
	std::ifstream ifs{device + "/dev"};
	std::string dev;
	std::getline(ifs, dev);

	auto split = dev | std::views::split(':');
	auto it = split.begin();

	// TODO(qookie): C++23 would let us do std::string_view{*it}
	std::string majorStr{(*it).begin(), (*it).end()};
	it++;
	std::string minorStr{(*it).begin(), (*it).end()};

	int major = std::stoi(majorStr), minor = std::stoi(minorStr);

	// Find the /dev node with the right major:minor numbers
	for(auto node : std::filesystem::directory_iterator{"/dev/"}) {
		struct stat st;
		stat(node.path().c_str(), &st);

		if(st.st_rdev == makedev(major, minor)) {
			return node.path();
		}
	}

	// This major:minor is not in /dev? Bail out...
	std::cout << "init: Device " << device << " (maj:min " << major << ":" << minor << ")"
		<< "is the root filesystem, but has no corresponding /dev node?" << std::endl;
	return "";
}

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

	frg::array args = {
		frg::option{"netserver.device", frg::as_string_view(uefiNetDevpath)},
		frg::option{"nosystemd", frg::store_false(systemd)},
	};
	frg::parse_arguments(cmdline.c_str(), args);

	std::filesystem::path dpSysfsPath{};
	bool uefiNetDevpathResolved = uefiNetDevpath.size() == 0;
	bool interfaceUp = uefiNetDevpathResolved;

	if(!uefiNetDevpathResolved) {
		auto dp_res = DevicePathParser::fromString(std::string{uefiNetDevpath.data(), uefiNetDevpath.size()});
		if(dp_res) {
			auto dp = dp_res.value();
			dpSysfsPath = std::filesystem::canonical(std::filesystem::path(dp.sysfs()));
		} else {
			std::cout << std::format("init: failed to parse device path '{}'", std::string{uefiNetDevpath.data(), uefiNetDevpath.size()}) << std::endl;
		}
	}

	std::optional<std::string> rootPath;
	// TODO(qookie): Query /proc/cmdline to see if the user
	// requested a different device.

	UeventEngine ueventEngine;
	std::cout << "init: Looking for the root partition" << std::endl;

	ueventEngine.init();
	ueventEngine.trigger();

	while (!rootPath || !uefiNetDevpathResolved || !interfaceUp) {
		auto uevent = ueventEngine.nextUevent();
		if (!uevent) {
			std::cout << "Failed to receive uevent" << std::endl;
			abort();
		}

		if(logDiscovery) {
			std::cout << "init: Received uevent";
			for (const auto &kv : *uevent)
				std::cout << "\n    " << kv.first << "=" << kv.second;
			std::cout << std::endl;
		}

		// Note: DEVPATH is unconditionally inserted by generic uevent code.
		const auto &devpath = uevent->at("DEVPATH");
		auto subsystemIt = uevent->find("SUBSYSTEM");

		if(subsystemIt != uevent->end() && subsystemIt->second == "pci" && uevent->contains("PCI_CLASS") && uevent->at("PCI_CLASS") == "10802") {
			auto nvme_server = fork();
			if(!nvme_server) {
				setenv("MBUS_ID", uevent->at("MBUS_ID").c_str(), 1);
				execl("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/block-nvme.bin", nullptr);
			}else assert(nvme_server != -1);
		}

		if(subsystemIt != uevent->end() && subsystemIt->second == "pci" && isEarlyNic("/sys" + devpath)) {
			auto netserver = fork();
			if(!netserver) {
				setenv("MBUS_ID", uevent->at("MBUS_ID").c_str(), 1);
				execl("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/netserver.bin", nullptr);
			}else assert(netserver != -1);
		}

		if (!rootPath && subsystemIt != uevent->end() && subsystemIt->second == "block")
			rootPath = checkRootDevice("/sys" + devpath);

		if(!uefiNetDevpathResolved) {
			if("/sys" + devpath == dpSysfsPath.string()) {
				auto netserver = fork();
				if(!netserver) {
					setenv("MBUS_ID", uevent->at("MBUS_ID").c_str(), 1);
					execl("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/netserver.bin", nullptr);
				}else assert(netserver != -1);
				uefiNetDevpathResolved = true;
			}
		} else if(!interfaceUp) {
			if(subsystemIt != uevent->end() && subsystemIt->second == "net" && ("/sys" + devpath).starts_with(dpSysfsPath.string())) {
				auto filter = mbus_ng::Conjunction{{
					mbus_ng::EqualsFilter{"class", "netserver"}
				}};

				auto enumerator = mbus_ng::Instance::global().enumerate(filter);
				auto [_, events] = async::run(enumerator.nextEvents(), helix::currentDispatcher).unwrap();
				assert(events.size() == 1);

				auto mbus_str = std::format("MBUS_ID={}", events[0].id);
				auto nvme = fork();
				if(!nvme) {
					const char *env[] = { mbus_str.c_str(), nullptr };
					execle("/usr/bin/runsvr", "/usr/bin/runsvr", "--fork", "bind", "/usr/lib/managarm/server/block-nvme.bin", nullptr, env);
				}else assert(nvme != -1);

				interfaceUp = true;
			}
		}
	}

	if (!rootPath->size())
		throw std::runtime_error("Can't determine root device");

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

	if(!systemd) {
		if(mount("", "/realfs/proc", "procfs", 0, ""))
			throw std::runtime_error("mount() failed");
		if(mount("", "/realfs/sys", "sysfs", 0, ""))
			throw std::runtime_error("mount() failed");
		if(mount("", "/realfs/dev", "devtmpfs", 0, ""))
			throw std::runtime_error("mount() failed");
		if(mount("", "/realfs/run", "tmpfs", 0, ""))
			throw std::runtime_error("mount() failed");
		if(mount("", "/realfs/tmp", "tmpfs", 0, ""))
			throw std::runtime_error("mount() failed");

		if(mkdir("/dev/pts", 0620))
			throw std::runtime_error("mkdir() failed");
		if(mount("", "/realfs/dev/pts", "devpts", 0, ""))
			throw std::runtime_error("mount() failed");
		if(mkdir("/dev/shm", 1777)) // Seems to be the same as linux
			throw std::runtime_error("mkdir() failed");
		if(mount("", "/realfs/dev/shm", "tmpfs", 0, ""))
			throw std::runtime_error("mount() failed");
	}

	if(chroot("/realfs"))
		throw std::runtime_error("chroot() failed");
	// Some programs, e.g. bash with its builtin getcwd() cannot deal with CWD outside of /.
	if(chdir("/"))
		throw std::runtime_error("chdir() failed");

	std::cout << "init: On /realfs" << std::endl;

	if(systemd && access("/etc/machine-id", F_OK)) {
		auto machineIdPid = fork();
		if(!machineIdPid) {
			execl("/usr/bin/systemd-machine-id-setup", "systemd-machine-id-setup", nullptr);
		}else assert(machineIdPid != -1);

		waitpid(machineIdPid, nullptr, 0);
	}

	// Systemd-tmpfilesd will make this for us
	if(!systemd) {
		// /run needs to be 0700 or programs start complaining.
		if(chmod("/run", 0700))
			throw std::runtime_error("chmod() failed");

		// /run/utmp must exist for login to be satisfied.
		int utmp = open("/run/utmp", O_CREAT, O_RDWR);
		if(utmp == -1)
			throw std::runtime_error("Opening /run/utmp failed");
		close(utmp);
	}

	// Systemd-tmpfilesd will make this for us
	if(!systemd) {
		// Symlink /var/run to /run, just like LFS does
		int varrun = symlink("/run", "/var/run");
		if(varrun == -1)
			throw std::runtime_error("Symlinking /var/run failed");
	}

	if(!systemd)
		execl("/usr/bin/init-stage2", "/usr/bin/init-stage2", nullptr);
	else
		execl("/usr/lib/systemd/systemd", "systemd", nullptr);
	std::cout << "init: Failed to execve() second stage" << std::endl;
	abort();
}

