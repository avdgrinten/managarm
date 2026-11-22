#include <algorithm>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/netlink.h>
#include <poll.h>
#include <print>
#include <ranges>
#include <string.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "log.hpp"
#include "uevent.hpp"

std::expected<UeventSocket, int> UeventSocket::create() {
	int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if(fd < 0)
		return std::unexpected{errno};
	UeventSocket self{fd};

	struct sockaddr_nl sa;
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	sa.nl_groups = 1;

	if(bind(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) < 0)
		return std::unexpected{errno};
	return self;
}

UeventSocket::~UeventSocket() {
	if(fd_ >= 0)
		close(fd_);
}

std::expected<std::optional<Uevent>, int> UeventSocket::nextUevent(int timeoutMs) {
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

		pollfd pfd{.fd = fd_, .events = POLLIN};
		ret = poll(&pfd, 1, remainingMs);
		if(ret < 0) {
			int err = errno;
			std::println("init: poll(nlFd) failed! errno: {}", strerror(err));
			return std::unexpected{err};
		}
		if(!ret)
			return std::nullopt;

		std::string buf;
		buf.resize(16384);

		ret = read(fd_, buf.data(), buf.size());
		if(ret < 0) {
			int err = errno;
			std::println("init: read(nlFd) failed! errno: {}", strerror(err));
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

void triggerColdplug() {
	for(auto dev : std::filesystem::recursive_directory_iterator{"/sys/devices/"}) {
		if (!dev.is_directory())
			continue;
		auto ueventPath = "/sys/devices/" / dev.path() / "uevent";
		if (!std::filesystem::exists(ueventPath))
			continue;
		if (logDiscovery)
			std::println("init: Triggering {}", ueventPath.string());

		int fd = open(ueventPath.c_str(), O_WRONLY);
		if (fd < 0) {
			std::println("init: Failed to open {} to trigger uevent", ueventPath.string());
			continue;
		}
		std::string_view s = "add";
		if (write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()))
			std::println("init: Failed to write to uevent file {}", ueventPath.string());
		close(fd);
	}
}
