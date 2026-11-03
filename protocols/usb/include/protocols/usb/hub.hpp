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

	virtual frg::expected<UsbError, HubCharacteristics> getCharacteristics() {
		return UsbError::unsupported;
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

	void observeHub(std::shared_ptr<Hub> hub);

private:
	async::detached observePort_(std::shared_ptr<Hub> hub, int port);
	async::result<void> observationCycle_(std::shared_ptr<Hub> hub, int port);

	async::result<frg::expected<UsbError>>
	enumerateDevice_(std::shared_ptr<DeviceServerData> device);

	BaseController *controller_;
	async::mutex enumerateMutex_;
};

} // namespace protocols::usb
