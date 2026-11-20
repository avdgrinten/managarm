#pragma once

#include <arch/bits.hpp>
#include <format>
#include <protocols/mbus/client.hpp>

#include "usb-net.hpp"

namespace nic::usb_ncm {

constexpr uint32_t NCM_NTH16_SIGNATURE = 0x484D434E;
constexpr uint32_t NCM_NDP16_NO_CRC_SIGNATURE = 0x304D434E;

struct [[gnu::packed]] NcmTransferHeader {
	uint32_t dwSignature;
	uint16_t wHeaderLength;
	uint16_t wSequence;
	uint16_t wBlockLength;
	uint16_t wNdpIndex;
};

struct [[gnu::packed]] NcmDatagramPointer {
	uint32_t dwSignature;
	uint16_t wLength;
	uint16_t wNextNdpIndex;
	struct [[gnu::packed]] {
		uint16_t Index;
		uint16_t Length;
	} wDatagram[2];
};

struct NtbParameter {
	uint16_t wLength;
	uint16_t bmNtbFormatsSupported;
	uint32_t dwNtbInMaxSize;
	uint16_t wNdpInDivisor;
	uint16_t wNdpInPayloadRemainder;
	uint16_t wNdpInAlignment;
	uint16_t reserved;
	uint32_t dwNtbOutMaxSize;
	uint16_t wNdpOutDivisor;
	uint16_t wNdpOutPayloadRemainder;
	uint16_t wNdpOutAlignment;
	uint16_t wNtbOutMaxDatagrams;
};

struct UsbNcmNic : UsbNic {
	UsbNcmNic(mbus_ng::EntityId entity, protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out,
		size_t config_index);

	async::result<void> initialize() override;
	async::detached listenForNotifications() override;

	async::result<void> send(const arch::dma_buffer_view) override;
private:
	async::detached receiveFrames();

	mbus_ng::EntityId entity_;
	size_t config_index_;
	size_t ntbInSize_ = 0;
};

// Feeds every datagram of an NTB16 whose NDPs carry ndpSignature to the link.
void feedNtb(nic::Link &link, arch::dma_buffer_view ntb, uint32_t ndpSignature);

namespace regs {

// NCM 1.0 5.2.1 Table 5-2
namespace bmNetworkCapabilities {
	constexpr arch::field<uint8_t, uint8_t> setEthernetPacketFilter{0, 1};
	constexpr arch::field<uint8_t, uint8_t> netAddress{1, 1};
	constexpr arch::field<uint8_t, uint8_t> encapsulatedCommand{2, 1};
	constexpr arch::field<uint8_t, uint8_t> maxDatagramSize{3, 1};
	constexpr arch::field<uint8_t, uint8_t> crcMode{4, 1};
	constexpr arch::field<uint8_t, uint8_t> ntbInputSize{5, 1};
} // namespace bmNetworkCapabilities

} // namespace regs

} // namespace nic::usb_ncm

template<>
struct std::formatter<nic::usb_ncm::NtbParameter> : std::formatter<std::string_view> {
	auto format(const nic::usb_ncm::NtbParameter& p, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "NTB Parameters:\n"
			"\tIN maxsize {} divisor {} payload_remainder {} alignment {}\n"
			"\tOUT maxsize {} divisor {} payload_remainder {} alignment {}\n"
			"\tOUT max datagrams {}{}{}",
			p.dwNtbInMaxSize, p.wNdpInDivisor, p.wNdpInPayloadRemainder, p.wNdpInAlignment,
			p.dwNtbOutMaxSize, p.wNdpOutDivisor, p.wNdpOutPayloadRemainder, p.wNdpOutAlignment,
			p.wNtbOutMaxDatagrams,
			(p.bmNtbFormatsSupported & 1) ? ", 16-bit NTB support" : "",
			(p.bmNtbFormatsSupported & 2) ? ", 32-bit NTB support" : ""
		);
	}
};
