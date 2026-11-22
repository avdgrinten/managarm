#pragma once

#include <expected>
#include <netinet/in.h>
#include <optional>
#include <string>

struct RouteLink {
	std::string name;
	bool lowerUp = false;
};

// Returns the interface that the most specific route to addr goes through.
std::expected<std::optional<RouteLink>, int> routeLink(int fd, in_addr addr);
