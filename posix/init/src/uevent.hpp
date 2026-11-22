#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <utility>

using Uevent = std::map<std::string, std::string, std::less<>>;

// Returns the value of a variable; the result is empty if the uevent does not carry the variable.
inline std::string_view lookup(const Uevent &uevent, std::string_view name) {
	auto it = uevent.find(name);
	if(it == uevent.end())
		return {};
	return it->second;
}

// Netlink socket that receives the uevents of the kernel, like udevd does.
class UeventSocket {
public:
	static std::expected<UeventSocket, int> create();

	UeventSocket(UeventSocket &&other) : fd_{std::exchange(other.fd_, -1)} {}

	~UeventSocket();

	int fd() { return fd_; }

	// Receives a single uevent; blocks if none is pending.
	std::expected<Uevent, int> receive();

private:
	explicit UeventSocket(int fd) : fd_{fd} {}

	int fd_;
};

// Triggers synthetic add uevents for all devices that exist already.
void triggerColdplug();
