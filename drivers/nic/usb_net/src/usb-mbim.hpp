#pragma once

#include <async/recurring-event.hpp>
#include <async/queue.hpp>
#include <protocols/mbus/client.hpp>
#include <vector>
#include <smarter.hpp>

#include "usb-net.hpp"

namespace nic::usb_mbim {

struct UsbMbimNic;

struct CdcWdmDevice {
	struct HandleIoctl;

	UsbMbimNic *nic;

	bool nonBlock_ = false;
};

struct UsbMbimNic : UsbNic {
	friend CdcWdmDevice;

	UsbMbimNic(mbus_ng::EntityId entity, protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out,
		size_t config_index);

	async::result<void> initialize() override;
	async::detached receiveEncapsulated();
	async::detached listenForNotifications() override;

	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> writeCommand(arch::dma_buffer_view request);
private:
	async::detached receiveFrames();

	mbus_ng::EntityId entity_;
	size_t config_index_;

	async::recurring_event response_available_;

	struct PacketInfo {
		PacketInfo(arch::dma_buffer buffer, size_t result_length)
		: buffer_{std::move(buffer)}, size_{result_length} {}

		size_t size() const {
			return size_;
		}

		arch::dma_buffer_view view() {
			return buffer_.subview(0, size());
		}

	private:
		arch::dma_buffer buffer_;
		size_t size_;
	};

	smarter::shared_ptr<CdcWdmDevice> cdcWdmDev_;

public:
	uint16_t wMaxControlMessage = 0;

	async::queue<PacketInfo, frg::stl_allocator> queue_;

	async::recurring_event statusBell_;
	uint64_t currentSeq_ = 0;
	uint64_t inSeq_ = 0;
};

} // namespace nic::usb_mbim
