#include <print>
#include <string.h>
#include <unordered_set>

#include "nvme.hpp"
#include "spawn.hpp"

async::result<void> bindNvmePciDevices(Reactor &reactor, async::cancellation_token ct) {
	UeventStream uevents{reactor};
	// MBUS_IDs of the PCI devices that we already launched block-nvme for.
	std::unordered_set<std::string> nvmeDevices;

	while(auto uevent = co_await uevents.next(ct)) {
		auto subsystemIt = uevent->find("SUBSYSTEM");

		// Only launch one block-nvme per device, even if the device is reported more than once.
		if(subsystemIt != uevent->end() && subsystemIt->second == "pci" && uevent->contains("PCI_CLASS") && uevent->at("PCI_CLASS") == "10802"
				&& nvmeDevices.insert(uevent->at("MBUS_ID")).second) {
			if(auto e = bindServer("block-nvme", uevent->at("MBUS_ID")); !e)
				std::println("init: Failed to bind block-nvme: {}", strerror(e.error()));
		}
	}
}
