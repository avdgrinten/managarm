#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <nic/usb_net/usb_net.hpp>
#include <net/ethernet.h>

#include "usb-ecm.hpp"
#include "usb-ncm.hpp"
#include "usb-net.hpp"

namespace nic::usb_ncm {

constexpr bool debugNcm = false;

UsbNcmNic::UsbNcmNic(mbus_ng::EntityId entity, protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface data_intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out,
		size_t config_index)
	: UsbNic{hw_device, mac, ctrl_intf, ctrl_ep, data_intf, in, out},
		entity_{entity}, config_index_{config_index} {

}

async::result<void> UsbNcmNic::initialize() {
	auto config_val = (co_await device_.currentConfigurationValue()).value();

	mbus_ng::Properties descriptor = {
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity_)}},
		{"usb.interface_drivers", mbus_ng::ArrayItem{{
			mbus_ng::ArrayItem{{
				mbus_ng::StringItem{std::format("{}.{}", config_val, ctrl_intf_.num())},
				mbus_ng::StringItem{"cdc_ncm"},
			}},
			mbus_ng::ArrayItem{{
				mbus_ng::StringItem{std::format("{}.{}", config_val, data_intf_.num())},
				mbus_ng::StringItem{"cdc_ncm"},
			}},
		}}},
	};

	auto device_entity = (co_await mbus_ng::Instance::global().createEntity("usb-ncm", descriptor)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));
		}
	}(std::move(device_entity));

	mbus_ng::Properties netProperties{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity_)}},
		{"unix.subsystem", mbus_ng::StringItem{"net"}},
		{"usb.parent-interface", mbus_ng::StringItem{std::format("{}.{}", config_val, ctrl_intf_.num())},}
	};
	netProperties.merge(mbusNetworkProperties());

	auto netClassEntity = (co_await mbus_ng::Instance::global().createEntity(
		"net", netProperties)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));
		}
	}(std::move(netClassEntity));

	std::optional<protocols::usb::CdcEthernetNetworking> ecm_hdr;
	std::optional<protocols::usb::CdcNcm> ncm_hdr;
	auto raw_descs = (co_await device_.configurationDescriptor(config_index_)).value();

	for(const auto &[head, body] : protocols::usb::configurationRange(raw_descs)) {
		if(head.descriptorType != protocols::usb::descriptor_type::cs_interface)
			continue;

		auto desc = protocols::usb::extractDescriptor<protocols::usb::CdcDescriptor>(body);
		if(!desc)
			continue;
		switch(desc->subtype) {
			using CdcSubType = protocols::usb::CdcDescriptor::CdcSubType;

			case CdcSubType::EthernetNetworking: {
				ecm_hdr = protocols::usb::extractDescriptor<protocols::usb::CdcEthernetNetworking>(body);
				break;
			}
			case CdcSubType::Ncm: {
				ncm_hdr = protocols::usb::extractDescriptor<protocols::usb::CdcNcm>(body);
				break;
			}
			default: {
				break;
			}
		}
	}

	if(!ecm_hdr || !ncm_hdr) {
		std::cout << "usb-ncm: Missing ECM/NCM functional descriptors" << std::endl;
		co_return;
	}

	// wMaxSegmentSize includes MTU and the ethernet header, but not CRC
	max_mtu = ecm_hdr->wMaxSegmentSize - sizeof(ether_header);
	mtu = max_mtu;

	if(ncm_hdr->bmNetworkCapabilities & regs::bmNetworkCapabilities::maxDatagramSize)
		min_mtu = 68;
	else
		min_mtu = mtu;

	arch::dma_object<protocols::usb::SetupPacket> ctrl_msg{&dmaPool_};
	arch::dma_object<NtbParameter> params{&dmaPool_};
	ctrl_msg->type = protocols::usb::setup_type::byClass |
					protocols::usb::setup_type::toHost | protocols::usb::setup_type::targetInterface;
	ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::GET_NTB_PARAMETERS);
	ctrl_msg->value = 0;
	ctrl_msg->index = ctrl_intf_.num();
	ctrl_msg->length = params.view_buffer().size();

	auto res = co_await device_.transfer(protocols::usb::ControlTransfer{
		protocols::usb::kXferToHost, ctrl_msg, params.view_buffer()
	});
	assert(res);

	if(debugNcm)
		std::cout << std::format("{}", *params) << std::endl;

	// NTB16 cannot exceed 64 KiB; the device sends NTBs of at most dwNtbInMaxSize bytes.
	ntbInSize_ = std::clamp<size_t>(params->dwNtbInMaxSize, 2048, 0xFFFF);

	if(ncm_hdr->bmNetworkCapabilities & regs::bmNetworkCapabilities::crcMode) {
		ctrl_msg->type = protocols::usb::setup_type::byClass | protocols::usb::setup_type::targetInterface;
		ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::SET_CRC_MODE);
		// CRC shall not be appended
		ctrl_msg->value = 0;
		ctrl_msg->index = ctrl_intf_.num();
		ctrl_msg->length = 0;

		res = co_await device_.transfer(protocols::usb::ControlTransfer{
			protocols::usb::kXferToDevice, ctrl_msg, {}
		});
		assert(res);
	}

	if(ncm_hdr->bmNetworkCapabilities & regs::bmNetworkCapabilities::setEthernetPacketFilter) {
		ctrl_msg->type = protocols::usb::setup_type::byClass | protocols::usb::setup_type::targetInterface;
		ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::SET_ETHERNET_PACKET_FILTER);
		ctrl_msg->index = ctrl_intf_.num();
		ctrl_msg->length = 0;

		{
			namespace packet_type = nic::usb_ecm::regs::setEthernetPacketFilter;

			ctrl_msg->value = (
				packet_type::promiscuous(1) |
				packet_type::all_multicast(1) |
				packet_type::directed(1) |
				packet_type::broadcast(1) |
				packet_type::multicast(0)
			).bits();

			promiscuous_ = true;
			all_multicast_ = true;
			multicast_ = true;
			broadcast_ = true;
		}

		res = co_await device_.transfer(protocols::usb::ControlTransfer{
			protocols::usb::kXferToDevice, ctrl_msg, {}
		});
		assert(res);
	}

	listenForNotifications();
	receiveFrames();
}

async::detached UsbNcmNic::listenForNotifications() {
	using NotificationHeader = protocols::usb::CdcNotificationHeader;

	while(true) {
		arch::dma_buffer report{device_.bufferPool(), 16};
		protocols::usb::InterruptTransfer transfer{protocols::usb::XferFlags::kXferToHost, report};
		transfer.allowShortPackets = true;
		auto length = (co_await ctrl_ep_.transfer(transfer)).unwrap();

		assert(length >= sizeof(NotificationHeader));

		auto notification = reinterpret_cast<NotificationHeader *>(report.data());

		switch(notification->bNotificationCode) {
			using Notification = NotificationHeader::Notification;

			case Notification::NETWORK_CONNECTION:
				l1_up_ = (notification->wValue == 1);
				break;
			case Notification::CONNECTION_SPEED_CHANGE: {
				auto change = reinterpret_cast<protocols::usb::CdcConnectionSpeedChange *>(report.subview(sizeof(protocols::usb::CdcNotificationHeader)).data());
				printf("netserver: connection speed %u MBit/s\n", change->DlBitRate / 1000 / 1000);
				break;
			}
			default: {
				printf("netserver: received notification 0x%x\n", uint8_t(notification->bNotificationCode));
				break;
			}
		}
	}

	co_return;
}

void feedNtb(nic::Link &link, arch::dma_buffer_view ntb, uint32_t ndpSignature) {
	// Bounds the walk in case the NDPs of a malformed NTB form a cycle.
	constexpr int maxNdps = 16;

	auto bytes = static_cast<const uint8_t *>(ntb.data());
	auto load16 = [&] (size_t offset) {
		uint16_t value;
		memcpy(&value, bytes + offset, sizeof(value));
		return value;
	};
	auto load32 = [&] (size_t offset) {
		uint32_t value;
		memcpy(&value, bytes + offset, sizeof(value));
		return value;
	};

	if(ntb.size() < sizeof(NcmTransferHeader)
			|| load32(offsetof(NcmTransferHeader, dwSignature)) != NCM_NTH16_SIGNATURE) {
		printf("usb-ncm: Dropping NTB without a valid header\n");
		return;
	}

	size_t ndpIndex = load16(offsetof(NcmTransferHeader, wNdpIndex));
	for(int n = 0; ndpIndex && n < maxNdps; n++) {
		constexpr size_t entriesOffset = offsetof(NcmDatagramPointer, wDatagram);
		if(ndpIndex + entriesOffset > ntb.size()
				|| load32(ndpIndex + offsetof(NcmDatagramPointer, dwSignature)) != ndpSignature) {
			printf("usb-ncm: Dropping the rest of an NTB with an invalid NDP\n");
			return;
		}

		size_t ndpLength = load16(ndpIndex + offsetof(NcmDatagramPointer, wLength));
		if(ndpLength < entriesOffset || ndpIndex + ndpLength > ntb.size()) {
			printf("usb-ncm: Dropping the rest of an NTB with an invalid NDP length\n");
			return;
		}

		// The datagram list is terminated by a zero entry.
		for(size_t entry = ndpIndex + entriesOffset; entry + 4 <= ndpIndex + ndpLength; entry += 4) {
			size_t index = load16(entry);
			size_t length = load16(entry + 2);
			if(!index || !length)
				break;
			if(index + length > ntb.size()) {
				printf("usb-ncm: Dropping datagram beyond the end of its NTB\n");
				continue;
			}
			link.feedPacket(ntb.subview(index, length));
		}

		ndpIndex = load16(ndpIndex + offsetof(NcmDatagramPointer, wNextNdpIndex));
	}
}

async::detached UsbNcmNic::receiveFrames() {
	arch::dma_buffer buffer{&dmaPool_, ntbInSize_};

	while(true) {
		auto res = co_await data_in_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToHost, buffer});
		assert(res);

		if(res.value() != 0)
			feedNtb(*this, buffer.subview(0, res.value()), NCM_NDP16_NO_CRC_SIGNATURE);
	}
}

async::result<void> UsbNcmNic::send(const arch::dma_buffer_view payload) {
	arch::dma_buffer buf{&dmaPool_, sizeof(NcmTransferHeader) + sizeof(NcmDatagramPointer) + payload.size()};
	auto ncmHeader = reinterpret_cast<NcmTransferHeader *>(buf.data());
	ncmHeader->dwSignature = NCM_NTH16_SIGNATURE;
	ncmHeader->wHeaderLength = sizeof(*ncmHeader);
	ncmHeader->wSequence = seq_++;
	ncmHeader->wBlockLength = uint16_t(buf.size());
	ncmHeader->wNdpIndex = sizeof(*ncmHeader);

	auto ndp = reinterpret_cast<NcmDatagramPointer *>(buf.subview(ncmHeader->wNdpIndex).data());
	ndp->dwSignature = NCM_NDP16_NO_CRC_SIGNATURE;
	ndp->wLength = sizeof(*ndp);
	ndp->wNextNdpIndex = 0;
	ndp->wDatagram[0].Index = ncmHeader->wHeaderLength + ndp->wLength;
	ndp->wDatagram[0].Length = uint16_t(payload.size());
	ndp->wDatagram[1].Index = 0;
	ndp->wDatagram[1].Length = 0;

	memcpy(buf.subview(ndp->wDatagram[0].Index).data(), payload.data(), payload.size());

	auto res = co_await data_out_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToDevice, buf});
	assert(res);
	co_return;
}

} // namespace nic::usb_ncm
