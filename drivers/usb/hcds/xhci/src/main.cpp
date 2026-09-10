
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <optional>
#include <memory>
#include <bit>
#include <format>
#include <print>

#include <arch/dma_pool.hpp>
#include <core/cmdline.hpp>
#include <frg/bitops.hpp>
#include <frg/cmdline.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/hub.hpp>
#include <protocols/usb/server.hpp>

#include <helix/memory.hpp>

#include "spec.hpp"
#include "context.hpp"
#include "xhci.hpp"
#include "trb.hpp"

namespace proto = protocols::usb;

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

std::vector<std::shared_ptr<Controller>> globalControllers;

namespace {

// Set by xhci.debug on the kernel command line.
bool debugLogs = false;
// Cleared by xhci.no-power-cycle on the kernel command line.
bool powerCyclePorts = true;
// Set by xhci.warm-reset on the kernel command line.
bool warmResetPorts = false;

// Number of address bits that the controller can use for DMA.
size_t dmaAddressBits([[maybe_unused]] arch::mem_space space) {
#ifdef __aarch64__
	// TODO: When we have a way to query the platform this could be restricted to RPI4 only.
	return 31;
#else
	return (space.load(cap_regs::hccparams1) & hccparams1::ac64) ? 64 : 32;
#endif
}

} // namespace

Controller::Controller(
    protocols::hw::Device hw_device,
    mbus_ng::Entity entity,
    helix::Mapping mapping,
    helix::UniqueDescriptor mmio,
    helix::UniqueIrq irq,
    std::string name,
    bool iommuActive,
    helix::UniqueDescriptor dmaSpaceHandle
)
: _hw_device{std::move(hw_device)},
  _mapping{std::move(mapping)},
  _mmio{std::move(mmio)},
  _irq{std::move(irq)},
  _space{_mapping.get()},
  _name{name},
  _memoryPool{&_dmaRealm, {.addressBits = dmaAddressBits(_space)}},
  _dmaSpaceHandle{std::move(dmaSpaceHandle)},
  _dmaSpace{_dmaRealm.attachDmaSpace(_dmaSpaceHandle, iommuActive)},
  _dcbaa{&_memoryPool, 256},
  _cmdRing{this},
  _eventRing{this},
  _enumerator{this},
  _largeCtx{false},
  _entity{std::move(entity)} {
	auto doorbell_offset = _space.load(cap_regs::dboff);
	_doorbells = _space.subspace(doorbell_offset);

	_numPorts = _space.load(cap_regs::hcsparams1) & hcsparams1::maxPorts;
	_ports.resize(_numPorts);
	std::println("{} {} ports in total, port power control: {}", this, _numPorts,
			bool(_space.load(cap_regs::hccparams1) & hccparams1::portPowerControl));
}

async::result<void> Controller::_biosHandoff(uint32_t cap) {
	arch::bit_register<uint32_t> sts{cap + 0x00};
	arch::bit_register<uint32_t> ctl{cap + 0x04};

	auto val = _space.load(sts);

	if (val & handoff_sts::hcBiosOwned) {
		_space.store(sts, val | handoff_sts::hcOsOwned(true));

		auto checkOwned = [&] { return !(_space.load(sts) & handoff_sts::hcBiosOwned); };
		if (!co_await helix::kindaBusyWait(1'000'000'000, checkOwned)) {
			std::println("{} BIOS handoff failed. Handoff status={:08x}", this,
					static_cast<uint32_t>(_space.load(sts)));

			_space.store(sts, val / handoff_sts::hcBiosOwned(false));
		}
	}

	val = _space.load(ctl);

	// Clear enable bits
	val /= handoff_ctl::smiEnable(false);
	val /= handoff_ctl::smiHseEnable(false);
	val /= handoff_ctl::smiOsOwnerEnable(false);
	val /= handoff_ctl::smiPciCmdEnable(false);
	val /= handoff_ctl::smiBarEnable(false);

	// Clear RW1C bits
	val |= handoff_ctl::smiOsOwner(true);
	val |= handoff_ctl::smiPciCmd(true);
	val |= handoff_ctl::smiBar(true);

	_space.store(ctl, val);

	std::println("{} Controller ownership obtained from BIOS", this);
}

async::result<void> Controller::_processExtendedCapabilities() {
	auto cur = (_space.load(cap_regs::hccparams1) & hccparams1::extCapPtr) * 4u;
	if(!cur)
		co_return;

	while(1) {
		auto val = arch::scalar_load<uint32_t>(_space, cur);
		if(val == 0xFFFFFFFF)
			break;

		auto id = val & 0xFF;
		if(!id)
			break;

		if(id == 1) {
			std::println("{} USB Legacy Support capability at {}", this, cur);
			co_await _biosHandoff(cur);
		} else if(id == 2) {
			SupportedProtocol proto;

			auto v = arch::scalar_load<uint32_t>(_space, cur);
			proto.major = (v >> 24) & 0xFF;
			proto.minor = (v >> 16) & 0xFF;

			// some broken controllers incorrectly set this BCD-coded value
			if (proto.minor == 0x01)
				proto.minor = 0x10;

			v = arch::scalar_load<uint32_t>(_space, cur + 8);
			proto.compatiblePortStart = v & 0xFF;
			proto.compatiblePortCount = (v >> 8) & 0xFF;

			v = arch::scalar_load<uint32_t>(_space, cur + 12);
			proto.slotType = v & 0xF;

			_supportedProtocols.push_back(proto);
		}

		auto next = cur + (((val >> 8) & 0xFF) << 2);
		if (next == cur)
			break;

		cur = next;
	}
}

async::detached Controller::initialize() {
	auto opOffset = _space.load(cap_regs::caplength);
	auto operational = _space.subspace(opOffset);

	co_await _processExtendedCapabilities();

	std::println("{} Initializing controller", this);

	// Stop the controller and wait for it to halt
	operational.store(op_regs::usbcmd, usbcmd::run(false));

	auto checkHalted = [&] { return operational.load(op_regs::usbsts) & usbsts::hcHalted; };
	if (!co_await helix::kindaBusyWait(5'000'000'000, checkHalted)) {
		std::println("{} Controller did not halt after 5s! USBSTS={:08x}", this,
				static_cast<uint32_t>(operational.load(op_regs::usbsts)));
		co_return;
	}

	// Reset the controller and wait for it to complete
	operational.store(op_regs::usbcmd, usbcmd::hcReset(1));

	auto checkReset = [&] { return !(operational.load(op_regs::usbcmd) & usbcmd::hcReset); };
	if (!co_await helix::kindaBusyWait(5'000'000'000, checkReset)) {
		std::println("{} Controller not come out of reset after 5s! USBCMD={:08x}", this,
				static_cast<uint32_t>(operational.load(op_regs::usbcmd)));
		co_return;
	}

	auto checkReady = [&] { return !(operational.load(op_regs::usbsts) & usbsts::controllerNotReady); };
	if (!co_await helix::kindaBusyWait(5'000'000'000, checkReady)) {
		std::println("{} Controller not ready after 5s! USBSTS={:08x}", this,
				static_cast<uint32_t>(operational.load(op_regs::usbsts)));
		co_return;
	}

	std::println("{} Controller reset done", this);

	_largeCtx = _space.load(cap_regs::hccparams1) & hccparams1::contextSize;

	_maxDeviceSlots = _space.load(cap_regs::hcsparams1) & hcsparams1::maxDevSlots;
	operational.store(op_regs::config, config::enabledDeviceSlots(_maxDeviceSlots));

	// Figure out how many scratchpad buffers are needed
	auto nScratchpadBufs =
		(uint32_t{_space.load(cap_regs::hcsparams2) & hcsparams2::maxScratchpadBufsHi} << 5)
		| (uint32_t{_space.load(cap_regs::hcsparams2) & hcsparams2::maxScratchpadBufsLow});
	std::println("{} Controller wants {} scratchpad buffers", this, nScratchpadBufs);

	// Pick the smallest supported page size
	// XXX(qookie): Linux seems to not care and always uses 4K
	// pages? I can't find anything in the spec that justifies
	// doing that...
	auto pageSize = 1u << ((std::countr_zero(operational.load(op_regs::pagesize))) + 12);
	std::println("{} Controller's minimum page size is {}", this, pageSize);

	// We need to force a cache flush even on cache coherent platforms since
	// the scratchpad buffer array and the scratchpad buffers use no-snoop transactions.
	arch::dma_barrier forcedBarrier{false};

	// Allocate the scratchpad buffers
	_scratchpadBufArray = arch::dma_array<uint64_t, 64>{&_memoryPool, nScratchpadBufs};
	for (size_t i = 0; i < nScratchpadBufs; i++) {
		_scratchpadBufs.push_back(arch::dma_buffer(&_memoryPool,
					pageSize, pageSize));
		memset(_scratchpadBufs.back().data(), 0, pageSize);

		forcedBarrier.writeback(_scratchpadBufs.back());
		co_await _dmaSpace.ensure_mapped(_scratchpadBufs.back());
		_scratchpadBufArray[i] = _dmaSpace.iova_of(_scratchpadBufs.back());
	}
	forcedBarrier.writeback(_scratchpadBufArray.view_buffer());

	// Initialize the device context pointer array
	for (size_t i = 0; i < 256; i++)
		_dcbaa[i] = 0;
	if(nScratchpadBufs)
		co_await _dmaSpace.ensure_mapped(_scratchpadBufArray);
	_dcbaa[0] = nScratchpadBufs ? _dmaSpace.iova_of(_scratchpadBufArray) : 0;
	barrier.writeback(_dcbaa.view_buffer());

	// Tell the controller about our device context pointer array
	co_await _dmaSpace.ensure_mapped(_dcbaa);
	auto dcbaaPtr = _dmaSpace.iova_of(_dcbaa);
	operational.store(op_regs::dcbaapLow, dcbaaPtr & 0xFFFFFFFF);
	operational.store(op_regs::dcbaapHi, dcbaaPtr >> 32);

	// Tell the controller about our command ring
	co_await _cmdRing.initialize();
	auto cmdRingPtr = _cmdRing.getPtr() | 1;
	operational.store(op_regs::crcrLow, cmdRingPtr & 0xFFFFFFFF);
	operational.store(op_regs::crcrHi, cmdRingPtr >> 32);

	// Set up interrupters
	// TODO(qookie): MSIs let us use multiple interrupters to
	// spread out the load (we probably want up to 1 per core?)
	auto runtimeOffset = _space.load(cap_regs::rtsoff);
	auto runtime = _space.subspace(runtimeOffset);
	_interrupters.push_back(std::make_unique<Interrupter>(
				&_eventRing,
				interrupter::interrupterSpace(runtime, 0)));
	_interrupters.back()->handleIrqs(_irq);
	co_await _interrupters.back()->initialize();

	// Start the controller and enable interrupts
	operational.store(op_regs::usbcmd, usbcmd::run(1) | usbcmd::intrEnable(1));

	// Wait for the controller to start
	auto checkRunning = [&] { return !(operational.load(op_regs::usbsts) & usbsts::hcHalted); };
	if (!co_await helix::kindaBusyWait(5'000'000'000, checkRunning)) {
		std::println("{} Controller did not start after 5s! USBSTS={:08x}", this,
				static_cast<uint32_t>(operational.load(op_regs::usbsts)));
		co_return;
	}

	// Set up root hubs for each protocol
	for (auto &p : _supportedProtocols) {
		std::println("{} USB {:x}.{:02x}: {} ports ({}-{}), slot type {}",
				this, p.major, p.minor, p.compatiblePortCount,
				p.compatiblePortStart, p.compatiblePortStart + p.compatiblePortCount - 1,
				p.slotType);

		mbus_ng::Properties descriptor{
			{"generic.devtype", mbus_ng::StringItem{"usb-controller"}},
			{"generic.devsubtype", mbus_ng::StringItem{"xhci"}},
			{"usb.version.major", mbus_ng::StringItem{std::to_string(p.major)}},
			{"usb.version.minor", mbus_ng::StringItem{std::to_string(p.minor)}},
			{"usb.root.parent", mbus_ng::StringItem{std::to_string(_entity.id())}},
		};

		auto xhciEntity = (co_await mbus_ng::Instance::global().createEntity(
					"xhci-controller", descriptor)).unwrap();

		auto hub = std::make_shared<RootHub>(
			this, p,
			op_regs::portSpace(operational, p.compatiblePortStart - 1),
			std::move(xhciEntity));
		_rootHubs.push_back(hub);
		_enumerator.observeHub(hub);
	}

	std::println("{} Initialization done", this);
}

void Controller::ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id) {
	arch::scalar_store<uint32_t>(_doorbells, doorbell * 4,
			target | (stream_id << 16));
}


async::result<frg::expected<proto::UsbError>>
Controller::enumerateDevice(std::shared_ptr<proto::Hub> parentHub, int port, proto::DeviceSpeed speed) {
	uint32_t route = 0;

	std::shared_ptr<proto::Hub> curHub = parentHub;
	int curPort = port;
	while (curHub->parent()) {
		route <<= 4;
		route |= curPort > 15 ? 15 : curPort;

		curPort = curHub->port();
		curHub = curHub->parent();
	}

	SupportedProtocol *proto = std::static_pointer_cast<RootHub>(curHub)->protocol();

	auto rootPort = curPort + proto->compatiblePortStart - 1;

	auto device = std::make_shared<Device>(this);
	FRG_CO_TRY(co_await device->enumerate(rootPort, port, route, parentHub, speed, proto->slotType));
	_devices[device->slot()] = device;

	// If this is full speed, our guess for MPS might be wrong,
	// get the first 8 bytes of the device descriptor to check.
	if (speed == proto::DeviceSpeed::fullSpeed) {
		arch::dma_object<proto::DeviceDescriptor> descriptor{&_memoryPool};
		FRG_CO_TRY(co_await device->readDescriptor(descriptor.view_buffer().subview(0, 8), 0x0100));

		std::println("{} Full-speed device on port {} has bMaxPacketSize0 = {}",
				this, port, int{descriptor->maxPacketSize});

		FRG_CO_TRY(co_await device->updateEp0PacketSize(descriptor->maxPacketSize));
	}

	arch::dma_object<proto::DeviceDescriptor> descriptor{&_memoryPool};
	FRG_CO_TRY(co_await device->readDescriptor(descriptor.view_buffer(), 0x0100));

	std::println("{} Slot {} on root port {}: {:04x}:{:04x}, class {:02x}/{:02x}/{:02x}, bcdUSB {:x}.{:02x}, {} Mbps",
			this, device->slot(), rootPort, descriptor->idVendor, descriptor->idProduct,
			descriptor->deviceClass, descriptor->deviceSubclass, descriptor->deviceProtocol,
			descriptor->bcdUsb >> 8, descriptor->bcdUsb & 0xFF, protocols::usb::getSpeedMbps(speed));

	arch::dma_object<proto::ConfigDescriptor> configDescriptor{&_memoryPool};
	FRG_CO_TRY(co_await device->readDescriptor(configDescriptor.view_buffer(), 0x0200));
	FRG_CO_TRY(co_await device->useConfiguration(0, configDescriptor->configValue));

	// Advertise the USB device on mbus.
	auto classCode = std::format("{:02x}", descriptor->deviceClass);
	auto subClass = std::format("{:02x}", descriptor->deviceSubclass);
	auto protocol = std::format("{:02x}", descriptor->deviceProtocol);
	auto vendor = std::format("{:04x}", descriptor->idVendor);
	auto product = std::format("{:04x}", descriptor->idProduct);
	auto release = std::format("{:04x}", descriptor->bcdDevice);

	if (descriptor->deviceClass == 0x09 && descriptor->deviceSubclass == 0) {
		auto hub = FRG_CO_TRY(co_await createHubFromDevice(parentHub, proto::Device{device}, port));

		FRG_CO_TRY(co_await device->configureHub(hub, speed));

		_enumerator.observeHub(std::move(hub));
	}

	auto name = std::format("{:02x}", device->slot());

	std::string mbps = protocols::usb::getSpeedMbps(speed);

	auto entity_id = std::static_pointer_cast<RootHub>(curHub)->entityId();

	mbus_ng::Properties mbusDescriptor{
		{"usb.type", mbus_ng::StringItem{"device"}},
		{"usb.vendor", mbus_ng::StringItem{vendor}},
		{"usb.product", mbus_ng::StringItem{product}},
		{"usb.class", mbus_ng::StringItem{classCode}},
		{"usb.subclass", mbus_ng::StringItem{subClass}},
		{"usb.protocol", mbus_ng::StringItem{protocol}},
		{"usb.release", mbus_ng::StringItem{release}},
		{"usb.hub_port", mbus_ng::StringItem{name}},
		{"usb.bus", mbus_ng::StringItem{std::to_string(entity_id)}},
		{"usb.speed", mbus_ng::StringItem{mbps}},
		{"unix.subsystem", mbus_ng::StringItem{"usb"}},
	};

	auto usbEntity = (co_await mbus_ng::Instance::global().createEntity(
				"usb-xhci-dev-" + std::string{name}, mbusDescriptor)).unwrap();

	[] (auto device, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			proto::serve(proto::Device{device}, std::move(localLane));
		}
	}(device, std::move(usbEntity));

	co_return frg::success;
}

void Controller::processEvent(Event ev) {
	switch (ev.type) {
		using enum TrbType;

		case commandCompletionEvent:
			_cmdRing.processEvent(ev);
			break;

		case transferEvent:
			if (!_devices[ev.slotId]) {
				std::println("{} Transfer event for unexpected slot {}", this, ev.slotId);
				break;
			}
			if (ev.endpointId == 0 || ev.endpointId > 31) {
				std::println("{} Transfer event for unexpected endpoint ID {} on slot {}", this, ev.endpointId, ev.slotId);
				break;
			}

			if (auto ep = _devices[ev.slotId]->endpoint(ev.endpointId))
				ep->transferRing().processEvent(ev);
			else
				std::println("{} Event for missing endpoint ID {} on slot {}", this, ev.endpointId, ev.slotId);
			break;

		case portStatusChangeEvent:
			if (ev.portId == 0 || ev.portId > _ports.size()) {
				std::println("{} Port event for unexpected port {}", this, ev.portId);
				break;
			}

			if (_ports[ev.portId - 1]) {
				if (debugLogs)
					_ports[ev.portId - 1]->logPortsc("status change");
				_ports[ev.portId - 1]->_doorbell.raise();
			}
			break;

		default:
			std::println("{} Unexpected event in processEvent, ignoring...", this);
			ev.printInfo();
	}
}

void Controller::dumpState() {
	auto operational = _space.subspace(_space.load(cap_regs::caplength));
	std::println("{} USBSTS={:#x}", this, static_cast<uint32_t>(operational.load(op_regs::usbsts)));
	for (auto &interrupter : _interrupters)
		interrupter->dump(this);
}

async::result<frg::expected<proto::UsbError, uint32_t>>
Controller::enableSlot(uint8_t slotType) {
	auto event = co_await submitCommand(Command::enableSlot(slotType));

	if (event.completionCode != CompletionCode::success)
		std::println("{} enableSlot({:d}) failed: {}",
				this, slotType, event.completionCodeName());

	FRG_CO_TRY(completionToError(event));
	co_return event.slotId;
}

async::result<frg::expected<proto::UsbError>>
Controller::addressDevice(uint32_t slotId, InputContext &ctx) {
	barrier.writeback(ctx.data(), ctx.rawSize());
	auto event = co_await submitCommand(
			Command::addressDevice(slotId, co_await ctx.iova(_dmaSpace)));

	if (event.completionCode != CompletionCode::success)
		std::println("{} addressDevice({}, ctx) failed: {}",
				this, slotId, event.completionCodeName());

	FRG_CO_TRY(completionToError(event));
	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Controller::configureEndpoint(uint32_t slotId, InputContext &ctx) {
	barrier.writeback(ctx.data(), ctx.rawSize());
	auto event = co_await submitCommand(
			Command::configureEndpoint(slotId, co_await ctx.iova(_dmaSpace)));

	if (event.completionCode != CompletionCode::success)
		std::println("{} configureEndpoint({}, ctx) failed: {}",
				this, slotId, event.completionCodeName());

	FRG_CO_TRY(completionToError(event));
	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Controller::evaluateContext(uint32_t slotId, InputContext &ctx) {
	barrier.writeback(ctx.data(), ctx.rawSize());
	auto event = co_await submitCommand(
			Command::evaluateContext(slotId, co_await ctx.iova(_dmaSpace)));

	if (event.completionCode != CompletionCode::success)
		std::println("{} evaluateContext({}, ctx) failed: {}",
				this, slotId, event.completionCodeName());

	FRG_CO_TRY(completionToError(event));
	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Controller::resetEndpoint(uint32_t slotId, uint32_t endpointId) {
	auto event = co_await submitCommand(Command::resetEndpoint(slotId, endpointId));

	if (event.completionCode != CompletionCode::success)
		std::println("{} resetEndpoint({}, {}) failed: {}",
				this, slotId, endpointId, event.completionCodeName());

	FRG_CO_TRY(completionToError(event));
	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Controller::setTransferRingDequeue(uint32_t slotId, uint32_t endpointId, ProducerRing &ring, RingPointer pointer) {
	uint64_t dequeue = (ring.getPtr() + pointer.index * sizeof(RawTrb)) | pointer.cycle;

	auto event = co_await submitCommand(
		Command::setTransferRingDequeue(slotId, endpointId, dequeue));

	if (event.completionCode != CompletionCode::success)
		std::println("{} setTransferRingDequeue({}, {}, ring, {{{}, {}}}) (dequeue = {:#x}) failed: {}",
				this, slotId, endpointId, pointer.index, pointer.cycle, dequeue, event.completionCodeName());

	FRG_CO_TRY(completionToError(event));
	co_return frg::success;
}

// ------------------------------------------------------------------------
// Interrutper
// ------------------------------------------------------------------------

async::result<void> Interrupter::initialize() {
	co_await _ring->init();

	// Initialize the event ring segment table
	_space.store(interrupter::erstsz, _ring->getErstSize());
	auto erstPtr = _ring->getErstPtr();
	_space.store(interrupter::erstbaLow, erstPtr & 0xFFFFFFFF);
	_space.store(interrupter::erstbaHi, erstPtr >> 32);

	_updateDequeue();

	_space.store(interrupter::iman, _space.load(interrupter::iman) | iman::enable(1));
}

async::detached Interrupter::handleIrqs(helix::UniqueIrq &irq) {
	uint64_t sequence = 0;

	while(1) {
		auto await = co_await helix_ng::awaitEvent(irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		if (!_isBusy()) {
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckNack, sequence));
			continue;
		}

		_clearPending();
		HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, sequence));

		_ring->processRing();
		_updateDequeue();
	}
}

async::detached Interrupter::pollIrqs() {
	while(1) {
		co_await helix::sleepFor(1'000'000);

		if (!_isBusy()) {
			continue;
		}

		_clearPending();
		_ring->processRing();
		_updateDequeue();
	}
}

void Interrupter::_updateDequeue() {
	auto eventRingPtr = _ring->getEventRingPtr();
	_space.store(interrupter::erdpLow, (eventRingPtr & 0xFFFFFFF0) | (1 << 3));
	_space.store(interrupter::erdpHi, eventRingPtr >> 32);
}

bool Interrupter::_isBusy() {
	return _space.load(interrupter::erdpLow) & (1 << 3);
}

void Interrupter::_clearPending() {
	_space.store(interrupter::iman, _space.load(interrupter::iman) | iman::pending(1));
}

void Interrupter::dump(Controller *controller) {
	std::println("{} IMAN={:#x}, ERDP={:#x}, unprocessed event: {}",
			controller, static_cast<uint32_t>(_space.load(interrupter::iman)),
			_space.load(interrupter::erdpLow), _ring->hasPendingEvent());
}

// ------------------------------------------------------------------------
// Controller::Port
// ------------------------------------------------------------------------

Controller::Port::Port(int id, arch::mem_space space, Controller *controller, SupportedProtocol *proto)
: _id{id}, _controller{controller}, _proto{proto}, _space{space} {
}

void Controller::Port::reset() {
	std::println("{} Resetting port {}", _controller, _id);
	_space.store(port::portsc, portsc::portPower(true) | portsc::portReset(true));
}

void Controller::Port::disable() {
	_space.store(port::portsc, portsc::portPower(true) | portsc::portEnable(true));
}

void Controller::Port::resetChangeBits() {
	_space.store(port::portsc, portsc::portPower(true)
			| portsc::connectStatusChange(true)
			| portsc::portResetChange(true)
			| portsc::portEnableChange(true)
			| portsc::warmPortResetChange(true)
			| portsc::overCurrentChange(true)
			| portsc::portLinkStatusChange(true)
			| portsc::portConfigErrorChange(true));
}

bool Controller::Port::isConnected() {
	auto portsc = _space.load(port::portsc);
	return portsc & portsc::connectStatus;
}

bool Controller::Port::isPowered() {
	auto portsc = _space.load(port::portsc);
	return portsc & portsc::portPower;
}

bool Controller::Port::isEnabled() {
	return _space.load(port::portsc) & portsc::portEnable;
}

uint8_t Controller::Port::getLinkStatus() {
	return _space.load(port::portsc) & portsc::portLinkStatus;
}

uint8_t Controller::Port::getSpeed() {
	return _space.load(port::portsc) & portsc::portSpeed;
}

void Controller::Port::logPortsc(std::string_view what) {
	auto sc = _space.load(port::portsc);
	std::println("{} USB {:x}.{:02x} port {} {}: PORTSC={:#010x} (CCS {:d}, PED {:d}, PR {:d}, PLS {:d}, speed {:d}, changes {:#04x})",
			_controller, _proto->major, _proto->minor, _id, what, static_cast<uint32_t>(sc),
			bool(sc & portsc::connectStatus), bool(sc & portsc::portEnable),
			bool(sc & portsc::portReset), sc & portsc::portLinkStatus, sc & portsc::portSpeed,
			(static_cast<uint32_t>(sc) >> 17) & 0x7F);
}

void Controller::Port::transitionToLinkStatus(uint8_t status) {
	_space.store(port::portsc, portsc::portPower(true)
			| portsc::portLinkStatus(status)
			| portsc::portLinkStatusStrobe(true));
}

async::result<bool> Controller::Port::recoverLink() {
	if (!warmResetPorts || _proto->major != 3)
		co_return false;

	// SS.Inactive (6) and Compliance (10) can only be left through a Warm Reset (xHCI 4.19.1.2.6/7).
	auto linkStatus = getLinkStatus();
	if (linkStatus != 6 && linkStatus != 10)
		co_return false;

	logPortsc("issuing warm reset");
	_space.store(port::portsc, portsc::portPower(true) | portsc::warmPortReset(true));
	for (int i = 0; i < 20 && (_space.load(port::portsc) & portsc::portReset); i++)
		co_await helix_ng::sleepFor(50'000'000);
	logPortsc("after warm reset");
	co_return true;
}

async::detached Controller::Port::initPort() {
	if (powerCyclePorts) {
		_space.store(port::portsc, portsc::portPower(false));
		co_await helix_ng::sleepFor(1'000'000'000);
	}

	_space.store(port::portsc, portsc::portPower(true));

	// Linux only waits for power good (20 ms for USB 2, 100 ms for USB 3 root hubs).
	co_await helix_ng::sleepFor(powerCyclePorts ? 1'000'000'000 : 100'000'000);

	// Ports can end up in states that do not generate events (e.g., Compliance), so take a snapshot.
	if (debugLogs) {
		[] (Port *self) -> async::detached {
			co_await helix_ng::sleepFor(3'000'000'000);
			self->logPortsc("3 s after power-on");
		}(this);
	}

	// Wait for something to connect to the port.
	// Entering Compliance does not generate an event, hence we also check the link on each timeout.
	int warmResets = 0;
	while (true) {
		resetChangeBits();
		if (isConnected())
			break;
		if (warmResets < 3 && co_await recoverLink()) {
			warmResets++;
			continue;
		}

		async::cancellation_event ev;
		helix::TimeoutCancellation tc{1'000'000'000, ev};

		co_await _doorbell.async_wait(ev);
		co_await tc.retire();
	}
	if (debugLogs)
		logPortsc("connected");

	// Notify the enumerator
	_state.changes |= proto::HubStatus::connect;
	_state.status |= proto::HubStatus::connect;
	_pollEv.raise();
}

async::result<proto::PortState> Controller::Port::pollState() {
	_pollSeq = co_await _pollEv.async_wait(_pollSeq);
	co_return _state;
}

async::result<frg::expected<proto::UsbError, void>> Controller::Port::issueReset() {
	// We know something is connected if we're here (CCS=1)

	// Reset the port only for USB 2 devices.
	// "A USB3 protocol port attempts to automatically advance to
	//  the Enabled state as part of the attach process."
	// "A USB2 protocol port requires software to reset the port
	//  to advance the port to the Enabled state [...]"
	if (_proto->major == 2) {
		reset();
	}

	// Wait for the port to enable. Give up (like Linux) if the device disconnects or the port
	// does not enable in time, since the enumerator blocks all other ports of this controller.
	int timeouts = 0;
	int warmResets = 0;
	while (true) {
		resetChangeBits();
		auto sc = _space.load(port::portsc);
		if (sc & portsc::portEnable)
			break;
		if (!(sc & portsc::connectStatus)) {
			logPortsc("lost the device while waiting for port enable");
			co_return proto::UsbError::other;
		}
		if (warmResets < 3 && co_await recoverLink()) {
			warmResets++;
			continue;
		}
		if (timeouts == 5) {
			logPortsc("did not enable, giving up");
			co_return proto::UsbError::timeout;
		}

		async::cancellation_event ev;
		helix::TimeoutCancellation tc{1'000'000'000, ev};

		bool raised = co_await _doorbell.async_wait(ev);
		co_await tc.retire();

		if (!raised)
			logPortsc(std::format("still waiting for port enable after {} s", ++timeouts));
	}

	auto linkStatus = getLinkStatus();

	if (debugLogs)
		logPortsc("enabled");

	if (linkStatus >= 1 && linkStatus <= 3) {
		transitionToLinkStatus(0);
	} else {
		assert(linkStatus == 0); // U0
	}

	// Notify the enumerator.
	_state.changes |= proto::HubStatus::enable;
	_state.status |= proto::HubStatus::enable;
	_pollEv.raise();

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> Controller::Port::querySpeed() {
	uint8_t speedId = getSpeed();

	std::optional<proto::DeviceSpeed> speed;

	switch(speedId) {
		case 1:
			speed = proto::DeviceSpeed::fullSpeed;
			break;
		case 2:
			speed = proto::DeviceSpeed::lowSpeed;
			break;
		case 3:
			speed = proto::DeviceSpeed::highSpeed;
			break;
		case 4:
		case 5:
		case 6:
		case 7:
			speed = proto::DeviceSpeed::superSpeed;
			break;
		default:
			break;
	}

	if (speed) {
		co_return speed.value();
	} else {
		std::println("{} Port {} has invalid speed ID {:d}", _controller, _id, speedId);
		co_return proto::UsbError::unsupported;
	}
}

// ------------------------------------------------------------------------
// Controller::RootHub
// ------------------------------------------------------------------------

Controller::RootHub::RootHub(Controller *controller, SupportedProtocol &proto, arch::mem_space portSpace, mbus_ng::EntityManager entity)
: Hub{nullptr, 0}, _controller{controller}, _proto{&proto}, _entity{std::move(entity)} {
	for (size_t i = 0; i < proto.compatiblePortCount; i++) {
		_ports.push_back(std::make_unique<Port>(
					i + proto.compatiblePortStart,
					port::spaceForIndex(portSpace, i),
					controller, &proto));
		_ports.back()->initPort();
		_controller->_ports[(i + proto.compatiblePortStart - 1)] =
				_ports.back().get();
	}
}

size_t Controller::RootHub::numPorts() {
	return _proto->compatiblePortCount;
}

async::result<proto::PortState> Controller::RootHub::pollState(int port) {
	co_return co_await _ports[port - 1]->pollState();
}

async::result<frg::expected<proto::UsbError, void>> Controller::RootHub::issueReset(int port) {
        FRG_CO_TRY(co_await _ports[port - 1]->issueReset());
	co_return frg::success;
}

async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> Controller::RootHub::querySpeed(int port) {
	co_return FRG_CO_TRY(co_await _ports[port - 1]->querySpeed());
}

// ------------------------------------------------------------------------
// Device
// ------------------------------------------------------------------------

Device::Device(Controller *controller)
: _slotId{-1}, _controller{controller} {
}

arch::dma_pool *Device::setupPool() {
	return _controller->memoryPool();
}

arch::dma_pool *Device::bufferPool() {
	return _controller->memoryPool();
}

async::result<frg::expected<proto::UsbError, std::string>>
Device::deviceDescriptor() {
	arch::dma_object<proto::DeviceDescriptor> descriptor{bufferPool()};
	FRG_CO_TRY(co_await readDescriptor(descriptor.view_buffer(), 0x0100));
	co_return std::string{(char *)descriptor.data(), descriptor.view_buffer().size()};
}

async::result<frg::expected<proto::UsbError, std::string>>
Device::configurationDescriptor(uint8_t configuration) {
	arch::dma_object<proto::ConfigDescriptor> header{bufferPool()};
	FRG_CO_TRY(co_await readDescriptor(header.view_buffer(), 0x0200 | configuration));

	arch::dma_buffer descriptor{bufferPool(), header->totalLength};
	FRG_CO_TRY(co_await readDescriptor(descriptor, 0x0200 | configuration));
	co_return std::string{(char *)descriptor.data(), descriptor.size()};
}

namespace {

int bIntervalIntoMicroframes(proto::DeviceSpeed speed, proto::EndpointType type, uint8_t bInterval) {
	using enum proto::DeviceSpeed;
	using enum proto::EndpointType;

	auto framesToExponent = [] (int interval, int min, int max) {
		int exponent = frg::floor_log2(interval);
		return std::clamp(exponent, min, max);
	};

	auto asMicroframes = [&] {
		if (bInterval == 0)
			return 0;
		return framesToExponent(bInterval, 0, 15);
	};

	auto asExponent = [&] {
		return std::clamp(int{bInterval}, 1, 16) - 1;
	};

	auto asFrames = [&] {
		return framesToExponent(bInterval * 8, 3, 10);
	};

	switch (type) {
		case control:
		case bulk:
			if (speed == highSpeed) {
				return asMicroframes(); // Max NAK rate
			}
			return 0;
		case isochronous:
			if (speed == fullSpeed) {
				// + 3 => * 2**3 to turn frames into microframes
				return asExponent() + 3;
			}
			[[fallthrough]];
		case interrupt:
			if (speed == superSpeed || speed == highSpeed) {
				return asExponent();
			}

			assert((speed == fullSpeed && type == interrupt) || speed == lowSpeed);
			return asFrames();
	}

	return 0;
}

} // namespace anonymous

async::result<frg::expected<proto::UsbError, proto::Configuration>>
Device::useConfiguration(uint8_t index, uint8_t value) {
	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor(index));
	auto cfg = proto::configurationRange(descriptor);

	auto configDesc = proto::configDescriptorFrom(cfg);
	if(!configDesc) {
		std::println("{} useConfiguration({:d}, {:d}) called, but that configuration has no valid configuration descriptor",
				_controller, index, value);
		co_return proto::UsbError::other;
	}
	// Bail out if the user has no idea what they're asking for
	if (configDesc->configValue != value) {
		std::println("{} useConfiguration({:d}, {:d}) called, but that configuration has bConfigurationValue = {:d} ???",
				_controller, index, value, configDesc->configValue);
		co_return proto::UsbError::other;
	}

	arch::dma_object<proto::SetupPacket> setConfig{setupPool()};
	setConfig->type = proto::setup_type::targetDevice | proto::setup_type::byStandard | proto::setup_type::toDevice;
	setConfig->request = proto::request_type::setConfig;
	setConfig->value = value;
	setConfig->index = 0;
	setConfig->length = 0;

	FRG_CO_TRY(co_await transfer({protocols::usb::kXferToDevice, setConfig, {}}));

	std::println("{} Slot {}: configuration {} set", _controller, _slotId, value);

	co_return proto::Configuration{std::make_shared<ConfigurationState>(shared_from_this(), index)};
}

async::result<frg::expected<proto::UsbError, size_t>>
Device::transfer(proto::ControlTransfer info) {
	co_return co_await _endpoints[0]->transfer(info);
}

void Device::submit(int endpoint) {
	assert(_slotId != -1);
	_controller->ringDoorbell(_slotId, endpoint);
}

static inline uint8_t getHcdSpeedId(proto::DeviceSpeed speed) {
	switch (speed) {
		using enum proto::DeviceSpeed;

		case lowSpeed: return 2;
		case fullSpeed: return 1;
		case highSpeed: return 3;
		case superSpeed: return 4;
	}

	return 0;
}

async::result<frg::expected<proto::UsbError>>
Device::enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed, int slotType) {
	_slotId = FRG_CO_TRY(co_await _controller->enableSlot(slotType));

	std::println("{} Slot {} allocated for port {} (route {:x})", _controller, _slotId, port, route);

	// Initialize slot context

	_devCtx = DeviceContext{_controller->largeCtx(), _controller->memoryPool()};
	co_await _devCtx.initialize(_controller->dmaSpace());
	_controller->barrier.writeback(_devCtx.data(), _devCtx.rawSize());

	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};
	auto &slotCtx = inputCtx.get(inputCtxSlot);

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context

	slotCtx |= SlotFields::routeString(route);
	slotCtx |= SlotFields::ctxEntries(1);
	slotCtx |= SlotFields::speed(getHcdSpeedId(speed));

	// For LS/FS devices not on the root hub ...
	if ((speed == proto::DeviceSpeed::lowSpeed || speed == proto::DeviceSpeed::fullSpeed) && hub->parent()) {
		// ... look for a hub with a TT.

		// TODO(qookie): This could probably be tracked by the generic hub code.
		auto curHub = hub;
		while (curHub->parent()) {
			auto hubDevice = std::static_pointer_cast<Device>(
				curHub->associatedDevice()->state());

			if (hubDevice->_speed == proto::DeviceSpeed::highSpeed)
				break;

			assert(hubDevice->_speed == proto::DeviceSpeed::lowSpeed
					|| hubDevice->_speed == proto::DeviceSpeed::fullSpeed);

			curHub = curHub->parent();
		}

		// Non-root high speed hub found in the path.
		if (curHub->parent()) {
			auto hubDevice = std::static_pointer_cast<Device>(
				curHub->associatedDevice()->state());

			assert(hubDevice->_speed == proto::DeviceSpeed::highSpeed);

			// We need to fill these fields out for split transactions.
			slotCtx |= SlotFields::parentHubPort(curHub->port());
			slotCtx |= SlotFields::parentHubSlot(hubDevice->_slotId);
		}
	}

	slotCtx |= SlotFields::rootHubPort(rootPort);

	size_t packetSize = 0;
	switch (speed) {
		using enum proto::DeviceSpeed;

		case lowSpeed:
		case fullSpeed: packetSize = 8; break;
		case highSpeed: packetSize = 64; break;
		case superSpeed: packetSize = 512; break;
	}
	_speed = speed;

	co_await _initEpCtx(inputCtx, 0, proto::PipeType::control, packetSize, proto::EndpointType::control, 0);

	_controller->setDeviceContext(_slotId, _devCtx);

	FRG_CO_TRY(co_await _controller->addressDevice(_slotId, inputCtx));

	std::println("{} Slot {}: device successfully addressed", _controller, _slotId);

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Device::readDescriptor(arch::dma_buffer_view dest, uint16_t desc) {
	arch::dma_object<proto::SetupPacket> getDesc{setupPool()};
	getDesc->type = proto::setup_type::targetDevice | proto::setup_type::byStandard | proto::setup_type::toHost;
	getDesc->request = proto::request_type::getDescriptor;
	getDesc->value = desc;
	getDesc->index = 0;
	getDesc->length = dest.size();

	FRG_CO_TRY(co_await transfer({protocols::usb::kXferToHost, getDesc, dest}));

	co_return frg::success;
}

static inline uint32_t getHcdEndpointType(proto::PipeType dir, proto::EndpointType type) {
	using proto::EndpointType;
	using proto::PipeType;

	if (type == EndpointType::control)
		return 4;
	if (type == EndpointType::isochronous)
		return 1 + (dir == PipeType::in ? 4 : 0);
	if (type == EndpointType::bulk)
		return 2 + (dir == PipeType::in ? 4 : 0);
	if (type == EndpointType::interrupt)
		return 3 + (dir == PipeType::in ? 4 : 0);
	return 0;
}

static inline uint32_t getDefaultAverageTrbLen(proto::EndpointType type) {
	using proto::EndpointType;

	if (type == EndpointType::control)
		return 8;
	if (type == EndpointType::isochronous)
		return 3 * 1024;
	if (type == EndpointType::bulk)
		return 3 * 1024;
	if (type == EndpointType::interrupt)
		return 1 * 1024;
	return 0;
}



async::result<frg::expected<proto::UsbError>>
Device::setupEndpoint(int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type, int interval) {
	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context

	_controller->barrier.invalidate(_devCtx.data(), _devCtx.rawSize());
	inputCtx.get(inputCtxSlot) = _devCtx.get(deviceCtxSlot);
	inputCtx.get(inputCtxSlot) |= SlotFields::ctxEntries(31);

	co_await _initEpCtx(inputCtx, endpoint, dir, maxPacketSize, type, interval);

	FRG_CO_TRY(co_await _controller->configureEndpoint(_slotId, inputCtx));

	std::println("{} Endpoint {} configured", _controller, endpoint);

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Device::configureHub(std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed) {
	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context

	_controller->barrier.invalidate(_devCtx.data(), _devCtx.rawSize());
	inputCtx.get(inputCtxSlot) = _devCtx.get(deviceCtxSlot);

	inputCtx.get(inputCtxSlot) |= SlotFields::hub(true);
	inputCtx.get(inputCtxSlot) |= SlotFields::portCount(hub->numPorts());

	if (speed == proto::DeviceSpeed::highSpeed)
		inputCtx.get(inputCtxSlot) |= SlotFields::ttThinkTime(
				hub->getCharacteristics().unwrap().ttThinkTime / 8 - 1);

	FRG_CO_TRY(co_await _controller->evaluateContext(_slotId, inputCtx));

	std::println("{} Hub setup done", _controller);

	co_return frg::success;
}

async::result<void> Device::_initEpCtx(InputContext &ctx, int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type, int interval) {
	int endpointId = getEndpointIndex(endpoint, dir);

	ctx.get(inputCtxCtrl) |= InputControlFields::add(endpointId); // EP Context

	auto ep = std::make_shared<EndpointState>(this, endpointId, type, maxPacketSize);
	_endpoints[endpointId - 1] = ep;

	co_await ep->transferRing().initialize();
	auto trPtr = ep->transferRing().getPtr();

	auto &epCtx = ctx.get(inputCtxEp0 + endpointId - 1);

	epCtx |= EpFields::errorCount(3);
	epCtx |= EpFields::interval(interval);
	epCtx |= EpFields::epType(getHcdEndpointType(dir, type));
	epCtx |= EpFields::maxPacketSize(maxPacketSize);
	// TODO(qookie): This is fine for USB 2 (unless max burst > 0),
	// but for USB 3 this should use wBytesPerInterval from the SS
	// endpoint companion descriptor.
	epCtx |= EpFields::maxEsitPayloadLo(maxPacketSize);
	epCtx |= EpFields::maxEsitPayloadHi(maxPacketSize);
	epCtx |= EpFields::dequeCycle(true);
	epCtx |= EpFields::trPointerLo(trPtr);
	epCtx |= EpFields::trPointerHi(trPtr);

	// TODO(qookie): We should keep track of the average transfer sizes and
	// update this every once in a while. Currently we just use the recommended
	// initial values from the specification.
	epCtx |= EpFields::averageTrbLength(getDefaultAverageTrbLen(type));
}

async::result<frg::expected<proto::UsbError>>
Device::updateEp0PacketSize(size_t maxPacketSize) {
	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};
	int endpointId = getEndpointIndex(0, proto::PipeType::control);

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(endpointId); // EP Context

	auto &epCtx = inputCtx.get(inputCtxEp0 + endpointId - 1);

	_controller->barrier.invalidate(_devCtx.data(), _devCtx.rawSize());
	epCtx = _devCtx.get(deviceCtxEp0 + endpointId - 1);

	epCtx &= ~EpFields::maxPacketSize(0xFFFF);
	epCtx |= EpFields::maxPacketSize(maxPacketSize);

	epCtx &= ~EpFields::maxEsitPayloadLo(0xFFFFFF);
	epCtx &= ~EpFields::maxEsitPayloadHi(0xFFFFFF);
	epCtx |= EpFields::maxEsitPayloadLo(maxPacketSize);
	epCtx |= EpFields::maxEsitPayloadHi(maxPacketSize);

	_endpoints[endpointId - 1]->_maxPacketSize = maxPacketSize;

	FRG_CO_TRY(co_await _controller->evaluateContext(_slotId, inputCtx));

	co_return frg::success;
}

// ------------------------------------------------------------------------
// ConfigurationState
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, proto::Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	arch::dma_object<proto::SetupPacket> desc{_device->setupPool()};
	desc->type = proto::setup_type::targetInterface | proto::setup_type::byStandard | proto::setup_type::toDevice;
	desc->request = proto::request_type::setInterface;
	desc->value = alternative;
	desc->index = number;
	desc->length = 0;

	struct EndpointInfo {
		int pipe;
		proto::PipeType dir;
		int packetSize;
		proto::EndpointType type;
		int interval;
	};

	std::vector<EndpointInfo> _eps = {};

	auto descriptor = FRG_CO_TRY(co_await _device->configurationDescriptor(_index));
	auto cfg = proto::configurationRange(descriptor);

	for(auto [intf, body] : proto::groupByInterface(cfg)) {
		if(intf.interfaceNumber != number || intf.alternateSetting != alternative)
			continue;

		for(auto ep : proto::endpointsOf(body)) {
			auto packetSize = ep.maxPacketSize & 0x7FF;
			auto epType = static_cast<proto::EndpointType>(ep.attributes & 0x03);
			auto interval = ep.interval;

			int pipe = ep.endpointAddress & 0x0F;
			if (ep.endpointAddress & 0x80) {
				_eps.push_back({pipe, proto::PipeType::in, packetSize, epType, interval});
			} else {
				_eps.push_back({pipe, proto::PipeType::out, packetSize, epType, interval});
			}
		}
	}

	// The device might stall if only the default setting is
	// supported so just ignore that.
	auto res = co_await _device->transfer({proto::kXferToDevice, desc, {}});
	if (!res && res.error() == proto::UsbError::stall) {
		std::println("{} SET_INTERFACE({}, {}) stalled, ignoring...", _device->controller(), number, alternative);
	} else {
		FRG_CO_TRY(res);
	}

	for (auto &ep : _eps) {
		auto interval = bIntervalIntoMicroframes(_device->speed(), ep.type, ep.interval);

		std::println("{} Slot {}: setting up {} endpoint {} (max packet size: {}) (bInterval {} = 2**{} microframes)",
				_device->controller(), _device->slot(), ep.dir == proto::PipeType::in ? "in" : "out", ep.pipe, ep.packetSize,
				ep.interval, interval);

		FRG_CO_TRY(co_await _device->setupEndpoint(ep.pipe, ep.dir, ep.packetSize, ep.type, interval));
	}
	co_return proto::Interface{std::make_shared<InterfaceState>(_device, number)};
}

// ------------------------------------------------------------------------
// EndpointState
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::transfer(proto::ControlTransfer info) {
	auto setup = *info.setup.data();
	auto what = std::format("control request {:02x}/{:02x} (wValue {:04x}, wIndex {:04x}, wLength {})",
			uint32_t{setup.type}, uint32_t{setup.request}, uint32_t{setup.value},
			uint32_t{setup.index}, uint32_t{setup.length});

	auto trbs = co_await Transfer::buildControlChain(
		setup,
		_device->controller()->dmaSpace(),
		info.buffer,
		info.flags == proto::kXferToHost,
		_maxPacketSize);

	co_return FRG_CO_TRY(co_await _postTd(
				std::move(trbs),
				info.buffer,
				info.flags == proto::kXferToHost,
				std::move(what)));
}

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::transfer(proto::InterruptTransfer info) {
	auto trbs = co_await Transfer::buildNormalChain(_device->controller()->dmaSpace(), info.buffer, _maxPacketSize);
	co_return FRG_CO_TRY(co_await _postTd(
				std::move(trbs),
				info.buffer,
				info.flags == proto::kXferToHost,
				std::format("interrupt transfer of {} bytes", info.buffer.size())));
}

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::transfer(proto::BulkTransfer info) {
	// A TD has to fit into the transfer ring (one TRB per page), so split large transfers into multiple
	// TDs. Since the TD size is a multiple of any max packet size, a short packet still ends the transfer.
	constexpr size_t maxTdSize = 64 * 4096;

	size_t progress = 0;
	do {
		auto chunk = info.buffer.subview(progress, std::min(maxTdSize, info.buffer.size() - progress));
		auto trbs = co_await Transfer::buildNormalChain(_device->controller()->dmaSpace(), chunk, _maxPacketSize);
		auto length = FRG_CO_TRY(co_await _postTd(
					std::move(trbs),
					chunk,
					info.flags == proto::kXferToHost,
					std::format("bulk transfer of {} bytes", chunk.size())));
		progress += length;
		if (length < chunk.size())
			break;
	} while (progress < info.buffer.size());

	co_return progress;
}

uint8_t Device::endpointState(int endpointId) {
	_controller->barrier.invalidate(_devCtx.data(), _devCtx.rawSize());
	return _devCtx.get(deviceCtxEp0 + endpointId - 1).val[0] & 7;
}

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::_postTd(std::vector<RawTrb> &&trbs, arch::dma_buffer_view buffer, bool toHost, std::string what) {
	ProducerRing::Transaction tx;

	// Invalidate the buffer before posting the TD in case the ring is already running.
	if (toHost)
		_device->controller()->barrier.clean_or_invalidate(buffer);
	else
		_device->controller()->barrier.writeback(buffer);

	auto nextDequeue = co_await _transferRing.pushTrbs(trbs, &tx);

	{
		co_await _submissionMutex.async_lock();
		frg::unique_lock lock{frg::adopt_lock, _submissionMutex};

		_device->submit(_endpointId);
	}

	// Interrupt transfers may legitimately stay pending indefinitely.
	auto done = std::make_shared<bool>(false);
	if (debugLogs && _type != proto::EndpointType::interrupt) {
		[] (EndpointState *self, std::shared_ptr<bool> done, std::string what) -> async::detached {
			co_await helix_ng::sleepFor(2'000'000'000);
			if (*done)
				co_return;
			std::println("{} Slot {}: {} on endpoint {} still pending after 2 s (endpoint state {})",
					self->_device->controller(), self->_device->slot(), what, self->_endpointId,
					self->_device->endpointState(self->_endpointId));
			self->_device->controller()->dumpState();
		}(this, done, what);
	}

	auto maybeResidue = co_await tx.transfer();
	*done = true;

	if (toHost)
		_device->controller()->barrier.invalidate(buffer);

	if (!maybeResidue && maybeResidue.error() == proto::UsbError::stall) {
		auto res = co_await _resetAfterError(nextDequeue);
		if (!res) {
			std::println("{} Failed to reset EP {} after stall: {}",
					_device->controller(), _endpointId, (int)res.error());
		}
	}

	co_return buffer.size() - FRG_CO_TRY(maybeResidue);
}

async::result<frg::expected<proto::UsbError>>
EndpointState::_resetAfterError(RingPointer nextDequeue) {
	// Hold submission mutex for the entire duration of this method to prevent races
	// with newer submissions while we skip the failed TD.
	co_await _submissionMutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _submissionMutex};

	// Issue the Reset Endpoint command to move the endpoint from
	// the halted state to the stopped state
	FRG_CO_TRY(co_await _device->controller()->resetEndpoint(_device->slot(), _endpointId));

	// TODO(qookie): If behind a TT, and this is a control or bulk
	// EP, issue ClearFeature(CLEAR_TT_BUFFER)

	// If this is not a control EP, clear the halt on the device
	// side.
	// XXX(qookie): Linux has class drivers deal with this (but
	// does the rest of the handling, incl. clearing TT buffers,
	// in the xHCI driver).
	if (_type != proto::EndpointType::control) {
		arch::dma_object<proto::SetupPacket> clearHalt{_device->setupPool()};
		clearHalt->type = proto::setup_type::targetEndpoint | proto::setup_type::byStandard | proto::setup_type::toDevice;
		clearHalt->request = proto::request_type::clearFeature;
		clearHalt->value = proto::features::endpointHalt;
		// Our ID is EP no. * 2 + direction, while USB's EP address uses bit 7 for direction instead.
		clearHalt->index = ((_endpointId & 1) ? 0x80 : 0x00) | (_endpointId >> 1);
		clearHalt->length = 0;

		FRG_CO_TRY(co_await _device->transfer({protocols::usb::kXferToDevice, clearHalt, {}}));
	}

	_transferRing.retire(nextDequeue);

	nextDequeue.advance(1, ProducerRing::ringSize);

	// Issue the Set TR Dequeue Pointer command to skip the failed
	// transfer
	FRG_CO_TRY(co_await _device->controller()->setTransferRingDequeue(
				_device->slot(), _endpointId,
				_transferRing, nextDequeue));

	// Ring the doorbell to restart the pipe
	_device->submit(_endpointId);

	co_return frg::success;
}

// ------------------------------------------------------------------------
// Freestanding PCI discovery functions.
// ------------------------------------------------------------------------

async::detached bindController(mbus_ng::Entity entity) {
	protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await device.accessBar(0);

	helix::UniqueDescriptor irq;

	if (info.numMsis) {
		co_await device.enableMsi();
		irq = co_await device.installMsi(0);
	} else {
		co_await device.enableBusIrq();
		irq = co_await device.accessIrq();
	}

	co_await device.enableBusmaster();
	co_await device.enableDma(false);

	auto [iommuActive, dmaSpaceHandle] = co_await device.getDmaSpace();

	helix::Mapping mapping{bar, info.barInfo[0].offset, info.barInfo[0].length};

	auto controller = std::make_shared<Controller>(std::move(device), std::move(entity), std::move(mapping),
			std::move(bar), std::move(irq), std::format("pci.{:08x}", info.barInfo[0].address), iommuActive, std::move(dmaSpaceHandle));
	controller->initialize();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	Cmdline cmdlineHelper{};
	auto cmdline = co_await cmdlineHelper.get();
	frg::array args = {
		frg::option{"xhci.debug", frg::store_true(debugLogs)},
		frg::option{"xhci.no-power-cycle", frg::store_false(powerCyclePorts)},
		frg::option{"xhci.warm-reset", frg::store_true(warmResetPorts)},
	};
	frg::parse_arguments({cmdline.data(), cmdline.size()}, args);

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", "0c"},
		mbus_ng::EqualsFilter{"pci-subclass", "03"},
		mbus_ng::EqualsFilter{"pci-interface", "30"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::println("xhci: Detected controller");
			bindController(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::println("xhci: Starting driver");

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

