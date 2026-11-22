#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <optional>
#include <print>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_set>

#include "nvme.hpp"
#include "route.hpp"
#include "spawn.hpp"

namespace {

constexpr auto carrierTimeout = std::chrono::seconds{10};
constexpr auto reportInterval = std::chrono::seconds{10};
constexpr auto pollInterval = std::chrono::milliseconds{250};

// Polls whether the interface that routes to the NVMe-oF server reports a carrier. Without a route,
// NVMe-oF cannot connect, so there is no timeout. Not all drivers report the carrier, so once the
// route exists, NVMe-oF is started after a timeout regardless.
// Completes once NVMe-oF should be started. Netlink errors are reported and end the wait.
async::result<void> waitForLink(Reactor &reactor, in_addr server) {
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if(fd < 0) {
		std::println("init: socket(NETLINK_ROUTE) failed: {}, starting NVMe-oF without waiting",
				strerror(errno));
		co_return;
	}

	auto nextReport = Clock::now() + reportInterval;
	// Set once a route to the server exists.
	std::optional<Clock::time_point> carrierDeadline;
	while(true) {
		auto now = Clock::now();

		auto link = routeLink(fd, server);
		if(!link) {
			std::println("init: Failed to query the route to the NVMe-oF server: {}, starting NVMe-oF without waiting",
					strerror(link.error()));
			break;
		}

		if(!*link) {
			if(now >= nextReport) {
				std::println("init: Still waiting for a route to the NVMe-oF server");
				nextReport = now + reportInterval;
			}
		} else if((*link)->lowerUp) {
			std::println("init: {} is up, starting NVMe-oF", (*link)->name);
			break;
		} else {
			if(!carrierDeadline)
				carrierDeadline = now + carrierTimeout;
			if(now >= *carrierDeadline) {
				std::println("init: No carrier on {}, starting NVMe-oF anyway", (*link)->name);
				break;
			}
		}

		co_await reactor.sleepFor(pollInterval);
	}

	close(fd);
}

} // namespace

async::result<void> bindNvmePciDevices(Reactor &reactor, async::cancellation_token ct) {
	UeventStream uevents{reactor};
	// MBUS_IDs of the PCI devices that we already launched block-nvme for.
	std::unordered_set<std::string> nvmeDevices;

	while(auto uevent = co_await uevents.next(ct)) {
		if(lookup(*uevent, "SUBSYSTEM") != "pci" || lookup(*uevent, "PCI_CLASS") != "10802")
			continue;

		auto mbusId = lookup(*uevent, "MBUS_ID");
		if(mbusId.empty()) {
			std::println("init: NVMe controller {} has no MBUS_ID", lookup(*uevent, "DEVPATH"));
			continue;
		}

		// Only launch one block-nvme per device, even if the device is reported more than once.
		if(!nvmeDevices.insert(std::string{mbusId}).second)
			continue;

		if(auto e = bindServer("block-nvme", mbusId); !e)
			std::println("init: Failed to bind block-nvme: {}", strerror(e.error()));
	}
}

async::result<void> startNvmeOf(Reactor &reactor, std::string netserver, std::string server) {
	// netserver's TCP does not retransmit, so a SYN sent before a link is up would be lost.
	in_addr addr{};
	if(inet_pton(AF_INET, server.c_str(), &addr) == 1) {
		std::println("init: Waiting for the network link to {} for NVMe-oF", server);
		co_await waitForLink(reactor, addr);
	} else {
		std::println("init: Invalid netserver.server '{}', starting NVMe-oF without waiting", server);
	}

	if(auto e = bindServer("block-nvme", netserver); !e)
		std::println("init: Failed to start NVMe-oF: {}", strerror(e.error()));
}
