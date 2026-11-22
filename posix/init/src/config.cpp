#include <frg/array.hpp>
#include <frg/cmdline.hpp>

#include "config.hpp"

Config parseConfig(std::string_view cmdline) {
	frg::string_view netserverDevice = "";
	frg::string_view netserverServer = "";
	bool nvmeOverFabric = false;
	frg::string_view root = "";

	frg::array args = {
		frg::option{"netserver.device", frg::as_string_view(netserverDevice)},
		frg::option{"nvme.over-fabric", frg::store_true(nvmeOverFabric)},
		frg::option{"netserver.server", frg::as_string_view(netserverServer)},
		frg::option{"root", frg::as_string_view(root)},
	};
	frg::parse_arguments({cmdline.data(), cmdline.size()}, args);

	return Config{
		.netserverDevice{netserverDevice.data(), netserverDevice.size()},
		.netserverServer{netserverServer.data(), netserverServer.size()},
		.nvmeOverFabric = nvmeOverFabric,
		.root{root.data(), root.size()},
	};
}
