#include <iostream>
#include <memory>

#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include <helix/dispatcher-pool.hpp>
#include <helix/timer.hpp>

#include "controller.hpp"

std::vector<std::unique_ptr<Controller>> globalControllers;

async::detached bindController(mbus_ng::Entity hwEntity) {
	protocols::hw::Device device((co_await hwEntity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();

	auto& ahciBarInfo = info.barInfo[5];
	assert(ahciBarInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	auto ahciBar = co_await device.accessBar(5);

	helix::UniqueDescriptor irq;

	if (info.numMsis) {
		// TODO: Don't hardcode 0 here
		irq = co_await device.installMsi(0);
		co_await device.enableMsi();
	} else {
		irq = co_await device.accessIrq();
	}

	co_await device.enableBusmaster();
	co_await device.enableDma(false);
	auto [iommuActive, dmaSpace, _] = co_await device.getDmaSpace();

	helix::Mapping mapping{ahciBar, ahciBarInfo.offset, ahciBarInfo.length};

	auto controller = std::make_unique<Controller>(
	    hwEntity.id(),
	    std::move(device),
	    std::move(mapping),
	    std::move(irq),
	    info.numMsis > 0,
	    std::move(dmaSpace),
	    iommuActive
	);
	controller->run();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", "01"},
		mbus_ng::EqualsFilter{"pci-subclass", "06"},
		mbus_ng::EqualsFilter{"pci-interface", "01"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "block/ahci: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

int main() {
	std::cout << "block/ahci: Starting driver\n";

	observeControllers();
	// This driver is not thread-safe yet; run the pool single threaded.
	helix::DispatcherPool::global().setThreadCount(1);
	helix::DispatcherPool::global().blockOn(
			async::suspend_indefinitely(async::cancellation_token{}));
}
