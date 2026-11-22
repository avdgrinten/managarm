#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/netlink.h>
#include <print>
#include <ranges>
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

std::expected<Uevent, int> UeventSocket::receive() {
	std::string buf;
	buf.resize(16384);

	ssize_t size = read(fd_, buf.data(), buf.size());
	if(size < 0)
		return std::unexpected{errno};

	// The message consists of an "<action>@<devpath>" header and NAME=value pairs.
	Uevent uevent;

	std::string_view message{buf.data(), static_cast<size_t>(size)};
	for(auto part : message | std::views::split('\0')) {
		std::string_view line{part};
		auto eq = line.find('=');
		// This skips the header; ACTION and DEVPATH carry the same information.
		if(eq == std::string_view::npos)
			continue;
		uevent.insert_or_assign(std::string{line.substr(0, eq)}, std::string{line.substr(eq + 1)});
	}

	return uevent;
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
