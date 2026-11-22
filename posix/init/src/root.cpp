#include <algorithm>
#include <async/algorithm.hpp>
#include <assert.h>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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
		assert(errno == ENOENT);
		if(logDiscovery)
			std::println("init: Not the root filesystem");
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

// Returns the device node of the first root partition that shows up.
async::result<std::string>
matchRoot(UeventStream &uevents, const std::optional<std::string> &partuuid) {
	while(true) {
		auto uevent = co_await uevents.next();

		// Note: DEVPATH is unconditionally inserted by generic uevent code.
		const auto &devpath = uevent.at("DEVPATH");
		auto subsystemIt = uevent.find("SUBSYSTEM");

		if (subsystemIt != uevent.end() && subsystemIt->second == "block") {
			auto device = "/sys" + devpath;
			if(logDiscovery)
				std::println("init: Considering device {}", device);

			bool isRoot;
			if(partuuid) {
				// Like Linux, root=PARTUUID= ignores the partition type.
				auto partuuidIt = uevent.find("PARTUUID");
				isRoot = partuuidIt != uevent.end() && toLower(partuuidIt->second) == *partuuid;
			} else {
				isRoot = hasManagarmRootAttribute(device);
			}
			if(isRoot)
				co_return findDevNode(device);
		}
	}
}

async::result<void> reportMissingRoot(Reactor &reactor, const std::string &description,
		async::cancellation_token ct) {
	while(co_await reactor.sleepFor(reportInterval, ct))
		std::println("init: Still waiting for the root partition {}", description);
}

} // namespace

async::result<std::string> findRoot(Reactor &reactor, std::string rootSpec) {
	UeventStream uevents{reactor};

	// Without root=, the first partition with the Managarm root type is used.
	std::optional<std::string> partuuid;
	if(rootSpec.size()) {
		std::string_view spec{rootSpec};
		if(spec.starts_with("PARTUUID="))
			partuuid = toLower(spec.substr(std::string_view{"PARTUUID="}.size()));
		else
			std::println("init: Unsupported root={}, falling back to the partition type", spec);
	}
	std::string description = partuuid
			? std::format("PARTUUID={}", *partuuid)
			: std::string{"with the Managarm root type"};

	std::println("init: Looking for the root partition {}", description);

	std::string devNode;
	co_await async::race_and_cancel(
		[&] (async::cancellation_token) {
			return async::transform(matchRoot(uevents, partuuid),
					[&] (std::string node) { devNode = std::move(node); });
		},
		[&] (async::cancellation_token ct) { return reportMissingRoot(reactor, description, ct); }
	);
	co_return devNode;
}
