#include <string>
#include <locale>
#include <codecvt>

#include "protocols/usb/api.hpp"
#include "protocols/usb/hub.hpp"

namespace protocols::usb {

// ----------------------------------------------------------------------------
// Device.
// ----------------------------------------------------------------------------

Device::Device(std::shared_ptr<DeviceData> state)
: _state(std::move(state)) { }

arch::dma_pool *Device::setupPool() const {
	return _state->setupPool();
}

arch::dma_pool *Device::bufferPool() const {
	return _state->bufferPool();
}

async::result<frg::expected<UsbError, std::string>> Device::deviceDescriptor() const {
	return _state->deviceDescriptor();
}

async::result<frg::expected<UsbError, std::string>> Device::configurationDescriptor(uint8_t configuration) const {
	return _state->configurationDescriptor(configuration);
}

async::result<frg::expected<UsbError, uint8_t>> Device::currentConfigurationValue() const {
	arch::dma_object<SetupPacket> get{setupPool()};
	get->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get->request = request_type::getConfig;
	get->value = 0;
	get->index = 0;
	get->length = 1;

	arch::dma_object<uint8_t> descriptor{bufferPool()};
	FRG_CO_TRY(co_await transfer(ControlTransfer{kXferToHost,
			get, descriptor.view_buffer()}));

	co_return *descriptor.data();
}

async::result<frg::expected<UsbError, Configuration>> Device::useConfiguration(uint8_t index, uint8_t value) const {
	return _state->useConfiguration(index, value);
}

async::result<frg::expected<UsbError, std::string>> Device::getString(size_t number) const {
	if(number == 0)
		co_return UsbError::unsupported;

	arch::dma_object<SetupPacket> desc{setupPool()};
	desc->type = setup_type::targetDevice | setup_type::byStandard | setup_type::toHost;
	desc->request = request_type::getDescriptor;
	desc->value = (descriptor_type::string << 8) | number;
	desc->index = 0x0409; // en-us
	desc->length = sizeof(StringDescriptor);

	arch::dma_object<StringDescriptor> header{bufferPool()};

	FRG_CO_TRY(co_await transfer(ControlTransfer(kXferToHost, desc, header.view_buffer())));

	desc->length = header->length;
	arch::dma_buffer buffer{bufferPool(), header->length};
	FRG_CO_TRY(co_await transfer(ControlTransfer(kXferToHost, desc, buffer)));

	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;
	auto res = reinterpret_cast<StringDescriptor *>(buffer.data());
	co_return convert.to_bytes(std::u16string{res->data, (res->length - sizeof(StringDescriptor)) / 2});
}

async::result<frg::expected<UsbError, size_t>> Device::transfer(ControlTransfer info) const {
	return _state->transfer(info);
}

// ----------------------------------------------------------------------------
// DeviceServerData.
// ----------------------------------------------------------------------------

std::tuple<std::shared_ptr<Hub>, int> DeviceServerData::nearestTTHub() const {
	// Only LS and FS devices use TTs.
	if (speed() != DeviceSpeed::lowSpeed && speed() != DeviceSpeed::fullSpeed)
		return {nullptr, -1};

	auto curHub = parent();
	auto curPort = port();
	while (!curHub->rootHub()) {
		auto hubDevice = curHub->state();

		if (hubDevice->speed() == DeviceSpeed::highSpeed)
			break;

		assert(hubDevice->speed() == DeviceSpeed::lowSpeed
				|| hubDevice->speed() == DeviceSpeed::fullSpeed);

		curPort = hubDevice->port();
		curHub = hubDevice->parent();
	}

	// Root hubs don't count as TTs.
	if (curHub->rootHub()) {
		return {nullptr, -1};
	}

	auto hubDevice = curHub->state();
	assert(hubDevice->speed() == DeviceSpeed::highSpeed);
	return {curHub, curPort};
}

std::tuple<uint32_t, std::shared_ptr<Hub>, int>
DeviceServerData::routeString() const {
	uint32_t route = 0;

	std::shared_ptr<Hub> curHub = parent();
	int curPort = port();
	while (!curHub->rootHub()) {
		auto hubDevice = curHub->state();

		// XHCI 6.2.2 Slot Context:
		// If HS or FS hub in the path supports more than 14 ports the
		// associated Route String Port field shall be set to 15.
		if (curPort > 15)
			curPort = 15;

		route <<= 4;
		route |= curPort;

		curPort = hubDevice->port();
		curHub = hubDevice->parent();
	}

	return std::make_tuple(route, curHub, curPort);
}

// ----------------------------------------------------------------------------
// Configuration.
// ----------------------------------------------------------------------------

Configuration::Configuration(std::shared_ptr<ConfigurationData> state)
: _state(std::move(state)) { }

async::result<frg::expected<UsbError, Interface>> Configuration::useInterface(int number,
		int alternative) const {
	return _state->useInterface(number, alternative);
}

// ----------------------------------------------------------------------------
// Interface.
// ----------------------------------------------------------------------------

Interface::Interface(std::shared_ptr<InterfaceData> state)
: _state(std::move(state)) { }

async::result<frg::expected<UsbError, Endpoint>>
Interface::getEndpoint(PipeType type, int number) const {
	return _state->getEndpoint(type, number);
}

// ----------------------------------------------------------------------------
// Endpoint.
// ----------------------------------------------------------------------------

Endpoint::Endpoint(std::shared_ptr<EndpointData> state)
: _state(std::move(state)) { }

async::result<frg::expected<UsbError, size_t>> Endpoint::transfer(InterruptTransfer info) const {
	return _state->transfer(info);
}

async::result<frg::expected<UsbError, size_t>> Endpoint::transfer(BulkTransfer info) const {
	return _state->transfer(info);
}

} // namespace protocols::usb
