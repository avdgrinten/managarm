#pragma once

#include <async/result.hpp>
#include <string>
#include <unordered_set>

#include "reactor.hpp"

// Launches block-nvme for the NVMe controllers that show up on the PCI bus.
class NvmeBinder final : public UeventRule {
public:
	explicit NvmeBinder(Reactor &reactor) : UeventRule{reactor} {}

	bool handle(const Uevent &uevent) override;

private:
	// MBUS_IDs of the PCI devices that we already launched block-nvme for.
	std::unordered_set<std::string> nvmeDevices_;
};

// Starts NVMe-oF once the network link to the NVMe-oF server is up.
// netserver is the mbus ID of netserver, server is the address of the NVMe-oF server.
async::result<void> startNvmeOf(Reactor &reactor, std::string netserver, std::string server);
