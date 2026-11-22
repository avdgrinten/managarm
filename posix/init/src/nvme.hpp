#pragma once

#include <algorithm>
#include <chrono>
#include <format>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <optional>
#include <print>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "route.hpp"

// Polls whether the interface that routes to the NVMe-oF server reports a carrier. Without a route,
// NVMe-oF cannot connect, so there is no timeout. Not all drivers report the carrier, so once the
// route exists, NVMe-oF is started after a timeout regardless.
class NvmeOfLinkWait {
public:
	using Clock = std::chrono::steady_clock;

	static constexpr auto carrierTimeout = std::chrono::seconds{10};
	static constexpr auto reportInterval = std::chrono::seconds{10};
	static constexpr auto pollInterval = std::chrono::milliseconds{250};

	NvmeOfLinkWait(in_addr server)
	: server_{server}, nextCheck_{Clock::now()}, nextReport_{Clock::now() + reportInterval} { }

	NvmeOfLinkWait(const NvmeOfLinkWait &) = delete;
	NvmeOfLinkWait &operator=(const NvmeOfLinkWait &) = delete;

	~NvmeOfLinkWait() {
		if(fd_ >= 0)
			close(fd_);
	}

	// Returns true once NVMe-oF should be started. Netlink errors are reported and end the wait.
	bool check() {
		auto now = Clock::now();
		if(now < nextCheck_)
			return false;
		nextCheck_ = now + pollInterval;

		if(fd_ < 0) {
			fd_ = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
			if(fd_ < 0) {
				std::println("init: socket(NETLINK_ROUTE) failed: {}, starting NVMe-oF without waiting",
						strerror(errno));
				return true;
			}
		}

		auto link = routeLink(fd_, server_);
		if(!link) {
			std::println("init: Failed to query the route to the NVMe-oF server: {}, starting NVMe-oF without waiting",
					strerror(link.error()));
			return true;
		}

		if(!*link) {
			if(now >= nextReport_) {
				std::println("init: Still waiting for a route to the NVMe-oF server");
				nextReport_ = now + reportInterval;
			}
			return false;
		}

		if((*link)->lowerUp) {
			std::println("init: {} is up, starting NVMe-oF", (*link)->name);
			return true;
		}
		if(!carrierDeadline_)
			carrierDeadline_ = now + carrierTimeout;
		if(now >= *carrierDeadline_) {
			std::println("init: No carrier on {}, starting NVMe-oF anyway", (*link)->name);
			return true;
		}
		return false;
	}

	// Milliseconds until check() needs to be called again.
	int timeoutMs() {
		auto left = std::chrono::ceil<std::chrono::milliseconds>(nextCheck_ - Clock::now());
		return std::max<int>(left.count(), 0);
	}

private:
	in_addr server_;
	Clock::time_point nextCheck_;
	Clock::time_point nextReport_;
	// Set once a route to the server exists.
	std::optional<Clock::time_point> carrierDeadline_;
	int fd_{-1};
};
