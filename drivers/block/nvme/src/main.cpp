#include <format>
#include <iostream>

#include <core/cmdline.hpp>
#include <frg/cmdline.hpp>
#include <helix/dispatcher-pool.hpp>
#include <map>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <netinet/in.h>

#include "controller.hpp"
#include "subsystem.hpp"
#include "fabric/tcp.hpp"

std::map<mbus_ng::EntityId, std::unique_ptr<nvme::Subsystem>> globalSubsystems;

namespace {

async::detached runFabrics(mbus_ng::EntityId entity_id) {
	Cmdline cmdHelper;
	bool use_fabric = false;
	auto cmdline = co_await cmdHelper.get();
	frg::string_view server = "";

	frg::array args = {
		frg::option{"nvme.over-fabric", frg::store_true(use_fabric)},
		frg::option{"netserver.server", frg::as_string_view(server)},
	};
	frg::parse_arguments(cmdline.c_str(), args);

	if(!use_fabric)
		co_return;

	std::string remote = std::string{server.data(), server.size()};
	std::cout << std::format("block/nvme: using NVMe-over-fabric to {}\n", remote);

	auto entity = co_await mbus_ng::Instance::global().getEntity(entity_id);
	auto netserverLane = (co_await entity.getRemoteLane()).unwrap();

	auto convert_ip = [](frg::string_view &str, in_addr *addr) -> bool {
		std::string strbuf{str.data(), str.size()};
		return inet_pton(AF_INET, strbuf.c_str(), addr) == 1;
	};

	in_addr server_ip{};
	convert_ip(server, &server_ip);

	auto nvme_subsystem = std::make_unique<nvme::Subsystem>();
	co_await nvme_subsystem->run();

	auto addr = std::format("traddr={},trsvcid={},src_addr=127.0.0.1", remote, 4420);

	auto controller = std::make_unique<nvme::fabric::Tcp>(-1, server_ip, 4420, addr, std::move(netserverLane));
	controller->run(nvme_subsystem->id());
	nvme_subsystem->addController(entity_id, std::move(controller));
	globalSubsystems.insert({nvme_subsystem->id(), std::move(nvme_subsystem)});
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	for(auto &[_, subsys] : globalSubsystems) {
		if(subsys->controllers().contains(base_id))
			co_return protocols::svrctl::Error::success;
	}

	auto entity = co_await mbus_ng::Instance::global().getEntity(base_id);

	auto properties = (co_await entity.getProperties()).unwrap();
	auto classval = std::get_if<mbus_ng::StringItem>(&properties["class"]);
	auto subsystem = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);

	if(classval && classval->value == "netserver") {
		runFabrics(base_id);
	} else if(subsystem && subsystem->value == "pci") {
		if(std::get<mbus_ng::StringItem>(properties.at("pci-class")).value != "01"
		|| std::get<mbus_ng::StringItem>(properties.at("pci-subclass")).value != "08"
		|| std::get<mbus_ng::StringItem>(properties.at("pci-interface")).value != "02")
			co_return protocols::svrctl::Error::deviceNotSupported;

		protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
		co_await device.enableBusmaster();
		co_await device.enableDma(false);
		auto info = co_await device.getPciInfo();
		auto [iommuActive, ioSpace, _] = co_await device.getDmaSpace();

		auto &barInfo = info.barInfo[0];
		assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
		auto bar0 = co_await device.accessBar(0);

		auto loc = std::format("{}:{}:{}.{}",
			std::get<mbus_ng::StringItem>(properties.at("pci-segment")).value,
			std::get<mbus_ng::StringItem>(properties.at("pci-bus")).value,
			std::get<mbus_ng::StringItem>(properties.at("pci-slot")).value,
			std::get<mbus_ng::StringItem>(properties.at("pci-function")).value);

		helix::Mapping mapping{bar0, barInfo.offset, barInfo.length};

		auto nvme_subsystem = std::make_unique<nvme::Subsystem>();
		co_await nvme_subsystem->run();
		auto controller = std::make_unique<PciExpressController>(
		    base_id, std::move(device), loc, std::move(mapping), std::move(ioSpace), iommuActive
		);
		controller->run(nvme_subsystem->id());
		nvme_subsystem->addController(base_id, std::move(controller));
		globalSubsystems.insert({nvme_subsystem->id(), std::move(nvme_subsystem)});
	} else {
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	co_return protocols::svrctl::Error::success;
}

constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice,
};

} // namespace

int main() {
	std::cout << "block/nvme: Starting driver\n";

	async::detach(protocols::svrctl::serveControl(&controlOps));
	// This driver is not thread-safe yet; run the pool single threaded.
	helix::DispatcherPool::global().setThreadCount(1);
	helix::DispatcherPool::global().blockOn(
			async::suspend_indefinitely(async::cancellation_token{}));
}
