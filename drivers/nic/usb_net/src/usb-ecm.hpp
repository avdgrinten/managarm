#pragma once

#include "usb-net.hpp"

namespace nic::usb_ecm {

struct UsbEcmNic : UsbNic {
	UsbEcmNic(mbus_ng::EntityId entity, protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out);

	async::result<void> initialize() override;
	async::detached listenForNotifications() override;

	async::result<void> send(const arch::dma_buffer_view) override;
private:
	async::detached receiveFrames();

	mbus_ng::EntityId entity_;
};

namespace regs {

namespace setEthernetPacketFilter {

constexpr arch::field<uint16_t, uint8_t> promiscuous{0, 1};
constexpr arch::field<uint16_t, uint8_t> all_multicast{1, 1};
constexpr arch::field<uint16_t, uint8_t> directed{2, 1};
constexpr arch::field<uint16_t, uint8_t> broadcast{3, 1};
constexpr arch::field<uint16_t, uint8_t> multicast{4, 1};

} // namespace setEthernetPacketFilter

} // namespace regs

} // namespace nic::usb_ecm
