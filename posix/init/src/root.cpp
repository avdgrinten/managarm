#include <algorithm>
#include <assert.h>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "log.hpp"
#include "root.hpp"

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
