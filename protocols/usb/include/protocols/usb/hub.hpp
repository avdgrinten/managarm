#pragma once

#include <memory>

#include <arch/dma_structs.hpp>
#include <async/result.hpp>
#include <async/mutex.hpp>
#include <frg/expected.hpp>

#include "usb.hpp"
#include "api.hpp"

#include <protocols/mbus/client.hpp>

namespace protocols::usb {

// ----------------------------------------------------------------
// Hub.
// ----------------------------------------------------------------

namespace HubStatus {
	static constexpr uint32_t connect = 0x01;
	static constexpr uint32_t enable = 0x02;
	static constexpr uint32_t reset = 0x04;
}

using PortState = uint32_t;

struct HubCharacteristics {
	int ttThinkTime; // In FS bit times
	int powerOnToPowerGood; // In milliseconds
};

struct Hub {
protected:
	~Hub() = default;

public:
	Hub(std::shared_ptr<DeviceServerData> state)
	: state_{state} { }

	virtual size_t numPorts() = 0;
	// Wait for the given port to have the desired state flags set.
	// Fails with an error when the device disconnects or enters an error state instead.
	virtual async::result<frg::expected<UsbError, void>> pollUntilState(int port, uint32_t desired) = 0;
	virtual async::result<frg::expected<UsbError, void>> issueReset(int port) = 0;
	virtual async::result<frg::expected<UsbError, DeviceSpeed>> querySpeed(int port) = 0;
	virtual async::result<frg::expected<UsbError, void>> setPortPower(int port, bool state) = 0;
	virtual async::result<frg::expected<UsbError, bool>> getPortPower(int port) = 0;

	virtual HubCharacteristics getCharacteristics() {
		return HubCharacteristics{
			.ttThinkTime = 0,
			.powerOnToPowerGood = hubSpeed() == DeviceSpeed::superSpeed ? 100 : 20,
		};
	}

	virtual async::detached run() = 0;

	virtual DeviceSpeed hubSpeed() {
		assert(state_ && "Root hub should override Hub::hubSpeed!");
		return state_->speed();
	}

	std::shared_ptr<DeviceServerData> state() const {
		return state_;
	}

	bool rootHub() const {
		return state_ == nullptr;
	}

	virtual mbus_ng::EntityId mbusEntityId() {
		assert(!"Unimplemented");
	}

private:
	std::shared_ptr<DeviceServerData> state_;
};

async::result<frg::expected<UsbError, std::shared_ptr<Hub>>>
createHubFromDevice(std::shared_ptr<DeviceServerData> device);

// ----------------------------------------------------------------
// Enumerator.
// ----------------------------------------------------------------

struct Enumerator {
	Enumerator(BaseController *controller)
	: controller_{controller} { }

	async::detached observeHub(std::shared_ptr<Hub> hub);

private:
	async::detached observePort_(std::shared_ptr<Hub> hub, int port);
	async::result<void> observationCycle_(std::shared_ptr<Hub> hub, int port);

	async::result<frg::expected<UsbError>>
	enumerateDevice_(std::shared_ptr<DeviceServerData> device);

	BaseController *controller_;
	async::mutex enumerateMutex_;
};

} // namespace protocols::usb
