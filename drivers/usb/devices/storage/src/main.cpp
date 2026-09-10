
#include <deque>
#include <optional>
#include <iostream>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <async/result.hpp>
#include <core/cmdline.hpp>
#include <frg/cmdline.hpp>
#include <helix/dispatcher-pool.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "storage.hpp"

namespace {
	// Set by usb-storage.debug and usb-storage.trace on the kernel command line.
	bool logEnumeration = false;
	bool logSteps = false;
}

namespace proto = protocols::usb;

async::result<void> StorageDevice::initialize(int config_num, int intf_num) {
	// I own a USB key that does not support the READ6 command. ~AvdG
	enableRead6 = false;

	auto descriptor = (co_await usbDevice_.configurationDescriptor(0)).unwrap();

	std::optional<int> in_endp_number;
	std::optional<int> out_endp_number;

	for(auto [intf, body] : proto::groupByInterface(proto::configurationRange(descriptor))) {
		// Use only alternateSetting zero.
		// Matches Linux which only ever uses the current altenate setting and never switches away from zero (for bulk-only).
		// TODO: Linux does use a non-zero alternateSetting if UAS is available.
		if(intf.interfaceNumber != intf_num || intf.alternateSetting != 0)
			continue;
		for(auto ep : proto::endpointsOf(body)) {
			if(ep.endpointAddress & 0x80)
				in_endp_number = ep.endpointAddress & 0x0F;
			else
				out_endp_number = ep.endpointAddress & 0x0F;
		}
	}

	if(logEnumeration)
		std::cout << "block-usb: Setting up configuration" << std::endl;

	auto config = (co_await usbDevice_.useConfiguration(0, config_num)).unwrap();
	auto intf = (co_await config.useInterface(intf_num, 0)).unwrap();
	endp_in_ = (co_await intf.getEndpoint(proto::PipeType::in, in_endp_number.value())).unwrap();
	endp_out_ = (co_await intf.getEndpoint(proto::PipeType::out, out_endp_number.value())).unwrap();

	if(logEnumeration)
		std::cout << "block-usb: Device is ready" << std::endl;
}

async::result<frg::expected<scsi::Error, size_t>> StorageDevice::sendScsiCommand(const scsi::CommandInfo &info) {
	co_await transportMutex_.async_lock();
	frg::unique_lock transportLock{frg::adopt_lock, transportMutex_};

	CommandBlockWrapper cbw{};
	cbw.signature = Signatures::kSignCbw;
	cbw.tag = 1;
	cbw.transferLength = info.data.size();
	if(!info.isWrite) {
		cbw.flags = 0x80; // Direction: Device-to-Host.
	}else{
		cbw.flags = 0; // Direction: Host-to-Device.
	}
	cbw.lun = 0;

	cbw.cmdLength = info.command.size();
	memcpy(cbw.cmdData, info.command.data(), info.command.size());

	// TODO: Respect USB device DMA requirements.

	// TODO: Ideally, we want to post the IN-transfer first.
	// We do this to try to avoid unnecessary IRQs
	// and round-trips to the device and host-controller driver.
	CommandStatusWrapper csw;

	if(logSteps)
		std::cout << "block-usb: Sending CBW" << std::endl;
	(co_await endp_out_.transfer(proto::BulkTransfer{proto::XferFlags::kXferToDevice,
			arch::dma_buffer_view{nullptr, &cbw, sizeof(CommandBlockWrapper)}})).unwrap();

	if(logSteps)
		std::cout << "block-usb: Waiting for data" << std::endl;
	if(!info.isWrite) {
		proto::BulkTransfer data_info{proto::XferFlags::kXferToHost, info.data};
		// TODO: We want this to be lazy but that only works if can ensure that
		// the next transaction is also posted to the queue.
//			data_info.lazyNotification = true;
		(co_await endp_in_.transfer(data_info)).unwrap();
	}else{
		(co_await endp_out_.transfer(proto::BulkTransfer{proto::XferFlags::kXferToDevice, info.data})).unwrap();
	}

	if(logSteps)
		std::cout << "block-usb: Waiting for CSW" << std::endl;
	(co_await endp_in_.transfer(proto::BulkTransfer{proto::XferFlags::kXferToHost,
			arch::dma_buffer_view{nullptr, &csw, sizeof(CommandStatusWrapper)}})).unwrap();

	if(logSteps)
		std::cout << "block-usb: Request complete" << std::endl;
	assert(csw.signature == Signatures::kSignCsw);
	assert(csw.tag == 1);
	assert(!csw.dataResidue);
	if(csw.status) {
		co_return scsi::statusToError(csw.status);
	}

	co_return info.data.size();
}

async::detached bindDevice(mbus_ng::Entity entity) {
	auto lane = (co_await entity.getRemoteLane()).unwrap();
	auto device = proto::connect(std::move(lane));

	std::optional<int> config_number;
	std::optional<int> intf_number;
	std::optional<int> intf_class;
	std::optional<int> intf_subclass;
	std::optional<int> intf_protocol;

	if(logEnumeration)
		std::cout << "block-usb: Getting configuration descriptor" << std::endl;

	auto descriptorOrError = co_await device.configurationDescriptor(0);
	if(!descriptorOrError) {
		std::cout << "block-usb: mbus ID " << entity.id()
				<< ": Failed to get configuration descriptor" << std::endl;
		co_return;
	}

	auto cfg = proto::configurationRange(descriptorOrError.value());

	auto configDesc = proto::configDescriptorFrom(cfg);
	if(!configDesc) {
		std::cout << "block-usb: Missing configuration descriptor" << std::endl;
		co_return;
	}
	config_number = configDesc->configValue;

	for(auto intf : proto::interfacesOf(cfg)) {
		if(intf_number) {
			std::cout << "block-usb: Ignoring interface "
					<< int{intf.interfaceNumber} << std::endl;
			continue;
		}
		if(logEnumeration)
			std::cout << "block-usb: Found interface: " << int{intf.interfaceNumber}
					<< ", alternative: " << int{intf.alternateSetting} << std::endl;
		intf_number = intf.interfaceNumber;
		intf_class = intf.interfaceClass;
		intf_subclass = intf.interfaceSubClass;
		intf_protocol = intf.interfaceProtocol;
	}

	if(logEnumeration)
		std::cout << "block-usb: mbus ID " << entity.id()
				<< ": interface class: 0x" << std::hex << intf_class.value()
				<< ", subclass: 0x" << intf_subclass.value()
				<< ", protocol: 0x" << intf_protocol.value()
				<< std::dec << std::endl;
	if(intf_class.value() != protocols::usb::usb_class::mass_storage
			|| intf_subclass.value() != 0x06
			|| intf_protocol.value() != 0x50)
		co_return;

	if(logEnumeration)
		std::cout << "block-usb: Detected USB device" << std::endl;

	auto storage_device = new StorageDevice(device, entity.id());
	co_await storage_device->initialize(config_number.value(), intf_number.value());
	blockfs::runDevice(storage_device);
}

async::detached observeDevices() {
	Cmdline cmdlineHelper{};
	auto cmdline = co_await cmdlineHelper.get();
	frg::array args = {
		frg::option{"usb-storage.debug", frg::store_true(logEnumeration)},
		frg::option{"usb-storage.trace", frg::store_true(logSteps)},
	};
	frg::parse_arguments({cmdline.data(), cmdline.size()}, args);

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"usb.type", "device"},
		mbus_ng::EqualsFilter{"usb.class", "00"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			bindDevice(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "block-usb: Starting driver" << std::endl;

	observeDevices();
	// This driver is not thread-safe yet; run the pool single threaded.
	helix::DispatcherPool::global().setThreadCount(1);
	helix::DispatcherPool::global().blockOn(
			async::suspend_indefinitely(async::cancellation_token{}));

	return 0;
}


