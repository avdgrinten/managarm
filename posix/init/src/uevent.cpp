#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/netlink.h>
#include <print>
#include <ranges>
#include <string.h>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

#include "log.hpp"
#include "uevent.hpp"

namespace {

// Triggers a synthetic add uevent for the device with the given sysfs directory.
void triggerDevice(const std::filesystem::path &device) {
	auto ueventPath = device / "uevent";

	int fd = open(ueventPath.c_str(), O_WRONLY);
	if (fd < 0) {
		// Directories that do not correspond to devices have no uevent file.
		if (errno != ENOENT)
			std::println("init: Failed to open {} to trigger uevent: {}",
					ueventPath.string(), strerror(errno));
		return;
	}
	if (logDiscovery)
		std::println("init: Triggering {}", ueventPath.string());

	std::string_view s = "add";
	if (write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()))
		std::println("init: Failed to write to uevent file {}", ueventPath.string());
	close(fd);
}

// Devices come and go while we walk sysfs, so errors only end the walk of the affected directory.
void triggerDevicesBelow(const std::filesystem::path &dir) {
	std::error_code ec;
	std::filesystem::directory_iterator it{dir, ec};
	for(; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
		// Symlinks lead to devices that are triggered through their own directories.
		std::error_code typeEc;
		if (it->is_symlink(typeEc) || !it->is_directory(typeEc))
			continue;
		triggerDevice(it->path());
		triggerDevicesBelow(it->path());
	}
	if(ec)
		std::println("init: Failed to list {}: {}", dir.string(), ec.message());
}

} // namespace

std::expected<UeventSocket, int> UeventSocket::create() {
	int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if(fd < 0)
		return std::unexpected{errno};
	UeventSocket self{fd};

	struct sockaddr_nl sa{};
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
	triggerDevicesBelow("/sys/devices");
}
