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

async::detached Enumerator::observeHub(std::shared_ptr<Hub> hub) {
	bool anyPowerChange = false;

	for (size_t port = 1; port <= hub->numPorts(); port++) {
		if (auto st = co_await hub->getPortPower(port); !st)
			std::println("usb: Failed to get port {} power state", port);
		else if (*st) // Skip already powered on ports
			continue;

		anyPowerChange = true;

		if (auto st = co_await hub->setPortPower(port, true); !st)
			std::println("usb: Failed to power on port {}", port);
	}

	if (anyPowerChange) {
		auto characteristics = hub->getCharacteristics();
		co_await helix::sleepFor(characteristics.powerOnToPowerGood * 1'000'000);
	}

	hub->run();

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
	// TODO(qookie): Deal with errors.
	(void)(co_await hub->pollUntilState(port, HubStatus::connect));

	co_await enumerateMutex_.async_lock();
	enumerateLock = std::unique_lock<async::mutex>{enumerateMutex_, std::adopt_lock};

	if (hub->hubSpeed() != DeviceSpeed::superSpeed) {
		std::cout << "usb: Issuing reset on port " << port << std::endl;

		if (auto v = co_await hub->issueReset(port); !v) {
			std::cout << "usb: Device on port " << port << " failed to reset: "
				  << (int)v.error() << std::endl;
			co_return;
		}
	}

	std::cout << "usb: Waiting for device to become enabled on port " << port << std::endl;

	// Wait until the device is enabled.
	// TODO(qookie): Deal with errors.
	(void)(co_await hub->pollUntilState(port, HubStatus::enable));

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

	// TODO(qookie): Wait until the device is disconnected.
	co_await std::suspend_always{};
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
static constexpr uint8_t setHubDepth = 12;
} // namespace ClassRequests

namespace PortStatusBits {
static constexpr uint16_t connect = 0x01;
static constexpr uint16_t enable = 0x02;
static constexpr uint16_t reset = 0x10;
// LS/FS/HS-specific bits
static constexpr uint16_t power2 = 0x100;
static constexpr uint16_t lowSpeed = 0x200;
static constexpr uint16_t highSpeed = 0x400;
// SS-specific bits
static constexpr uint16_t power3 = 0x200;
} // namespace PortStatusBits

namespace PortChangeBits {
static constexpr uint16_t connect = 0x01;
static constexpr uint16_t reset = 0x10;
// LS/FS/HS-specific bits
static constexpr uint16_t enable = 0x02;
// SS-specific bits
static constexpr uint16_t bhReset = 0x20;
static constexpr uint16_t linkState = 0x40;
static constexpr uint16_t configError = 0x80;
} // namespace PortChangeBits


namespace PortFeatures {
//static constexpr uint16_t connect = 0;
//static constexpr uint16_t enable = 1;
static constexpr uint16_t reset = 4;
static constexpr uint16_t power = 8;
static constexpr uint16_t connectChange = 16;
static constexpr uint16_t enableChange = 17;
static constexpr uint16_t resetChange = 20;
// SS-specific bits
static constexpr uint16_t linkStateChange = 25;
static constexpr uint16_t configErrorChange = 26;
static constexpr uint16_t bhResetChange = 29;
} // namespace PortFeatures

struct StandardHub final : Hub {
	StandardHub(std::shared_ptr<DeviceServerData> device)
	: Hub{std::move(device)}, endpoint_{nullptr} { }

	async::result<frg::expected<UsbError>> initialize();
	async::detached run() override;

private:
	HubCharacteristics characteristics_;

public:
	size_t numPorts() override;
	async::result<frg::expected<UsbError, void>> pollUntilState(int port, uint32_t desired) override;
	async::result<frg::expected<UsbError, void>> issueReset(int port) override;
	async::result<frg::expected<UsbError, DeviceSpeed>> querySpeed(int port) override;
	async::result<frg::expected<UsbError, void>> setPortPower(int port, bool state) override;
	async::result<frg::expected<UsbError, bool>> getPortPower(int port) override;

	HubCharacteristics getCharacteristics() override {
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
	getDescriptor->value = (hubSpeed() == DeviceSpeed::superSpeed ? 0x2A : 0x29) << 8;
	getDescriptor->index = 0;
	getDescriptor->length = sizeof(HubDescriptor);

	arch::dma_object<HubDescriptor> hubDescriptor{state()->bufferPool()};
	FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToHost,
			getDescriptor, hubDescriptor.view_buffer()}));

	state_.resize(hubDescriptor->numPorts, PortState{0});

	auto rawThinkTime = (hubDescriptor->hubCharacteristics >> 5) & 0b11;
	// Wait for the ports to power on (time is specified in 2 ms units).
	// Linux waits for at least 100ms, even if the hub says less time is needed.
	auto durationMs = std::max(hubDescriptor->powerOnToPowerGood * 2, 100);

	characteristics_.ttThinkTime = 8 * (1 + rawThinkTime);
	characteristics_.powerOnToPowerGood = durationMs;

	if (hubSpeed() == DeviceSpeed::superSpeed) {
		int depth = 0;
		auto parent = state()->parent();
		while (!parent->rootHub()) {
			depth++;
			parent = parent->state()->parent();
		}

		// Issue a SetHubDepth request to configure the hub.
		arch::dma_object<SetupPacket> depthReq{state()->setupPool()};
		depthReq->type = setup_type::targetDevice | setup_type::byClass
			| setup_type::toDevice;
		depthReq->request = ClassRequests::setHubDepth;
		depthReq->value = depth;
		depthReq->index = 0;
		depthReq->length = 0;

		FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToDevice,
						depthReq, arch::dma_buffer_view{}}));
	}

	co_return {};
}

async::detached StandardHub::run() {
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

			auto handleChangeBit = [&] (auto portBit, auto feature) -> async::result<void> {
				if (result[1] & portBit) {
					doorbell_.raise();

					arch::dma_object<SetupPacket> clearReq{state()->setupPool()};
					clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
					clearReq->request = ClassRequests::clearFeature;
					clearReq->value = feature;
					clearReq->index = port;
					clearReq->length = 0;

					auto res = co_await state()->transfer(ControlTransfer{kXferToDevice,
								clearReq, arch::dma_buffer_view{}});

					if (!res) {
						std::println("usb: Failed to clear hub feature {:04x}: {}", feature, (int)res.error());
					}
				}
			};

			state_[port - 1] = 0;
			if(result[0] & PortStatusBits::connect)
				state_[port - 1] |= HubStatus::connect;
			if(result[0] & PortStatusBits::enable)
				state_[port - 1] |= HubStatus::enable;
			if(result[0] & PortStatusBits::reset)
				state_[port - 1] |= HubStatus::reset;

			// Inspect the status change bits and reset them.
			co_await handleChangeBit(PortChangeBits::connect, PortFeatures::connectChange);
			// USB SS hubs do not have enable change bit/feature
			if (hubSpeed() != DeviceSpeed::superSpeed)
				co_await handleChangeBit(PortChangeBits::enable, PortFeatures::enableChange);
			co_await handleChangeBit(PortChangeBits::reset, PortFeatures::resetChange);
			co_await handleChangeBit(PortChangeBits::bhReset, PortFeatures::bhResetChange);
			co_await handleChangeBit(PortChangeBits::linkState, PortFeatures::linkStateChange);
			co_await handleChangeBit(PortChangeBits::configError, PortFeatures::configErrorChange);
		}
	}
}

size_t StandardHub::numPorts() {
	return state_.size();
}

async::result<frg::expected<UsbError, void>> StandardHub::pollUntilState(int port, uint32_t desired) {
	auto &state = state_[port - 1];
	while (true) {
		// TODO(qookie): Handle disconnect and errors.
		if ((state & desired) == desired) {
			co_return frg::success;
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

	// Preemptively clear enable status bit to avoid racing `observationCycle_()`.
	state_[port - 1] &= ~HubStatus::enable;

	FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToDevice,
			resetReq, arch::dma_buffer_view{}}));

	co_return frg::success;
}

async::result<frg::expected<UsbError, DeviceSpeed>> StandardHub::querySpeed(int port) {
	// USB SS hubs will always have SS devices behind them.
	if (hubSpeed() == DeviceSpeed::superSpeed)
		co_return DeviceSpeed::superSpeed;

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

	auto lowSpeed = result[0] & PortStatusBits::lowSpeed;
	auto highSpeed = result[0] & PortStatusBits::highSpeed;

	if (lowSpeed)
		co_return DeviceSpeed::lowSpeed;
	else if (highSpeed)
		co_return DeviceSpeed::highSpeed;
	else
		co_return DeviceSpeed::fullSpeed;
}

async::result<frg::expected<UsbError, void>> StandardHub::setPortPower(int port, bool powerState) {
	// Issue a SetPortFeature/ClearPortFeature request to change port power state.
	arch::dma_object<SetupPacket> powerReq{state()->setupPool()};
	powerReq->type = setup_type::targetOther | setup_type::byClass
		| setup_type::toDevice;
	powerReq->request = powerState ? ClassRequests::setFeature : ClassRequests::clearFeature;
	powerReq->value = PortFeatures::power;
	powerReq->index = port;
	powerReq->length = 0;

	FRG_CO_TRY(co_await state()->transfer(ControlTransfer{kXferToDevice,
						powerReq, arch::dma_buffer_view{}}));

	co_return frg::success;
}

async::result<frg::expected<UsbError, bool>> StandardHub::getPortPower(int port) {
	// Issue a GetPortStatus request to query port power.
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

	if (hubSpeed() == DeviceSpeed::superSpeed)
		co_return result[0] & PortStatusBits::power3;
	co_return result[0] & PortStatusBits::power2;
}

} // namespace anonymous

async::result<frg::expected<UsbError, std::shared_ptr<Hub>>>
createHubFromDevice(std::shared_ptr<DeviceServerData> device) {
	auto hub = std::make_shared<StandardHub>(std::move(device));
	FRG_CO_TRY(co_await hub->initialize());
	co_return hub;
}

} // namespace protocols::usb
