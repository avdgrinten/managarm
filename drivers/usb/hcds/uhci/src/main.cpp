
#include <assert.h>
#include <protocols/posix/procdata.hpp>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <fafnir/dsl.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "uhci.hpp"
#include "spec.hpp"
#include "schedule.hpp"

namespace proto = protocols::usb;

std::vector<std::shared_ptr<Controller>> globalControllers;

// ----------------------------------------------------------------------------
// Memory management.
// ----------------------------------------------------------------------------

namespace {
	arch::contiguous_pool schedulePool;
}

// ----------------------------------------------------------------------------
// Pointer.
// ----------------------------------------------------------------------------

Pointer Pointer::from(TransferDescriptor *item) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(item, &physical));
	assert(physical % sizeof(*item) == 0);
	assert((physical & 0xFFFFFFFF) == physical);
	return Pointer(physical, false);
}
Pointer Pointer::from(QueueHead *item) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(item, &physical));
	assert(physical % sizeof(*item) == 0);
	assert((physical & 0xFFFFFFFF) == physical);
	return Pointer(physical, true);
}

// ----------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------

DeviceState::DeviceState(std::shared_ptr<Controller> controller, int device)
: _controller{std::move(controller)}, _device(device) { }

arch::dma_pool *DeviceState::setupPool() {
	return &schedulePool;
}

arch::dma_pool *DeviceState::bufferPool() {
	return &schedulePool;
}

async::result<frg::expected<proto::UsbError, std::string>> DeviceState::deviceDescriptor() {
	return _controller->deviceDescriptor(_device);
}

async::result<frg::expected<proto::UsbError, std::string>> DeviceState::configurationDescriptor(uint8_t configuration) {
	return _controller->configurationDescriptor(_device, configuration);
}

async::result<frg::expected<proto::UsbError, proto::Configuration>> DeviceState::useConfiguration(uint8_t index, uint8_t value) {
	FRG_CO_TRY(co_await _controller->useConfiguration(_device, value));
	co_return proto::Configuration{std::make_shared<ConfigurationState>(_controller,
				_device, index, value)};
}

async::result<frg::expected<proto::UsbError, size_t>> DeviceState::transfer(proto::ControlTransfer info) {
	return _controller->transfer(_device, 0, info);
}

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

ConfigurationState::ConfigurationState(std::shared_ptr<Controller> controller,
		int device, uint8_t index, uint8_t value)
: _controller{std::move(controller)}, _device(device), _index(index), _value(value) {
}

async::result<frg::expected<proto::UsbError, proto::Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	FRG_CO_TRY(co_await _controller->useInterface(_device, _index, _value, number, alternative));
	co_return proto::Interface{std::make_shared<InterfaceState>(_controller, _device, number)};
}

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

InterfaceState::InterfaceState(std::shared_ptr<Controller> controller,
		int device, int interface)
: proto::InterfaceData{interface}, _controller{std::move(controller)}, _device(device), _interface(interface) {
}

async::result<frg::expected<proto::UsbError, proto::Endpoint>>
InterfaceState::getEndpoint(proto::PipeType type, int number) {
	co_return proto::Endpoint{std::make_shared<EndpointState>(_controller,
			_device, type, number)};
}

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

EndpointState::EndpointState(std::shared_ptr<Controller> controller,
		int device, proto::PipeType type, int endpoint)
: _controller{std::move(controller)}, _device(device), _type(type), _endpoint(endpoint) { }

async::result<frg::expected<proto::UsbError, size_t>> EndpointState::transfer(proto::ControlTransfer info) {
	(void)info;
	assert(!"FIXME: Implement this");
	__builtin_unreachable();
}

async::result<frg::expected<proto::UsbError, size_t>> EndpointState::transfer(proto::InterruptTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

async::result<frg::expected<proto::UsbError, size_t>> EndpointState::transfer(proto::BulkTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

// ----------------------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------------------

Controller::Controller(protocols::hw::Device hw_device, mbus_ng::EntityManager entity, uintptr_t base,
		arch::io_space space, helix::UniqueIrq irq)
: _hwDevice{std::move(hw_device)}, _ioBase{base}, _ioSpace{space}, _irq{std::move(irq)},
		_lastFrame{0}, _frameCounter{0},
		_entity{std::move(entity)}, _enumerator{this} {
	for(int i = 1; i < 128; i++) {
		_addressStack.push(i);
	}
}

void Controller::initialize() {
	// Host controller reset.
	_ioSpace.store(op_regs::command, command::hostReset(true));
	while((_ioSpace.load(op_regs::command) & command::hostReset) != 0) { }

	async::run(_hwDevice.enableDma(), helix::currentDispatcher);

	// TODO: What is the rationale of this check?
	auto initial_status = _ioSpace.load(op_regs::status);
	assert(!(initial_status & status::transactionIrq));
	assert(!(initial_status & status::errorIrq));

	// Setup the frame list.
	HelHandle list_handle;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), 4096, 0, nullptr, &list_handle));
	void *list_mapping;
	HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
			nullptr, 0, 4096, kHelMapProtRead | kHelMapProtWrite, &list_mapping));

	_frameList = (FrameList *)list_mapping;
	for(int i = 0; i < 1024; i++)
		_frameList->entries[i].store(FrameListPointer{}._bits);

	// Pass the frame list to the controller and run it.
	uintptr_t list_physical;
	HEL_CHECK(helPointerPhysical(_frameList, &list_physical));
	assert((list_physical % 0x1000) == 0);
	_ioSpace.store(op_regs::frameListBase, list_physical);
	_ioSpace.store(op_regs::command, command::runStop(true));

	// Enable interrupts.
	_ioSpace.store(op_regs::irqEnable, irq::timeout(true) | irq::resume(true)
			| irq::transaction(true) | irq::shortPacket(true));

	_enumerator.observeHub(std::make_shared<RootHub>(this));
	_handleIrqs();
	_refreshFrame();
}

async::detached Controller::_handleIrqs() {
	co_await connectKernletCompiler();

	std::vector<uint8_t> kernlet_program;
	fnr::emit_to(std::back_inserter(kernlet_program),
		// Load the USBSTS register.
		fnr::scope_push{} (
			fnr::intrin{"__pio_read16", 1, 1} (
				fnr::binding{0} // UHCI PIO offset (bound to slot 0).
					 + fnr::literal{op_regs::status.offset()}
			) & fnr::literal{static_cast<uint16_t>(status::transactionIrq(true)
					| status::errorIrq(true)
					| status::hostProcessError(true)
					| status::hostSystemError(true))}
		),
		// Ack the IRQ iff one of the bits was set.
		fnr::check_if{},
			fnr::scope_get{0},
		fnr::then{},
			// Write back the interrupt bits to USBSTS to deassert the IRQ.
			fnr::intrin{"__pio_write16", 2, 0} (
				fnr::binding{0} // UHCI PIO offset (bound to slot 0).
					+ fnr::literal{op_regs::status.offset()},
				fnr::scope_get{0}
			),
			// Trigger the bitset event (bound to slot 1).
			fnr::intrin{"__trigger_bitset", 2, 0} (
				fnr::binding{1},
				fnr::scope_get{0}
			),
			fnr::scope_push{} ( fnr::literal{1} ),
		fnr::else_then{},
			fnr::scope_push{} ( fnr::literal{2} ),
		fnr::end{}
	);

	auto kernlet_object = co_await compile(kernlet_program.data(),
			kernlet_program.size(), {BindType::offset, BindType::bitsetEvent});

	HelHandle event_handle;
	HEL_CHECK(helCreateBitsetEvent(&event_handle));
	helix::UniqueDescriptor event{event_handle};

	HelKernletData data[2];
	data[0].handle = _ioBase;
	data[1].handle = event.getHandle();
	HelHandle bound_handle;
	HEL_CHECK(helBindKernlet(kernlet_object.getHandle(), data, 2, &bound_handle));
	HEL_CHECK(helAutomateIrq(_irq.getHandle(), 0, bound_handle));

	co_await _hwDevice.enableBusIrq();

	// Clear the IRQ in case it was pending while we attached the kernlet.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick | kHelAckClear, 0));

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(event, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto bits = arch::bit_value<uint16_t>(await.bitset());

		assert(!(bits & status::hostProcessError));
		assert(!(bits & status::hostSystemError));

		if(bits & status::errorIrq)
			printf("\e[31muhci: Error interrupt\e[39m\n");
		if((bits & status::transactionIrq) || (bits & status::errorIrq)) {
			//printf("uhci: Processing transfers.\n");
			_progressSchedule();
		}
	}
}

async::detached Controller::_refreshFrame() {
	while(true) {
//		std::cout << "uhci: Frame update" << std::endl;
		_updateFrame();

		uint64_t tick;
		HEL_CHECK(helGetClock(&tick));

		helix::AwaitClock await_clock;
		auto &&submit = helix::submitAwaitClock(&await_clock, tick + 500'000'000,
				helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(await_clock.error());
	}
}

void Controller::_updateFrame() {
	auto frame = _ioSpace.load(op_regs::frameNumber);
	auto counter = (frame >= _lastFrame) ? (_frameCounter + frame - _lastFrame)
			: (_frameCounter + 2048 - _lastFrame + frame);

	if(counter / 1024 > _frameCounter / 1024) {
		for(int port = 0; port < 2; port++) {
			auto port_space = _ioSpace.subspace(0x10 + (2 * port));
			auto sc = port_space.load(port_regs::statusCtrl);
//			std::cout << "uhci: Port " << port << " status/control: "
//					<< static_cast<uint16_t>(sc) << std::endl;

			// Extract the status bits.
			_portState[port].status = 0;
			if(sc & port_status_ctrl::connectStatus)
				_portState[port].status |= proto::HubStatus::connect;
			if(sc & port_status_ctrl::enableStatus)
				_portState[port].status |= proto::HubStatus::enable;

			// Extract the change bits.
			if(sc & port_status_ctrl::connectChange) {
				_portState[port].changes |= proto::HubStatus::connect;
				_portDoorbell.raise();
			}
			if(sc & port_status_ctrl::enableChange) {
				_portState[port].changes |= proto::HubStatus::enable;
				_portDoorbell.raise();
			}

			// Write-back clears the change bits.
			port_space.store(port_regs::statusCtrl, sc);
		}
	}

	_lastFrame = frame;
	_frameCounter = counter;

	// This is where we perform actual reclamation.
	while(!_reclaimQueue.empty()) {
		auto item = &_reclaimQueue.front();
		if(item->reclaimFrame > _frameCounter)
			break;
		_reclaimQueue.pop_front();
		delete item;
	}
}

// ----------------------------------------------------------------
// Controller: USB device discovery methods.
// ----------------------------------------------------------------

size_t Controller::RootHub::numPorts() {
	return 2;
}

async::result<proto::PortState> Controller::RootHub::pollState(int port) {
	while(true) {
		auto state = _controller->_portState[port - 1];
		if(state.changes) {
			_controller->_portState[port - 1].changes = 0;
			co_return state;
		}

		co_await _controller->_portDoorbell.async_wait();
	}
}

async::result<frg::expected<proto::UsbError, void>>
Controller::RootHub::issueReset(int port) {
	using proto::UsbError;
	using proto::DeviceSpeed;

	auto port_space = _controller->_ioSpace.subspace(0x10 + (2 * (port - 1)));

	// Reset the port for 50 ms.
	port_space.store(port_regs::statusCtrl, port_status_ctrl::portReset(true));

	uint64_t tick;
	HEL_CHECK(helGetClock(&tick));

	helix::AwaitClock await_clock;
	auto &&submit = helix::submitAwaitClock(&await_clock, tick + 50'000'000,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(await_clock.error());

	// Disable the reset line.
	port_space.store(port_regs::statusCtrl, port_status_ctrl::portReset(false));

	// Linux issues a 10us wait here, probably to wait until reset is turned off in hardware.
	usleep(10);

	// Enable the port and wait until it is available.
	port_space.store(port_regs::statusCtrl, port_status_ctrl::enableStatus(true));

	uint64_t start;
	HEL_CHECK(helGetClock(&start));
	while(true) {
		auto sc = port_space.load(port_regs::statusCtrl);
		if((sc & port_status_ctrl::enableStatus))
			break;

		uint64_t now;
		HEL_CHECK(helGetClock(&now));
		if(now - start > 1000'000'000) {
			std::cout << "\e[31muhci: Could not enable device after reset\e[39m" << std::endl;
			co_return UsbError::timeout;
		}
	}

	// Similar to USB standard hubs we do not reset the enable-change bit.
	_controller->_portState[port - 1].status |= proto::HubStatus::enable;
	_controller->_portState[port - 1].changes |= proto::HubStatus::reset;
	_controller->_portDoorbell.raise();

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>>
Controller::RootHub::querySpeed(int port) {
	using proto::DeviceSpeed;

	auto port_space = _controller->_ioSpace.subspace(0x10 + (2 * (port - 1)));

	auto sc = port_space.load(port_regs::statusCtrl);

	DeviceSpeed speed = (sc & port_status_ctrl::lowSpeed)
				? DeviceSpeed::lowSpeed
				: DeviceSpeed::fullSpeed;

	co_return speed;
}

async::result<frg::expected<proto::UsbError>>
Controller::enumerateDevice(std::shared_ptr<proto::Hub> parentHub, int port, proto::DeviceSpeed speed) {
	using proto::DeviceSpeed;

	assert(speed == DeviceSpeed::lowSpeed || speed == DeviceSpeed::fullSpeed);
	bool low_speed = speed == DeviceSpeed::lowSpeed;

	// This queue will become the default control pipe of our new device.
	auto queue = new QueueEntity{arch::dma_object<QueueHead>{&schedulePool}};
	_linkAsync(queue);

	// Allocate an address for the device.
	assert(!_addressStack.empty());
	auto address = _addressStack.front();
	_addressStack.pop();

	arch::dma_object<proto::SetupPacket> set_address{&schedulePool};
	set_address->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toDevice;
	set_address->request = proto::request_type::setAddress;
	set_address->value = address;
	set_address->index = 0;
	set_address->length = 0;

	(co_await _directTransfer(0, 0, proto::ControlTransfer{proto::kXferToDevice,
			set_address, arch::dma_buffer_view{}}, queue, low_speed, 8)).unwrap();

	// Enquire the maximum packet size of the default control pipe.
	arch::dma_object<proto::SetupPacket> get_header{&schedulePool};
	get_header->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_header->request = proto::request_type::getDescriptor;
	get_header->value = proto::descriptor_type::device << 8;
	get_header->index = 0;
	get_header->length = 8;

	arch::dma_object<proto::DeviceDescriptor> descriptor{&schedulePool};
	(co_await _directTransfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_header, descriptor.view_buffer().subview(0, 8)}, queue, low_speed, 8)).unwrap();

	_activeDevices[address].lowSpeed = low_speed;
	_activeDevices[address].controlStates[0].queueEntity = queue;
	_activeDevices[address].controlStates[0].maxPacketSize = descriptor->maxPacketSize;

	// Read the rest of the device descriptor.
	arch::dma_object<proto::SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_descriptor->request = proto::request_type::getDescriptor;
	get_descriptor->value = proto::descriptor_type::device << 8;
	get_descriptor->index = 0;
	get_descriptor->length = sizeof(proto::DeviceDescriptor);

	(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_descriptor, descriptor.view_buffer()})).unwrap();
	assert(descriptor->length == sizeof(proto::DeviceDescriptor));

	// Read the configuration descriptor
	arch::dma_object<proto::SetupPacket> getConfigDescriptor{&schedulePool};
	arch::dma_object<proto::ConfigDescriptor> configDescriptor{&schedulePool};

	getConfigDescriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	getConfigDescriptor->request = proto::request_type::getDescriptor;
	getConfigDescriptor->value = proto::descriptor_type::configuration << 8;
	getConfigDescriptor->index = 0;
	getConfigDescriptor->length = sizeof(proto::ConfigDescriptor);

	(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			getConfigDescriptor, configDescriptor.view_buffer()})).unwrap();
	assert(configDescriptor->length == sizeof(proto::ConfigDescriptor));

	// Select the first configuration in order to exit the addressing state
	arch::dma_object<proto::SetupPacket> setConfig{&schedulePool};
	setConfig->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toDevice;
	setConfig->request = proto::request_type::setConfig;
	setConfig->value = configDescriptor->configValue;
	setConfig->index = 0;
	setConfig->length = 0;
	(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToDevice,
			setConfig, {}})).unwrap();

	char class_code[3], sub_class[3], protocol[3];
	char vendor[5], product[5], release[5];
	snprintf(class_code, 3, "%.2x", descriptor->deviceClass);
	snprintf(sub_class, 3, "%.2x", descriptor->deviceSubclass);
	snprintf(protocol, 3, "%.2x", descriptor->deviceProtocol);
	snprintf(vendor, 5, "%.4x", descriptor->idVendor);
	snprintf(product, 5, "%.4x", descriptor->idProduct);
	snprintf(release, 5, "%.4x", descriptor->bcdDevice);

	std::cout << "uhci: Enumerating device of class: 0x" << class_code
			<< ", sub class: 0x" << sub_class << ", protocol: 0x" << protocol << std::endl;

	if(descriptor->deviceClass == 0x09 && descriptor->deviceSubclass == 0
			&& descriptor->deviceProtocol == 0) {
		auto state = std::make_shared<DeviceState>(shared_from_this(), address);
		auto hub = (co_await createHubFromDevice(parentHub, proto::Device{std::move(state)}, port)).unwrap();
		_enumerator.observeHub(std::move(hub));
	}

	char name[3];
	snprintf(name, 3, "%.2x", address);

	std::string mbps = protocols::usb::getSpeedMbps(speed);

	mbus_ng::Properties mbusDescriptor {
		{ "usb.type", mbus_ng::StringItem{"device"}},
		{ "usb.vendor", mbus_ng::StringItem{vendor}},
		{ "usb.product", mbus_ng::StringItem{product}},
		{ "usb.class", mbus_ng::StringItem{class_code}},
		{ "usb.subclass", mbus_ng::StringItem{sub_class}},
		{ "usb.protocol", mbus_ng::StringItem{protocol}},
		{ "usb.release", mbus_ng::StringItem{release}},
		{ "usb.hub_port", mbus_ng::StringItem{name}},
		{ "usb.bus", mbus_ng::StringItem{std::to_string(_entity.id())}},
		{ "usb.speed", mbus_ng::StringItem{mbps}},
		{ "unix.subsystem", mbus_ng::StringItem{"usb"}},
	};


	auto usbEntity = (co_await mbus_ng::Instance::global().createEntity(
				"usb-uhci-dev-" + std::string{name}, mbusDescriptor)).unwrap();

	[] (auto self, auto address, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			auto state = std::make_shared<DeviceState>(self->shared_from_this(), address);
			proto::serve(proto::Device{std::move(state)}, std::move(localLane));
		}
	}(this, address, std::move(usbEntity));

	co_return frg::success;
}

// ------------------------------------------------------------------------
// Controller: Device management.
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, std::string>>
Controller::deviceDescriptor(int address) {
	arch::dma_object<proto::SetupPacket> get_header{&schedulePool};
	get_header->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_header->request = proto::request_type::getDescriptor;
	get_header->value = proto::descriptor_type::device << 8;
	get_header->index = 0;
	get_header->length = 8;

	arch::dma_object<proto::DeviceDescriptor> descriptor{&schedulePool};
	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_header, descriptor.view_buffer().subview(0, 8)}));

	// Read the rest of the device descriptor.
	arch::dma_object<proto::SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_descriptor->request = proto::request_type::getDescriptor;
	get_descriptor->value = proto::descriptor_type::device << 8;
	get_descriptor->index = 0;
	get_descriptor->length = sizeof(proto::DeviceDescriptor);

	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_descriptor, descriptor.view_buffer()}));
	assert(descriptor->length == sizeof(proto::DeviceDescriptor));

	std::string copy((char *)descriptor.data(), sizeof(proto::DeviceDescriptor));
	co_return std::move(copy);
}

async::result<frg::expected<proto::UsbError, std::string>>
Controller::configurationDescriptor(int address, uint8_t configuration) {
	// Read the descriptor header that contains the hierachy size.
	arch::dma_object<proto::SetupPacket> get_header{&schedulePool};
	get_header->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_header->request = proto::request_type::getDescriptor;
	get_header->value = proto::descriptor_type::configuration << 8 | configuration;
	get_header->index = 0;
	get_header->length = sizeof(proto::ConfigDescriptor);

	arch::dma_object<proto::ConfigDescriptor> header{&schedulePool};
	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_header, header.view_buffer()}));
	assert(header->length == sizeof(proto::ConfigDescriptor));

	// Read the whole descriptor hierachy.
	arch::dma_object<proto::SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_descriptor->request = proto::request_type::getDescriptor;
	get_descriptor->value = proto::descriptor_type::configuration << 8 | configuration;
	get_descriptor->index = 0;
	get_descriptor->length = header->totalLength;

	arch::dma_buffer descriptor{&schedulePool, header->totalLength};
	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_descriptor, descriptor}));

	// TODO: This function should return a arch::dma_buffer!
	std::string copy((char *)descriptor.data(), header->totalLength);
	co_return std::move(copy);
}

async::result<frg::expected<proto::UsbError>>
Controller::useConfiguration(int address, int configuration) {
	arch::dma_object<proto::SetupPacket> set_config{&schedulePool};
	set_config->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toDevice;
	set_config->request = proto::request_type::setConfig;
	set_config->value = configuration;
	set_config->index = 0;
	set_config->length = 0;

	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToDevice,
			set_config, arch::dma_buffer_view{}}));
	co_return{};
}

async::result<frg::expected<proto::UsbError>>
Controller::useInterface(int address, uint8_t configIndex, uint8_t configValue, int interface, int alternative) {
	(void) interface;
	assert(!alternative);

	std::optional<uint8_t> valueByIndex;

	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor(address, configIndex));
	bool fail = false;
	proto::walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type == proto::descriptor_type::configuration) {
			auto desc = (proto::ConfigDescriptor *)p;
			valueByIndex = desc->configValue;
		}

		if(type != proto::descriptor_type::endpoint)
			return;
		auto desc = (proto::EndpointDescriptor *)p;

		// TODO: Pay attention to interface/alternative.
		std::cout << "uhci: Interval is " << (int)desc->interval << std::endl;

		int pipe = info.endpointNumber.value();
		QueueEntity *entity;
		if(info.endpointIn.value()) {
			std::cout << "uhci: Setting up IN endpoint " << pipe << std::endl;
			entity = new QueueEntity{arch::dma_object<QueueHead>{&schedulePool}};
			_activeDevices[address].inStates[pipe].maxPacketSize = desc->maxPacketSize;
			_activeDevices[address].inStates[pipe].queueEntity = entity;
		}else{
			std::cout << "uhci: Setting up OUT endpoint " << pipe << std::endl;
			entity = new QueueEntity{arch::dma_object<QueueHead>{&schedulePool}};
			_activeDevices[address].outStates[pipe].maxPacketSize = desc->maxPacketSize;
			_activeDevices[address].outStates[pipe].queueEntity = entity;
		}

		if (info.endpointType == proto::EndpointType::interrupt) {
			auto order = 1 << (CHAR_BIT * sizeof(int) - __builtin_clz(desc->interval) - 1);
			std::cout << "uhci: Using order " << order << std::endl;
			this->_linkInterrupt(entity, order, 0);
		} else if (info.endpointType == proto::EndpointType::bulk){
			this->_linkAsync(entity);
		} else {
			std::cout << "uhci: Unsupported endpoint type in Controller::useInterface!" << std::endl;
			fail = true;
			return;
		}
	});

	assert(valueByIndex);
	// Bail out if the user has no idea what they're asking for
	// A little late, but better late than never...
	if (*valueByIndex != configValue) {
		printf("uhci: useConfiguration(%u, %u) called, but that configuration has bConfigurationValue = %u???\n",
				configIndex, configValue, *valueByIndex);
		co_return proto::UsbError::other;
	}

	if (fail)
		co_return proto::UsbError::unsupported;
	else
		co_return {};
}

// ------------------------------------------------------------------------
// Controller: Transfer functions.
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, size_t>>
Controller::transfer(int address, int pipe, proto::ControlTransfer info) {
	auto device = &_activeDevices[address];
	auto endpoint = &device->controlStates[pipe];

	auto transaction = _buildControl(address, pipe, info.flags,
			info.setup, info.buffer, device->lowSpeed, endpoint->maxPacketSize);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::transfer(int address, proto::PipeType type, int pipe,
		proto::InterruptTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == proto::PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == proto::PipeType::out);
		assert(!info.allowShortPackets);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(address, pipe, info.flags,
			info.buffer, device->lowSpeed, endpoint->maxPacketSize, info.allowShortPackets);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::transfer(int address, proto::PipeType type, int pipe,
		proto::BulkTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == proto::PipeType::in) {
		assert(info.flags == proto::kXferToHost);
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == proto::PipeType::out);
		assert(info.flags == proto::kXferToDevice);
		assert(!info.allowShortPackets);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(address, pipe, info.flags,
			info.buffer, device->lowSpeed, endpoint->maxPacketSize, info.allowShortPackets);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

auto Controller::_buildControl(int address, int pipe, proto::XferFlags dir,
		arch::dma_object_view<proto::SetupPacket> setup, arch::dma_buffer_view buffer,
		bool low_speed, size_t max_packet_size) -> Transaction * {
	assert((dir == proto::kXferToDevice) || (dir == proto::kXferToHost));

	size_t num_data = (buffer.size() + max_packet_size - 1) / max_packet_size;
	arch::dma_array<TransferDescriptor> transfers{&schedulePool, num_data + 2};

	transfers[0].status.store(td_status::active(true) | td_status::detectShort(true)
			| td_status::lowSpeed(low_speed));
	transfers[0].token.store(td_token::pid(Packet::setup) | td_token::address(address)
			| td_token::pipe(pipe) | td_token::length(sizeof(proto::SetupPacket) - 1));
	transfers[0]._bufferPointer = TransferBufferPointer::from(setup.data());
	transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[1]);

	size_t progress = 0;
	for(size_t i = 0; i < num_data; i++) {
		size_t chunk = std::min(max_packet_size, buffer.size() - progress);
		assert(chunk);
		transfers[i + 1].status.store(td_status::active(true) | td_status::detectShort(true)
				| td_status::lowSpeed(low_speed));
		transfers[i + 1].token.store(td_token::pid(dir == proto::kXferToDevice ? Packet::out : Packet::in)
				| td_token::toggle(i % 2 == 0) | td_token::address(address)
				| td_token::pipe(pipe) | td_token::length(chunk - 1));
		transfers[i + 1]._bufferPointer
				= TransferBufferPointer::from((char *)buffer.data() + progress);
		transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 2]);
		progress += chunk;
	}

	transfers[num_data + 1].status.store(td_status::active(true) | td_status::completionIrq(true)
			| td_status::lowSpeed(low_speed));
	transfers[num_data + 1].token.store(td_token::pid(dir == proto::kXferToDevice ? Packet::in : Packet::out)
			| td_token::toggle(true) | td_token::address(address)
			| td_token::pipe(pipe) | td_token::length(0x7FF));

	return new Transaction{std::move(transfers)};
}

auto Controller::_buildInterruptOrBulk(int address, int pipe, proto::XferFlags dir,
		arch::dma_buffer_view buffer,
		bool low_speed, size_t max_packet_size,
		bool allow_short_packet) -> Transaction * {
	assert((dir == proto::kXferToDevice) || (dir == proto::kXferToHost));
//	std::cout << "_buildInterruptOrBulk. Address: " << address
//			<< ", pipe: " << pipe << ", direction: " << dir
//			<< ", maxPacketSize: " << max_packet_size
//			<< ", buffer size: " << buffer.size() << std::endl;

	size_t num_data = (buffer.size() + max_packet_size - 1) / max_packet_size;
	arch::dma_array<TransferDescriptor> transfers{&schedulePool, num_data};

	size_t progress = 0;
	for(size_t i = 0; i < num_data; i++) {
		size_t chunk = std::min(max_packet_size, buffer.size() - progress);
		assert(chunk);
		// TODO: Only set detectShort bit if allow_short_packet is true?
		transfers[i].status.store(td_status::active(true)
				| td_status::completionIrq(i + 1 == num_data) | td_status::detectShort(true)
				| td_status::lowSpeed(low_speed));
		transfers[i].token.store(td_token::pid(dir == proto::kXferToDevice ? Packet::out : Packet::in)
				| td_token::address(address) | td_token::pipe(pipe)
				| td_token::length(chunk - 1));
		transfers[i]._bufferPointer = TransferBufferPointer::from((char *)buffer.data() + progress);

		if(i + 1 < num_data)
			transfers[i]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 1]);
		progress += chunk;
	}

	auto transaction = new Transaction{std::move(transfers), allow_short_packet};
	transaction->autoToggle = true;
	return transaction;
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::_directTransfer(int address, int pipe, proto::ControlTransfer info,
		QueueEntity *queue, bool low_speed, size_t max_packet_size) {
	auto transaction = _buildControl(address, pipe, info.flags,
			info.setup, info.buffer, low_speed, max_packet_size);
	auto future = transaction->promise.get_future();
	_linkTransaction(queue, transaction);
	co_return *(co_await future.get());
}

// ----------------------------------------------------------------
// Controller: Schedule manipulation functions.
// ----------------------------------------------------------------

void Controller::_linkInterrupt(QueueEntity *entity, int order, int index) {
	assert(order > 0 && order <= 1024);
	assert(index < order);

	// Try to find a periodic entity with lower order that we link to.
	int so = order >> 1;
	while(so) {
		/*auto n = (so - 1) + (index & (so - 1));
		if(!_interruptSchedule[n].empty()) {
			std::cout << "Linking to a lower order. This is untested" << std::endl;
			auto successor = &_interruptSchedule[n].front();
			entity->head->_linkPointer = QueueHead::LinkPointer::from(successor->head.data());
			break;
		}*/
		so >>= 1;
	}

	// If there is no lower-order periodic entity, link to the async schedule.
	if(!so) {
		assert(!_asyncSchedule.empty());
		auto successor = &_asyncSchedule.front();
		entity->head->_linkPointer = QueueHead::LinkPointer::from(successor->head.data());
	}

	// Link to the back of this order/index of the periodic schedule.
	auto n = (order - 1) + index;
	if(_interruptSchedule[n].empty()) {
		// Link the front of the schedule to the new entity.
		if(order == 1024) {
			_frameList->entries[index].store(FrameListPointer::from(entity->head.data())._bits);
		}else{
			_linkIntoScheduleTree(order << 1, index, entity);
			_linkIntoScheduleTree(order << 1, index + order, entity);
		}
	}else{
		auto predecessor = &_interruptSchedule[n].back();
		predecessor->head->_linkPointer = QueueHead::LinkPointer::from(entity->head.data());
	}
	_interruptSchedule[n].push_back(*entity);
	_activeEntities.push_back(entity);
}

void Controller::_linkAsync(QueueEntity *entity) {
	// Link to the back of the asynchronous schedule.
	if(_asyncSchedule.empty()) {
		// Link the front of the schedule to the new entity.
		_linkIntoScheduleTree(1, 0, entity);
	}else{
		_asyncSchedule.back().head->_linkPointer
				= QueueHead::LinkPointer::from(entity->head.data());
	}
	_asyncSchedule.push_back(*entity);
	_activeEntities.push_back(entity);
}

void Controller::_linkIntoScheduleTree(int order, int index, QueueEntity *entity) {
	assert(order > 0 && order <= 1024);
	assert(index < order);

	auto n = (order - 1) + index;
	if(_interruptSchedule[n].empty()) {
		if(order == 1024) {
			_frameList->entries[index].store(FrameListPointer::from(entity->head.data())._bits);
		}else{
			_linkIntoScheduleTree(order << 1, index, entity);
			_linkIntoScheduleTree(order << 1, index + order, entity);
		}
	}else{
		auto predecessor = &_interruptSchedule[n].back();
		predecessor->head->_linkPointer = QueueHead::LinkPointer::from(entity->head.data());
	}
}

void Controller::_linkTransaction(QueueEntity *queue, Transaction *transaction) {
	if(queue->transactions.empty()) {
		// Update the toggle state of the transaction.
		if(transaction->autoToggle) {
			bool state = queue->toggleState;
			for(size_t i = 0; i < transaction->transfers.size(); i++) {
				transaction->transfers[i].token.store(transaction->transfers[i].token.load()
						| td_token::toggle(state));
				state = !state;
			}
		}

		queue->head->_elementPointer = QueueHead::LinkPointer::from(&transaction->transfers[0]);
	}

	queue->transactions.push_back(*transaction);
}

void Controller::_progressSchedule() {
	// NOTE: This loop is intentionally weird to account for the fact that
	// _progressQueue may in fact add entries to the active list.  Any iterators
	// are then potentially invalidated.
	volatile size_t size = _activeEntities.size();
	for(size_t i = 0; i < size; i++) {
		_progressQueue(_activeEntities[i]);
		size = _activeEntities.size();
	}
}

void Controller::_progressQueue(QueueEntity *entity) {
	if(entity->transactions.empty())
		return;

	auto front = &entity->transactions.front();

	auto handleError = [&] () {
		// TODO: This could also mean that the TD is not retired because of SPD.
		// TODO: Unify this case with the transaction success case above.
		std::cout << "\e[31muhci: Transfer error!\e[39m" << std::endl;
		_dump(front);

		// Clean up the Queue.
		entity->transactions.pop_front();
//TODO:		_reclaim(front);
	};

	auto decodeLength = [] (size_t n) -> size_t {
		if(n == 0x7FF)
			return 0;
		assert(n <= 0x4FF);
		return n + 1;
	};

	while(front->numComplete < front->transfers.size()) {
		auto &transfer = front->transfers[front->numComplete];
		auto status = transfer.status.load();
		if(status & td_status::active) {
			return;
		}else if(status & td_status::errorBits) {
			handleError();
			return;
		}
		assert(!(status & td_status::stalled));

		auto n = status & td_status::actualLength;
		front->numComplete++;
		front->lengthComplete += decodeLength(n);

		// We advance the toggleState on each successful transaction for
		// each pipe type, not only for bulk/interrupt. This does not really hurt.
		entity->toggleState = !entity->toggleState;

		// Short packets end the transfer without advancing the queue.
		if(n != (transfer.token.load() & td_token::length)) {
			if(!front->allowShortPackets) {
				std::cout << "uhci: Actual length is " << n
						<< ", while we expect " << (transfer.token.load() & td_token::length)
						<< ", auto toggle is " << front->autoToggle
						<< std::endl;
				throw std::runtime_error("uhci: Short packet not allowed");
			}
			break;
		}
	}

	//printf("Transfer complete!\n");
	front->promise.set_value(size_t{front->lengthComplete});

	// Schedule the next transaction.
	entity->transactions.pop_front();
	if(entity->transactions.empty()) {
		entity->head->_elementPointer = QueueHead::LinkPointer{};
	}else{
		auto front = &entity->transactions.front();
		entity->head->_elementPointer = QueueHead::LinkPointer::from(&front->transfers[0]);
	}

	// Reclaim memory.
	_reclaim(front);
}

void Controller::_reclaim(ScheduleItem *item) {
	assert(item->reclaimFrame == -1);

	_updateFrame();
	item->reclaimFrame = _frameCounter + 1;
	_reclaimQueue.push_back(*item);
}

// ----------------------------------------------------------------------------
// Debugging functions.
// ----------------------------------------------------------------------------

void Controller::_dump(Transaction *transaction) {
	for(size_t i = 0; i < transaction->transfers.size(); i++) {
		std::cout << "    TD " << i << ":";
		transaction->transfers[i].dumpStatus();
		std::cout << std::endl;
	}
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::detached bindController(mbus_ng::Entity entity) {
	protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[4].ioType == protocols::hw::IoType::kIoTypePort);
	auto bar = co_await device.accessBar(4);
	auto irq = co_await device.accessIrq();

	mbus_ng::Properties descriptor{
		{"generic.devtype", mbus_ng::StringItem{"usb-controller"}},
		{"generic.devsubtype", mbus_ng::StringItem{"uhci"}},
		{"usb.version.major", mbus_ng::StringItem{"1"}},
		{"usb.version.minor", mbus_ng::StringItem{"16"}},
		{"usb.root.parent", mbus_ng::StringItem{std::to_string(entity.id())}},
	};

	auto uhciEntity = (co_await mbus_ng::Instance::global().createEntity(
				"uhci-controller", descriptor)).unwrap();

	// TODO: Disable the legacy support registers of all UHCI devices
	// before using one of them!
	auto legsup = co_await device.loadPciSpace(kPciLegacySupport, 2);
	std::cout << "uhci: Legacy support register: " << legsup << std::endl;

	co_await device.enableBusmaster();
	HEL_CHECK(helEnableIo(bar.getHandle()));

	arch::io_space base = arch::global_io.subspace(info.barInfo[4].address);
	auto controller = std::make_shared<Controller>(std::move(device), std::move(uhciEntity),
			info.barInfo[4].address, base, std::move(irq));
	controller->initialize();

	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", "0c"},
		mbus_ng::EqualsFilter{"pci-subclass", "03"},
		mbus_ng::EqualsFilter{"pci-interface", "00"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "uhci: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("uhci: Starting driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 2));

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

