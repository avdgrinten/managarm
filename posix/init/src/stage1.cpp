
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

bool logDiscovery = false;

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

	// Yields std::nullopt if no uevent arrives within timeoutMs; a negative timeout waits indefinitely.
	std::expected<std::optional<Uevent>, int> nextUevent(int timeoutMs = -1) {
		int ret;

		// Uevents that are skipped below must not extend the timeout.
		std::optional<std::chrono::steady_clock::time_point> deadline;
		if(timeoutMs >= 0)
			deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};

		while(true) {
			int remainingMs = -1;
			if(deadline) {
				auto left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now());
				remainingMs = std::max<int>(left.count(), 0);
			}

			pollfd pfd{.fd = nlFd_, .events = POLLIN};
			ret = poll(&pfd, 1, remainingMs);
			if(ret < 0) {
				int err = errno;
				std::cout << "init: poll(nlFd) failed! errno: " << strerror(err) << std::endl;
				return std::unexpected{err};
			}
			if(!ret)
				return std::nullopt;

			std::string buf;
			buf.resize(16384);

			ret = read(nlFd_, buf.data(), buf.size());
			if(ret < 0) {
				int err = errno;
				std::cout << "init: read(nlFd) failed! errno: " << strerror(err) << std::endl;
				return std::unexpected{err};
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

			// Devices can be reported more than once (e.g., a device that is announced
			// while trigger() runs gets both its real and a synthetic add uevent).
			// Hence, consumers of the uevents need to be idempotent.
			if (uevent.at("ACTION") != "add")
				continue;
			return uevent;
		}
	}

private:
	int nlFd_{-1};
};

// Sends a NETLINK_ROUTE dump request and invokes fn on each message of the reply.
template<typename Payload, typename F>
std::expected<void, int> netlinkDump(int fd, uint16_t type, Payload payload, F fn) {
	struct {
		nlmsghdr hdr;
		Payload payload;
	} req{};
	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(Payload));
	req.hdr.nlmsg_type = type;
	req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.payload = payload;
	if(send(fd, &req, req.hdr.nlmsg_len, 0) < 0)
		return std::unexpected{errno};

	std::vector<char> buffer(16384);
	while(true) {
		ssize_t len = recv(fd, buffer.data(), buffer.size(), 0);
		if(len < 0)
			return std::unexpected{errno};
		for(auto hdr = reinterpret_cast<nlmsghdr *>(buffer.data()); NLMSG_OK(hdr, len);
				hdr = NLMSG_NEXT(hdr, len)) {
			if(hdr->nlmsg_type == NLMSG_DONE)
				return {};
			if(hdr->nlmsg_type == NLMSG_ERROR) {
				auto err = reinterpret_cast<nlmsgerr *>(NLMSG_DATA(hdr));
				if(err->error)
					return std::unexpected{-err->error};
				return {};
			}
			fn(hdr);
		}
	}
}

// Returns the index of the interface that the most specific route to addr goes through.
std::expected<std::optional<int>, int> routeInterface(int fd, in_addr addr) {
	std::optional<int> index;
	int bestPrefix = -1;
	auto dump = netlinkDump(fd, RTM_GETROUTE, rtmsg{.rtm_family = AF_INET}, [&](nlmsghdr *hdr) {
		if(hdr->nlmsg_type != RTM_NEWROUTE)
			return;
		auto rtm = reinterpret_cast<rtmsg *>(NLMSG_DATA(hdr));
		if(rtm->rtm_family != AF_INET || rtm->rtm_dst_len > 32)
			return;
		uint32_t dst = 0;
		int oif = 0;
		int attrLen = RTM_PAYLOAD(hdr);
		for(auto rta = RTM_RTA(rtm); RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen)) {
			if(rta->rta_type == RTA_DST)
				memcpy(&dst, RTA_DATA(rta), sizeof(dst));
			else if(rta->rta_type == RTA_OIF)
				memcpy(&oif, RTA_DATA(rta), sizeof(oif));
		}
		uint32_t mask = rtm->rtm_dst_len ? ~uint32_t{0} << (32 - rtm->rtm_dst_len) : 0;
		if(!oif || ((ntohl(addr.s_addr) ^ ntohl(dst)) & mask) || rtm->rtm_dst_len <= bestPrefix)
			return;
		index = oif;
		bestPrefix = rtm->rtm_dst_len;
	});
	if(!dump)
		return std::unexpected{dump.error()};
	return index;
}

struct RouteLink {
	std::string name;
	bool lowerUp = false;
};

// Returns the interface that the most specific route to addr goes through.
std::expected<std::optional<RouteLink>, int> routeLink(int fd, in_addr addr) {
	auto index = routeInterface(fd, addr);
	if(!index)
		return std::unexpected{index.error()};
	if(!*index)
		return std::nullopt;

	std::optional<RouteLink> link;
	auto dump = netlinkDump(fd, RTM_GETLINK, ifinfomsg{.ifi_family = AF_UNSPEC}, [&](nlmsghdr *hdr) {
		if(hdr->nlmsg_type != RTM_NEWLINK)
			return;
		auto ifi = reinterpret_cast<ifinfomsg *>(NLMSG_DATA(hdr));
		if(ifi->ifi_index != **index)
			return;
		link.emplace();
		int attrLen = IFLA_PAYLOAD(hdr);
		for(auto rta = IFLA_RTA(ifi); RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen)) {
			if(rta->rta_type == IFLA_IFNAME)
				link->name = reinterpret_cast<char *>(RTA_DATA(rta));
		}
		link->lowerUp = ifi->ifi_flags & IFF_LOWER_UP;
	});
	if(!dump)
		return std::unexpected{dump.error()};
	return link;
}

// Polls whether the interface that routes to the NVMe-oF server reports a carrier. Without a route,
// NVMe-oF cannot connect, so there is no timeout. Not all drivers report the carrier, so once the
// route exists, NVMe-oF is started after a timeout regardless.
class NvmeOfLinkWait {
public:
	using Clock = std::chrono::steady_clock;

	static constexpr auto carrierTimeout = std::chrono::seconds{10};
	static constexpr auto reportInterval = std::chrono::seconds{10};
	static constexpr auto pollInterval = std::chrono::milliseconds{250};

	NvmeOfLinkWait(in_addr server)
	: server_{server}, nextCheck_{Clock::now()}, nextReport_{Clock::now() + reportInterval} { }

	NvmeOfLinkWait(const NvmeOfLinkWait &) = delete;
	NvmeOfLinkWait &operator=(const NvmeOfLinkWait &) = delete;

	~NvmeOfLinkWait() {
		if(fd_ >= 0)
			close(fd_);
	}

	// Returns true once NVMe-oF should be started. Netlink errors are reported and end the wait.
	bool check() {
		auto now = Clock::now();
		if(now < nextCheck_)
			return false;
		nextCheck_ = now + pollInterval;

		if(fd_ < 0) {
			fd_ = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
			if(fd_ < 0) {
				std::cout << std::format("init: socket(NETLINK_ROUTE) failed: {}, starting NVMe-oF without waiting",
						strerror(errno)) << std::endl;
				return true;
			}
		}

		auto link = routeLink(fd_, server_);
		if(!link) {
			std::cout << std::format("init: Failed to query the route to the NVMe-oF server: {}, starting NVMe-oF without waiting",
					strerror(link.error())) << std::endl;
			return true;
		}

		if(!*link) {
			if(now >= nextReport_) {
				std::cout << "init: Still waiting for a route to the NVMe-oF server" << std::endl;
				nextReport_ = now + reportInterval;
			}
			return false;
		}

		if((*link)->lowerUp) {
			std::cout << std::format("init: {} is up, starting NVMe-oF", (*link)->name) << std::endl;
			return true;
		}
		if(!carrierDeadline_)
			carrierDeadline_ = now + carrierTimeout;
		if(now >= *carrierDeadline_) {
			std::cout << std::format("init: No carrier on {}, starting NVMe-oF anyway", (*link)->name) << std::endl;
			return true;
		}
		return false;
	}

	// Milliseconds until check() needs to be called again.
	int timeoutMs() {
		auto left = std::chrono::ceil<std::chrono::milliseconds>(nextCheck_ - Clock::now());
		return std::max<int>(left.count(), 0);
	}

private:
	in_addr server_;
	Clock::time_point nextCheck_;
	Clock::time_point nextReport_;
	// Set once a route to the server exists.
	std::optional<Clock::time_point> carrierDeadline_;
	int fd_{-1};
};

std::string toLower(std::string_view s) {
	std::string result{s};
	std::ranges::transform(result, result.begin(), [] (unsigned char c) { return std::tolower(c); });
	return result;
}

bool hasManagarmRootAttribute(const std::string &device) {
	auto rootAttr = device + "/managarm-root";

	// Check if the managarm-root attribute exists
	if(access(rootAttr.data(), R_OK)) {
		assert(errno == ENOENT);
		if(logDiscovery)
			std::cout << "init: Not the root filesystem" << std::endl;
		return false;
	}
	return true;
}

std::string findDevNode(const std::string &device) {
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

	// Device nodes are created before the device's uevents are emitted.
	// Hence, if the node is missing, it will not appear later.
	throw std::runtime_error(std::format("init: Device {} (maj:min {}:{}) is the root filesystem,"
			" but has no corresponding /dev node", device, major, minor));
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

