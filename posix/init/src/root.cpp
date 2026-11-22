#include <algorithm>
#include <async/algorithm.hpp>
#include <cctype>
#include <chrono>
#include <core/logging.hpp>
#include <errno.h>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <unistd.h>

#include "log.hpp"
#include "root.hpp"

namespace {

constexpr auto reportInterval = std::chrono::seconds{10};

std::string toLower(std::string_view s) {
	std::string result{s};
	std::ranges::transform(result, result.begin(), [] (unsigned char c) { return std::tolower(c); });
	return result;
}

bool hasManagarmRootAttribute(const std::string &device) {
	auto rootAttr = device + "/managarm-root";

	// Check if the managarm-root attribute exists
	if(access(rootAttr.data(), R_OK)) {
		if(errno != ENOENT)
			std::println("init: Failed to access {}: {}", rootAttr, strerror(errno));
		else if(logDiscovery)
			std::println("init: Not the root filesystem");
		return false;
	}
	return true;
}

// Returns the /dev node of a block device, given the path of the device in sysfs.
std::expected<std::string, int> findDevNode(const std::string &device) {
	// Figure out the device's major:minor
	std::ifstream ifs{device + "/dev"};
	std::string dev;
	if(!std::getline(ifs, dev))
		return std::unexpected{EIO};

	unsigned int major, minor;
	if(sscanf(dev.c_str(), "%u:%u", &major, &minor) != 2)
		return std::unexpected{EINVAL};

	// Find the /dev node with the right major:minor numbers
	std::error_code ec;
	std::filesystem::directory_iterator it{"/dev/", ec};
	for(; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
		struct stat st;
		if(stat(it->path().c_str(), &st))
			continue;

		// Character devices have their own major:minor numbers.
		if(S_ISBLK(st.st_mode) && st.st_rdev == makedev(major, minor))
			return it->path().string();
	}
	if(ec)
		return std::unexpected{ec.value()};

	// Device nodes are created before the device's uevents are emitted.
	// Hence, if the node is missing, it will not appear later.
	return std::unexpected{ENOENT};
}

async::result<void> reportMissingRoot(Reactor &reactor, const std::string &description,
		async::cancellation_token ct) {
	while(co_await reactor.sleepFor(reportInterval, ct))
		std::println("init: Still waiting for the root partition {}", description);
}

} // namespace

RootSearch::RootSearch(Reactor &reactor, const std::string &rootSpec) : UeventRule{reactor} {
	if(rootSpec.size()) {
		std::string_view spec{rootSpec};
		// An empty PARTUUID cannot identify a partition.
		constexpr std::string_view prefix = "PARTUUID=";
		if(spec.starts_with(prefix) && spec.size() > prefix.size())
			partuuid_ = toLower(spec.substr(prefix.size()));
		else
			std::println("init: Unsupported root={}, falling back to the partition type", spec);
	}
	description_ = partuuid_
			? std::format("PARTUUID={}", *partuuid_)
			: std::string{"with the Managarm root type"};

	std::println("init: Looking for the root partition {}", description_);
}

// Only the first root partition that shows up is used.
bool RootSearch::handle(const Uevent &uevent) {
	if (lookup(uevent, "SUBSYSTEM") == "block") {
		auto device = std::format("/sys{}", lookup(uevent, "DEVPATH"));
		if(logDiscovery)
			std::println("init: Considering device {}", device);

		bool isRoot;
		if(partuuid_) {
			// Like Linux, root=PARTUUID= ignores the partition type.
			isRoot = toLower(lookup(uevent, "PARTUUID")) == *partuuid_;
		} else {
			isRoot = hasManagarmRootAttribute(device);
		}
		if(!isRoot)
			return true;

		auto devNode = findDevNode(device);
		if(!devNode)
			logPanic("init: {} is the root partition, but its /dev node cannot be determined: {}",
					device, strerror(devNode.error()));
		devNode_ = std::move(*devNode);
		found_.raise();
		return false;
	}
	return true;
}

async::result<void> RootSearch::wait() {
	co_await async::race_and_cancel(
		[&] (async::cancellation_token) { return found_.wait(); },
		[&] (async::cancellation_token ct) { return reportMissingRoot(reactor_, description_, ct); }
	);
}
