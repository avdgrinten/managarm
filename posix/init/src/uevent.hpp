#pragma once

#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

using Uevent = std::unordered_map<std::string, std::string>;

// Netlink socket that receives the uevents of the kernel, like udevd does.
class UeventSocket {
public:
	static std::expected<UeventSocket, int> create();

	UeventSocket(UeventSocket &&other) : fd_{std::exchange(other.fd_, -1)} {}

	~UeventSocket();

	// Yields std::nullopt if no uevent arrives within timeoutMs; a negative timeout waits indefinitely.
	std::expected<std::optional<Uevent>, int> nextUevent(int timeoutMs = -1);

private:
	explicit UeventSocket(int fd) : fd_{fd} {}

	int fd_;
};

// Triggers synthetic add uevents for all devices that exist already.
void triggerColdplug();
