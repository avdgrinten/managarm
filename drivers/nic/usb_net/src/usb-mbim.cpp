#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <fcntl.h>
#include <format>
#include <linux/usb/cdc-wdm.h>
#include <nic/usb_net/usb_net.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>
#include <smarter.hpp>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "usb-ncm.hpp"
#include "usb-mbim.hpp"
#include "usb-net.hpp"

namespace nic::usb_mbim {

constexpr uint32_t NCM_NDP16_IPS_SIGNATURE = 0x00535049; /* IPS<sessionID> */

static async::result<void> setFileFlags(void *object, int flags) {
	auto self = static_cast<CdcWdmDevice *>(object);

	if(flags & ~O_NONBLOCK) {
		std::cout << std::format("netserver: setFileFlags with unknown flags 0x{:x}\n", flags);
		co_return;
	}

	if(flags & O_NONBLOCK)
		self->nonBlock_ = true;
	else
		self->nonBlock_ = false;
	co_return;
}

static async::result<int> getFileFlags(void *object) {
	auto self = static_cast<CdcWdmDevice *>(object);
	int flags = O_RDWR;

	if(self->nonBlock_)
		flags |= O_NONBLOCK;

	co_return flags;
}

static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
pollWait(void *object, uint64_t pastSeq, int mask, async::cancellation_token cancellation) {
	(void)mask; // TODO: utilize mask.

	auto self = static_cast<CdcWdmDevice *>(object);

	if(cancellation.is_cancellation_requested())
		std::cout << "\e[33mnetserver: pollWait() cancellation is untested\e[39m" << std::endl;

	assert(pastSeq <= self->nic->currentSeq_);
	while(pastSeq == self->nic->currentSeq_ && !cancellation.is_cancellation_requested())
		co_await self->nic->statusBell_.async_wait(cancellation);

	// For now making this always writable is sufficient.
	int edges = EPOLLOUT | EPOLLWRNORM;
	if(self->nic->inSeq_ > pastSeq)
		edges |= EPOLLIN | EPOLLRDNORM;

	co_return protocols::fs::PollWaitResult(self->nic->currentSeq_, edges);
}

static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
pollStatus(void *object) {
	auto self = static_cast<CdcWdmDevice *>(object);
	int events = EPOLLOUT | EPOLLWRNORM;
	if(!self->nic->queue_.empty())
		events |= EPOLLIN | EPOLLRDNORM;

	co_return protocols::fs::PollStatusResult(self->nic->currentSeq_, events);
}

static async::result<frg::expected<protocols::fs::Error, size_t>>
write(void *object, helix_ng::CredentialsView credentials, const void *buffer, size_t length) {
	(void) credentials;

	auto self = static_cast<CdcWdmDevice *>(object);

	co_await self->nic->writeCommand(arch::dma_buffer_view{nullptr, const_cast<void *>(buffer), length});

	co_return length;
}

static async::result<protocols::fs::ReadResult> read(void *object, helix_ng::CredentialsView credentials,
			void *buffer, size_t length, async::cancellation_token ct) {
	(void) credentials;

	auto self = static_cast<CdcWdmDevice *>(object);

	auto p = co_await self->nic->queue_.async_get(ct);
	if (!p)
		co_return std::unexpected{protocols::fs::Error::interrupted};

	memcpy(buffer, p->view().data(), std::min(length, p->size()));

	co_return p->size();
}

struct CdcWdmDevice::HandleIoctl {
	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, CdcWdmDevice *self) {
		switch(req.command()) {
			case IOCTL_WDM_MAX_COMMAND: {
				managarm::fs::GenericIoctlReply resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_result(0);
				resp.set_size(self->nic->wMaxControlMessage);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				break;
			}
			default: {
				std::cout << std::format("drivers/usb-mbim: unexpected ioctl request 0x{:x}", req.command());
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
				break;
			}
		}

		co_return {};
	}
};

static async::result<void> ioctl(void *object, uint32_t, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) {
	auto self = static_cast<CdcWdmDevice *>(object);

	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest
	>(conversation, std::move(msg), CdcWdmDevice::HandleIoctl{}, self);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

constexpr auto fileOperations = protocols::fs::FileOperations{
	.read = &read,
	.write = &write,
	.ioctl = &ioctl,
	.pollWait = &pollWait,
	.pollStatus = &pollStatus,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

struct HandleDevice {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, smarter::shared_ptr<CdcWdmDevice> cdc_wdm) {
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), std::move(cdc_wdm), &fileOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Invalid serveDevice request!");
		}
		co_return {};
	}
};

async::detached serveDevice(helix::UniqueLane lane, smarter::shared_ptr<CdcWdmDevice> cdc_wdm) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest
		>(lane, HandleDevice{}, cdc_wdm);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "usb_net: dispatch error" << std::endl;
			continue;
		}
	}
}

UsbMbimNic::UsbMbimNic(mbus_ng::EntityId entity, protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface data_intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out,
		size_t config_index)
	: UsbNic{hw_device, mac, ctrl_intf, ctrl_ep, data_intf, in, out}, entity_{entity},
		config_index_{config_index} {
	raw_ip_ = true;
	configureName("wwan");
}

async::result<void> UsbMbimNic::initialize() {
	auto raw_descs = (co_await device_.configurationDescriptor(config_index_)).value();

	for(const auto &[head, body] : protocols::usb::configurationRange(raw_descs)) {
		if(head.descriptorType != protocols::usb::descriptor_type::cs_interface)
			continue;

		auto desc = protocols::usb::extractDescriptor<protocols::usb::CdcDescriptor>(body);
		if(!desc)
			continue;
		switch(desc->subtype) {
			using CdcSubType = protocols::usb::CdcDescriptor::CdcSubType;

			case CdcSubType::Mbim: {
				auto mbim_hdr = protocols::usb::extractDescriptor<protocols::usb::CdcMbim>(body);
				if(mbim_hdr)
					wMaxControlMessage = mbim_hdr->wMaxControlMessage;
				break;
			}
			default: {
				break;
			}
		}
	}

	if(!wMaxControlMessage) {
		std::cout << "usb-mbim: No usable MBIM functional descriptor found" << std::endl;
		co_return;
	}

	auto config_val = (co_await device_.currentConfigurationValue()).value();
	mbus_ng::Properties descriptor{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity_)}},
		{"generic.devtype", mbus_ng::StringItem{"char"}},
		{"generic.devname", mbus_ng::StringItem{"cdc-wdm"}},
		{"usb.interface_drivers", mbus_ng::ArrayItem{{
			mbus_ng::ArrayItem{{
				mbus_ng::StringItem{std::format("{}.{}", config_val, ctrl_intf_.num())},
				mbus_ng::StringItem{"cdc_mbim"},
			}},
			mbus_ng::ArrayItem{{
				mbus_ng::StringItem{std::format("{}.{}", config_val, data_intf_.num())},
				mbus_ng::StringItem{"cdc_mbim"},
			}},
		}}},
	};

	auto wwan_entity = (co_await mbus_ng::Instance::global().createEntity(
		"wwan", descriptor)).unwrap();

	auto cdc_wdm = smarter::make_shared<CdcWdmDevice>(this);

	[] (smarter::shared_ptr<CdcWdmDevice> cdc_wdm, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			serveDevice(std::move(localLane), cdc_wdm);
		}
	}(cdc_wdm, std::move(wwan_entity));

	cdcWdmDev_ = std::move(cdc_wdm);

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

	mbus_ng::Properties usbmiscProperties{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity_)}},
		{"unix.subsystem", mbus_ng::StringItem{"usbmisc"}},
		{"usb.parent-interface", mbus_ng::StringItem{std::format("{}.{}", config_val, ctrl_intf_.num())},}
	};

	auto usbmiscClassEntity = (co_await mbus_ng::Instance::global().createEntity(
		"usbmisc", usbmiscProperties)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));
		}
	}(std::move(usbmiscClassEntity));

	receiveEncapsulated();
	listenForNotifications();
	receiveFrames();

	co_return;
}

async::detached UsbMbimNic::receiveEncapsulated() {
	while(true) {
		co_await response_available_.async_wait();

		arch::dma_object<protocols::usb::SetupPacket> ctrl_msg{&dmaPool_};
		arch::dma_buffer data{&dmaPool_, 0x1000};
		ctrl_msg->type = protocols::usb::setup_type::byClass |
						protocols::usb::setup_type::toHost | protocols::usb::setup_type::targetInterface;
		ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::GET_ENCAPSULATED_RESPONSE);
		ctrl_msg->value = 0;
		ctrl_msg->index = ctrl_intf_.num();
		ctrl_msg->length = data.size();

		auto control = protocols::usb::ControlTransfer{
			protocols::usb::kXferToHost, ctrl_msg, data
		};
		auto res = co_await device_.transfer(control);
		assert(res);

		// TODO: put the correct data size
		queue_.put({std::move(data), res.value()});
		inSeq_ = ++currentSeq_;
		statusBell_.raise();
	}
}

async::detached UsbMbimNic::listenForNotifications() {
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

			case Notification::RESPONSE_AVAILABLE:
				response_available_.raise();
				break;
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

async::detached UsbMbimNic::receiveFrames() {
	// The NTB input size is not negotiated; this holds the NTBs of common devices.
	arch::dma_buffer buffer{&dmaPool_, 16384};

	while(true) {
		auto res = co_await data_in_.transfer(protocols::usb::BulkTransfer{protocols::usb::kXferToHost, buffer});
		assert(res);

		if(res.value() != 0)
			usb_ncm::feedNtb(*this, buffer.subview(0, res.value()), usb_mbim::NCM_NDP16_IPS_SIGNATURE);
	}
}

async::result<void> UsbMbimNic::send(const arch::dma_buffer_view payload) {
	arch::dma_buffer buf{&dmaPool_, sizeof(usb_ncm::NcmTransferHeader) + sizeof(usb_ncm::NcmDatagramPointer) + payload.size()};
	auto ncmHeader = reinterpret_cast<usb_ncm::NcmTransferHeader *>(buf.data());
	ncmHeader->dwSignature = usb_ncm::NCM_NTH16_SIGNATURE;
	ncmHeader->wHeaderLength = sizeof(*ncmHeader);
	ncmHeader->wSequence = seq_++;
	ncmHeader->wBlockLength = uint16_t(buf.size());
	ncmHeader->wNdpIndex = sizeof(*ncmHeader);

	auto ndp = reinterpret_cast<usb_ncm::NcmDatagramPointer *>(buf.subview(ncmHeader->wNdpIndex).data());
	ndp->dwSignature = usb_mbim::NCM_NDP16_IPS_SIGNATURE;
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

async::result<void> UsbMbimNic::writeCommand(const arch::dma_buffer_view request) {
	arch::dma_object<protocols::usb::SetupPacket> ctrl_msg{&dmaPool_};
	ctrl_msg->type = protocols::usb::setup_type::byClass |
					protocols::usb::setup_type::toDevice | protocols::usb::setup_type::targetInterface;
	ctrl_msg->request = uint8_t(nic::usb_net::RequestCode::SEND_ENCAPSULATED_COMMAND);
	ctrl_msg->value = 0;
	ctrl_msg->index = ctrl_intf_.num();
	ctrl_msg->length = request.size();

	auto res = co_await device_.transfer(protocols::usb::ControlTransfer{
		protocols::usb::kXferToDevice, ctrl_msg, request
	});
	assert(res);

	co_return;
}

} // namespace nic::usb_mbim
