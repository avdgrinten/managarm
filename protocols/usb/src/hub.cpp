#include <vector>

#include <protocols/usb/hub.hpp>
#include <protocols/usb/server.hpp>
#include <protocols/mbus/client.hpp>

#include <async/recurring-event.hpp>

#include <helix/timer.hpp>

namespace protocols::usb {

// ----------------------------------------------------------------
// Enumerator.
// ----------------------------------------------------------------

void Enumerator::observeHub(std::shared_ptr<Hub> hub) {
	for (size_t port = 1; port <= hub->numPorts(); port++)
		observePort_(hub, port);
}

async::detached Enumerator::observePort_(std::shared_ptr<Hub> hub, int port) {
	while (true)
		co_await observationCycle_(hub, port);
}

async::result<void> Enumerator::observationCycle_(std::shared_ptr<Hub> hub, int port) {
	std::unique_lock<async::mutex> enumerateLock;

	// Wait until the device is connected.
	while (true) {
		auto s = co_await hub->pollState(port);

		if (s.status & HubStatus::connect)
			break;
	}

	co_await enumerateMutex_.async_lock();
	enumerateLock = std::unique_lock<async::mutex>{enumerateMutex_, std::adopt_lock};

	std::cout << "usb: Issuing reset on port " << port << std::endl;

	if (auto v = co_await hub->issueReset(port); !v) {
		std::cout << "usb: Device on port " << port << " failed to reset: "
			<< (int)v.error() << std::endl;
		co_return;
	}

	std::cout << "usb: Waiting for device to become enabled on port " << port << std::endl;

	// Wait until the device is enabled.
	while (true) {
		auto s = co_await hub->pollState(port);

		// TODO: Handle disconnect here.
		if (s.status & HubStatus::enable)
			break;
	}

	std::cout << "usb: Enumerating device on port " << port << std::endl;

	DeviceSpeed speed;
	if (auto v = co_await hub->querySpeed(port); !v) {
		std::cout << "usb: Failed to query speed of device on port " << port << ": "
			<< (int)v.error() << std::endl;
		co_return;
	} else {
		speed = v.value();
	}

	auto device = controller_->createDevice(hub, port, speed);

	if (auto v = co_await enumerateDevice_(device); !v) {
		std::cout << "usb: Device on port " << port << " failed to enumerate: "
			<< (int)v.error() << std::endl;
		co_return;
	}

	enumerateLock.unlock();

	// Wait until the device is disconnected.
	while(true) {
		auto s = co_await hub->pollState(port);

		if(!(s.status & HubStatus::connect))
			break;
	}
}

async::result<frg::expected<UsbError>>
Enumerator::enumerateDevice_(std::shared_ptr<DeviceServerData> device) {
	FRG_CO_TRY(co_await device->initialize());

	// If this is full speed, our guess for MPS might be wrong,
	// get the first 8 bytes of the device descriptor to check.
	if (device->speed() == DeviceSpeed::fullSpeed) {
		arch::dma_object<DeviceDescriptor> descriptor{device->bufferPool()};
		FRG_CO_TRY(co_await device->readDescriptor(descriptor.view_buffer().subview(0, 8), 0x0100));

		std::println("usb: Full-speed device on port {} has bMaxPacketSize0 = {}",
				device->port(), int{descriptor->maxPacketSize});

		FRG_CO_TRY(co_await device->updateEp0MaxPacketSize(descriptor->maxPacketSize));
	}

	arch::dma_object<DeviceDescriptor> descriptor{device->bufferPool()};
	FRG_CO_TRY(co_await device->readDescriptor(descriptor.view_buffer(), 0x0100));

	arch::dma_object<ConfigDescriptor> configDescriptor{device->bufferPool()};
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
		auto hub = FRG_CO_TRY(co_await createHubFromDevice(device));

		FRG_CO_TRY(co_await device->configureAsHub(hub));

		observeHub(std::move(hub));
	}

	auto address = std::format("{:02x}", device->address());

	std::string mbps = getSpeedMbps(device->speed());

	auto [_route, rootHub, _rootPort] = device->routeString();
	auto rootEntityId = rootHub->mbusEntityId();

	mbus_ng::Properties mbusDescriptor{
		{"usb.type", mbus_ng::StringItem{"device"}},
		{"usb.vendor", mbus_ng::StringItem{vendor}},
		{"usb.product", mbus_ng::StringItem{product}},
		{"usb.class", mbus_ng::StringItem{classCode}},
		{"usb.subclass", mbus_ng::StringItem{subClass}},
		{"usb.protocol", mbus_ng::StringItem{protocol}},
		{"usb.release", mbus_ng::StringItem{release}},
		{"usb.hub_port", mbus_ng::StringItem{address}},
		{"usb.bus", mbus_ng::StringItem{std::to_string(rootEntityId)}},
		{"usb.speed", mbus_ng::StringItem{mbps}},
		{"unix.subsystem", mbus_ng::StringItem{"usb"}},
	};

	auto usbEntity = (co_await mbus_ng::Instance::global().createEntity(
				"usb-dev-" + std::string{address}, mbusDescriptor)).unwrap();

	[] (auto device, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			serve(Device{device}, std::move(localLane));
		}
	}(device, std::move(usbEntity));

	co_return frg::success;
}

// ----------------------------------------------------------------
// StandardHub.
// ----------------------------------------------------------------

namespace {

namespace ClassRequests {
	static constexpr uint8_t getStatus = 0;
	static constexpr uint8_t clearFeature = 1;
	static constexpr uint8_t setFeature = 3;
	static constexpr uint8_t getDescriptor = 6;
}

namespace PortBits {
	static constexpr uint16_t connect = 0x01;
	static constexpr uint16_t enable = 0x02;
	static constexpr uint16_t reset = 0x10;
	static constexpr uint16_t lowSpeed = 0x200;
	static constexpr uint16_t highSpeed = 0x400;
}

namespace PortFeatures {
	//static constexpr uint16_t connect = 0;
	//static constexpr uint16_t enable = 1;
	static constexpr uint16_t reset = 4;
	static constexpr uint16_t power = 8;
	static constexpr uint16_t connectChange = 16;
	static constexpr uint16_t enableChange = 17;
	static constexpr uint16_t resetChange = 20;
}

struct StandardHub final : Hub {
	StandardHub(std::shared_ptr<DeviceServerData> device)
	: Hub{std::move(device)}, endpoint_{nullptr} { }

	async::result<frg::expected<UsbError>> initialize();

private:
	async::detached run_();

	HubCharacteristics characteristics_;

public:
	size_t numPorts() override;
	async::result<PortState> pollState(int port) override;
	async::result<frg::expected<UsbError, void>> issueReset(int port) override;
	async::result<frg::expected<UsbError, DeviceSpeed>> querySpeed(int port) override;

	frg::expected<UsbError, HubCharacteristics> getCharacteristics() override {
		return characteristics_;
	}

private:
	Endpoint endpoint_;

	async::recurring_event doorbell_;
	std::vector<PortState> state_;
};

async::result<frg::expected<UsbError>> StandardHub::initialize() {
	// Read the generic USB device configuration.
	std::optional<int> intfNumber;
	std::optional<int> endNumber;

	auto cfgDescriptor = FRG_CO_TRY(co_await state()->configurationDescriptor(0));
	auto cfgRange = configurationRange(cfgDescriptor);

	auto configDesc = configDescriptorFrom(cfgRange);
	if(!configDesc)
		co_return UsbError::other;

	for(auto [intf, body] : groupByInterface(cfgRange)) {
		intfNumber = intf.interfaceNumber;
		for(auto ep : endpointsOf(body)) {
			endNumber = ep.endpointAddress & 0x0F;
			break;
		}
		break;
	}

	auto cfg = FRG_CO_TRY(co_await state()->useConfiguration(0, configDesc->configValue));
	auto intf = FRG_CO_TRY(co_await cfg.useInterface(intfNumber.value(), 0));
	endpoint_ = FRG_CO_TRY(co_await intf.getEndpoint(PipeType::in, endNumber.value()));

	// Read the hub class-specific descriptor.
	struct [[gnu::packed]] HubDescriptor : public DescriptorBase {
		uint8_t numPorts;
		uint16_t hubCharacteristics;
		uint8_t powerOnToPowerGood;
	};

	arch::dma_object<SetupPacket> getDescriptor{state()->setupPool()};
	getDescriptor->type = setup_type::targetDevice | setup_type::byClass
			| setup_type::toHost;
	getDescriptor->request = ClassRequests::getDescriptor;
	getDescriptor->value = 0x29 << 8;
	getDescriptor->index = intfNumber.value();
	getDescriptor->length = sizeof(HubDescriptor);

	arch::dma_object<HubDescriptor> hubDescriptor{state()->bufferPool()};
	FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToHost,
			getDescriptor, hubDescriptor.view_buffer()}));

	state_.resize(hubDescriptor->numPorts, PortState{0, 0});

	auto rawThinkTime = (hubDescriptor->hubCharacteristics >> 5) & 0b11;
	characteristics_.ttThinkTime = 8 * (1 + rawThinkTime);

	for (size_t port = 1; port <= hubDescriptor->numPorts; port++) {
		// Issue a SetPortFeature request to power on the port.
		arch::dma_object<SetupPacket> powerReq{state()->setupPool()};
		powerReq->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toDevice;
		powerReq->request = ClassRequests::setFeature;
		powerReq->value = PortFeatures::power;
		powerReq->index = port;
		powerReq->length = 0;

		FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToDevice,
						powerReq, arch::dma_buffer_view{}}));
	}

	// Wait for the ports to power on (time is specified in 2 ms units).
	// Linux waits for at least 100ms, even if the hub says less time is needed.
	auto durationMs = std::max(hubDescriptor->powerOnToPowerGood * 2, 100);
	co_await helix::sleepFor(durationMs * 1'000'000);

	run_();
	co_return {};
}

async::detached StandardHub::run_() {
	std::cout << "usb: Serving standard hub with "
			<< state_.size() << " ports." << std::endl;

	while(true) {
		arch::dma_array<uint8_t> report{state()->bufferPool(), (state_.size() + 1 + 7) / 8};
		(co_await endpoint_.transfer(InterruptTransfer{XferFlags::kXferToHost,
				report.view_buffer()})).unwrap();

//		std::cout << "usb: Hub report: " << (unsigned int)report[0] << std::endl;
		for(size_t port = 1; port <= state_.size(); port++) {
			if(!(report[port / 8] & (1 << (port % 8))))
				continue;

			// Query issue a GetPortStatus request and inspect the status.
			arch::dma_object<SetupPacket> statusReq{state()->setupPool()};
			statusReq->type = setup_type::targetOther | setup_type::byClass
					| setup_type::toHost;
			statusReq->request = ClassRequests::getStatus;
			statusReq->value = 0;
			statusReq->index = port;
			statusReq->length = 4;

			arch::dma_array<uint16_t> result{state()->bufferPool(), 2};
			(co_await state()->transfer(ControlTransfer{kXferToHost,
					statusReq, result.view_buffer()})).unwrap();
//			std::cout << "usb: Port " << port << " status: "
//					<< result[0] << ", " << result[1] << std::endl;

			state_[port - 1].status = 0;
			if(result[0] & PortBits::connect)
				state_[port - 1].status |= HubStatus::connect;
			if(result[0] & PortBits::enable)
				state_[port - 1].status |= HubStatus::enable;
			if(result[0] & PortBits::reset)
				state_[port - 1].status |= HubStatus::reset;

			// Inspect the status change bits and reset them.
			if(result[1] & PortBits::connect) {
				state_[port - 1].changes |= HubStatus::connect;
				doorbell_.raise();

				arch::dma_object<SetupPacket> clearReq{state()->setupPool()};
				clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clearReq->request = ClassRequests::clearFeature;
				clearReq->value = PortFeatures::connectChange;
				clearReq->index = port;
				clearReq->length = 0;

				(co_await state()->transfer(ControlTransfer{kXferToDevice,
						clearReq, arch::dma_buffer_view{}})).unwrap();
			}

			if(result[1] & PortBits::enable) {
				state_[port - 1].changes |= HubStatus::enable;
				doorbell_.raise();

				arch::dma_object<SetupPacket> clearReq{state()->setupPool()};
				clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clearReq->request = ClassRequests::clearFeature;
				clearReq->value = PortFeatures::enableChange;
				clearReq->index = port;
				clearReq->length = 0;

				(co_await state()->transfer(ControlTransfer{kXferToDevice,
						clearReq, arch::dma_buffer_view{}})).unwrap();
			}

			if(result[1] & PortBits::reset) {
				state_[port - 1].changes |= HubStatus::reset;
				doorbell_.raise();

				arch::dma_object<SetupPacket> clearReq{state()->setupPool()};
				clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clearReq->request = ClassRequests::clearFeature;
				clearReq->value = PortFeatures::resetChange;
				clearReq->index = port;
				clearReq->length = 0;

				(co_await state()->transfer(ControlTransfer{kXferToDevice,
						clearReq, arch::dma_buffer_view{}})).unwrap();
			}
		}
	}
}

size_t StandardHub::numPorts() {
	return state_.size();
}

async::result<PortState> StandardHub::pollState(int port) {
	while(true) {
		auto state = state_[port - 1];
		if(state.changes) {
			state_[port - 1].changes = 0;
			co_return state;
		}

		co_await doorbell_.async_wait();
	}
}

async::result<frg::expected<UsbError, void>> StandardHub::issueReset(int port) {
	// Issue a SetPortFeature request to reset the port.
	arch::dma_object<SetupPacket> resetReq{state()->setupPool()};
	resetReq->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toDevice;
	resetReq->request = ClassRequests::setFeature;
	resetReq->value = PortFeatures::reset;
	resetReq->index = port;
	resetReq->length = 0;

	FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToDevice,
			resetReq, arch::dma_buffer_view{}}));

	co_return frg::success;
}

async::result<frg::expected<UsbError, DeviceSpeed>> StandardHub::querySpeed(int port) {
	// Issue a GetPortStatus request to determine the device speed.
	arch::dma_object<SetupPacket> statusReq{state()->setupPool()};
	statusReq->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toHost;
	statusReq->request = ClassRequests::getStatus;
	statusReq->value = 0;
	statusReq->index = port;
	statusReq->length = 4;

	arch::dma_array<uint16_t> result{state()->bufferPool(), 2};
	FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToHost,
			statusReq, result.view_buffer()}));

	auto lowSpeed = result[0] & PortBits::lowSpeed;
	auto highSpeed = result[0] & PortBits::highSpeed;

	if (lowSpeed)
		co_return DeviceSpeed::lowSpeed;
	else if (highSpeed)
		co_return DeviceSpeed::highSpeed;
	else // TODO(qookie): What about SuperSpeed hubs?
		co_return DeviceSpeed::fullSpeed;
}

} // namespace anonymous

async::result<frg::expected<UsbError, std::shared_ptr<Hub>>>
createHubFromDevice(std::shared_ptr<DeviceServerData> device) {
	auto hub = std::make_shared<StandardHub>(std::move(device));
	FRG_CO_TRY(co_await hub->initialize());
	co_return hub;
}

} // namespace protocols::usb
