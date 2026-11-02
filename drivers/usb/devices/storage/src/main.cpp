
#include <algorithm>
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
	// Set by block-usb.debug and block-usb.trace on the kernel command line.
	bool logDebug = false;
	bool logTrace = false;
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

	if(logDebug)
		std::cout << "block-usb: Setting up configuration" << std::endl;

	auto config = (co_await usbDevice_.useConfiguration(0, config_num)).unwrap();
	auto intf = (co_await config.useInterface(intf_num, 0)).unwrap();
	endp_in_ = (co_await intf.getEndpoint(proto::PipeType::in, in_endp_number.value())).unwrap();
	endp_out_ = (co_await intf.getEndpoint(proto::PipeType::out, out_endp_number.value())).unwrap();

	if(logDebug)
		std::cout << "block-usb: Device is ready" << std::endl;
}

async::result<frg::expected<scsi::Error, size_t>> StorageDevice::sendScsiCommand(const scsi::CommandInfo &info) {
	co_await transportMutex_.async_lock();
	frg::unique_lock transportLock{frg::adopt_lock, transportMutex_};

	auto result = co_await runCommand_(info);
	if(!result && result.error().type == scsi::ErrorType::checkCondition) {
		auto error = result.error();
		error.sense = co_await requestSense_();
		co_return error;
	}
	co_return result;
}

async::result<std::optional<scsi::SenseData>> StorageDevice::requestSense_() {
	scsi::FixedSense sense{};
	scsi::RequestSense command{.opCode = 0x03, .allocationLength = sizeof(sense)};
	scsi::CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data{nullptr, &sense, sizeof(sense)},
		.isWrite = false
	};
	auto result = co_await runCommand_(info);
	if(!result)
		co_return std::nullopt;
	co_return scsi::decodeSense(sense);
}

scsi::Error StorageDevice::failTransport_(const char *reason) {
	std::cout << "block-usb: Giving up on the device: " << reason << std::endl;
	transportFailed_ = true;
	return {.type = scsi::ErrorType::transportError, .code = 0};
}

async::result<frg::expected<scsi::Error, size_t>> StorageDevice::runCommand_(const scsi::CommandInfo &info) {
	if(transportFailed_)
		co_return scsi::Error{.type = scsi::ErrorType::transportError, .code = 0};

	// Commands never overlap, but a new tag catches a device repeating an earlier CSW.
	auto tag = nextTag_++;

	CommandBlockWrapper cbw{};
	cbw.signature = Signatures::kSignCbw;
	cbw.tag = tag;
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
	CommandStatusWrapper csw{};

	if(logTrace)
		std::cout << "block-usb: Sending CBW" << std::endl;
	auto cbwResult = co_await endp_out_.transfer(proto::BulkTransfer{proto::XferFlags::kXferToDevice,
			arch::dma_buffer_view{nullptr, &cbw, sizeof(CommandBlockWrapper)}});
	if(!cbwResult)
		co_return failTransport_("CBW failed");

	if(logTrace)
		std::cout << "block-usb: Waiting for data" << std::endl;
	// BOT has no data stage at all when dCBWDataTransferLength is zero.
	// OUT transfers do not report how much the device took.
	size_t hostTransferred = info.data.size();
	if(info.data.size()) {
		frg::expected<proto::UsbError, size_t> dataResult;
		if(!info.isWrite) {
			proto::BulkTransfer data_info{proto::XferFlags::kXferToHost, info.data};
			data_info.allowShortPackets = true;
			dataResult = co_await endp_in_.transfer(data_info);
		}else{
			dataResult = co_await endp_out_.transfer(proto::BulkTransfer{proto::XferFlags::kXferToDevice, info.data});
		}

		// A device fails a command by stalling the data stage, and the CSW follows.
		if(!dataResult) {
			if(dataResult.error() != proto::UsbError::stall)
				co_return failTransport_("Data stage failed");
			hostTransferred = 0;
		}else if(!info.isWrite) {
			hostTransferred = dataResult.value();
		}
	}

	if(logTrace)
		std::cout << "block-usb: Waiting for CSW" << std::endl;
	proto::BulkTransfer csw_info{proto::XferFlags::kXferToHost,
			arch::dma_buffer_view{nullptr, &csw, sizeof(CommandStatusWrapper)}};
	csw_info.allowShortPackets = true;
	auto cswResult = co_await endp_in_.transfer(csw_info);
	// According to Linux, some devices end their data stage with a stray zero-length packet.
	if(cswResult && !cswResult.value())
		cswResult = co_await endp_in_.transfer(csw_info);
	if(!cswResult && cswResult.error() == proto::UsbError::stall)
		cswResult = co_await endp_in_.transfer(csw_info);
	if(!cswResult)
		co_return failTransport_("Reading the CSW failed");

	if(logTrace)
		std::cout << "block-usb: Request complete" << std::endl;
	if(cswResult.value() != sizeof(CommandStatusWrapper)
			|| csw.signature != Signatures::kSignCsw || csw.tag != tag)
		co_return failTransport_("Invalid CSW");
	// CSW status is a BOT status, not a SCSI status byte. In particular,
	// 1 is command failure and 2 is a transport phase error.
	if(csw.status == 1)
		co_return scsi::Error{.type = scsi::ErrorType::checkCondition, .code = csw.status};
	if(csw.status)
		co_return failTransport_("Phase error");

	// Same as Linux: go by what the host saw or the device reported, whichever is less.
	co_return std::min(hostTransferred,
			info.data.size() - std::min<size_t>(csw.dataResidue, info.data.size()));
}

async::detached bindDevice(mbus_ng::Entity entity) {
	auto lane = (co_await entity.getRemoteLane()).unwrap();
	auto device = proto::connect(std::move(lane));

	std::optional<int> config_number;
	std::optional<int> intf_number;
	std::optional<int> intf_class;
	std::optional<int> intf_subclass;
	std::optional<int> intf_protocol;

	if(logDebug)
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
		if(logDebug)
			std::cout << "block-usb: Found interface: " << int{intf.interfaceNumber}
					<< ", alternative: " << int{intf.alternateSetting} << std::endl;
		intf_number = intf.interfaceNumber;
		intf_class = intf.interfaceClass;
		intf_subclass = intf.interfaceSubClass;
		intf_protocol = intf.interfaceProtocol;
	}

	if(logDebug)
		std::cout << "block-usb: mbus ID " << entity.id()
				<< ": interface class: 0x" << std::hex << intf_class.value()
				<< ", subclass: 0x" << intf_subclass.value()
				<< ", protocol: 0x" << intf_protocol.value()
				<< std::dec << std::endl;
	if(intf_class.value() != protocols::usb::usb_class::mass_storage
			|| intf_subclass.value() != 0x06
			|| intf_protocol.value() != 0x50)
		co_return;

	if(logDebug)
		std::cout << "block-usb: Detected USB device" << std::endl;

	auto storage_device = new StorageDevice(device, entity.id());
	co_await storage_device->initialize(config_number.value(), intf_number.value());

	auto probeResult = co_await storage_device->probe();
	if(!probeResult) {
		std::cout << "block-usb: mbus ID " << entity.id()
				<< ": Failed to probe device" << std::endl;
		delete storage_device;
		co_return;
	}

	blockfs::runDevice(storage_device);
}

async::detached observeDevices() {
	Cmdline cmdlineHelper{};
	auto cmdline = co_await cmdlineHelper.get();
	frg::array args = {
		frg::option{"block-usb.debug", frg::store_true(logDebug)},
		frg::option{"block-usb.trace", frg::store_true(logTrace)},
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


