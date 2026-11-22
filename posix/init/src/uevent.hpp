#pragma once

#include <expected>
#include <optional>
#include <string>
#include <unordered_map>

using Uevent = std::unordered_map<std::string, std::string>;

// This class implements a udevd-like mechanism to discover devices via netlink uevents.
class UeventEngine {
public:
	void init();

	// Trigger synthetic uevents that are handled by nextUevent().
	void trigger();

	// Yields std::nullopt if no uevent arrives within timeoutMs; a negative timeout waits indefinitely.
	std::expected<std::optional<Uevent>, int> nextUevent(int timeoutMs = -1);

private:
	int nlFd_{-1};
};
