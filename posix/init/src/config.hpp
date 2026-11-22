#pragma once

#include <string>
#include <string_view>

// Options that init takes from the kernel command line.
struct Config {
	// netserver.device=: UEFI device path of the NIC that netserver is bound to.
	std::string netserverDevice;
	// netserver.server=: address of the NVMe-oF server.
	std::string netserverServer;
	// nvme.over-fabric: whether NVMe over fabrics is started once the network is up.
	bool nvmeOverFabric = false;
	// root=: selects the root partition; if it is empty, the partition type decides.
	std::string root;
};

Config parseConfig(std::string_view cmdline);
