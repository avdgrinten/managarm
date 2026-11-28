#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <asm/ioctls.h>
#include <fcntl.h>
#include <termios.h>
#include <iostream>

#include <arch/dma_pool.hpp>
#include <core/cmdline.hpp>
#include <core/kernel-logs.hpp>
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <frg/cmdline.hpp>
#include <frg/string.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <kerncfg.bragi.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>
#include <protocols/svrctl/server.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>
#include <protocols/usb/usb.hpp>

#include "ch341/ch341.hpp"
#include "cp2102/cp2102.hpp"
#include "ft232/ft232.hpp"
#include "posix.bragi.hpp"
#include "controller.hpp"

namespace {

using WriteQueue = frg::intrusive_list<
	WriteRequest,
	frg::locate_member<
		WriteRequest,
		frg::default_list_hook<WriteRequest>,
		&WriteRequest::hook
	>
>;

std::vector<smarter::shared_ptr<Controller>> controllers;

arch::dma_realm dmaRealm;
arch::contiguous_pool pool{&dmaRealm, {.addressBits = 64}};

WriteQueue sendRequests;

helix::UniqueDescriptor kerncfgLane = {};

async::result<void> dumpKernelMessages(smarter::shared_ptr<Controller> c) {
	std::vector<uint8_t> buffer(2048);
	KernelLogs logs{};

	while(true) {
		auto res = co_await logs.getMessage({buffer});

		WriteRequest req{std::span(buffer).subspan(0, res), c.get()};
		sendRequests.push_back(&req);

		c->flushSends();
		co_await req.event.wait();
	}
}

}

async::result<frg::expected<protocols::fs::Error, size_t>>
write(void *object, helix_ng::CredentialsView, const void *buffer, size_t length) {
	auto self = static_cast<Controller *>(object);

	if(!length)
		co_return 0;

	WriteRequest req{std::span(reinterpret_cast<const uint8_t *>(buffer), length), self};
	sendRequests.push_back(&req);

	self->flushSends();
	co_await req.event.wait();

	co_return length;
}

async::result<protocols::fs::SeekResult> seek(void *, int64_t) {
	co_return protocols::fs::Error::seekOnPipe;
}

struct Controller::HandleIoctl {
	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, Controller *self) {
		switch(req.command()) {
			case TCGETS: {
				managarm::fs::GenericIoctlReply resp;
				struct termios attrs;

				// Element-wise copy to avoid information leaks in padding.
				memset(&attrs, 0, sizeof(struct termios));
				attrs.c_iflag = self->activeSettings.c_iflag;
				attrs.c_oflag = self->activeSettings.c_oflag;
				attrs.c_cflag = self->activeSettings.c_cflag;
				attrs.c_lflag = self->activeSettings.c_lflag;
				for(int i = 0; i < NCCS; i++)
					attrs.c_cc[i] = self->activeSettings.c_cc[i];

				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto [send_resp, send_attrs] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::sendBuffer(&attrs, sizeof(struct termios))
				);
				HEL_CHECK(send_resp.error());
				HEL_CHECK(send_attrs.error());
				break;
			}
			case TCSETS: {
				struct termios attrs;
				managarm::fs::GenericIoctlReply resp;

				auto [recv_attrs] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(&attrs, sizeof(struct termios))
				);
				HEL_CHECK(recv_attrs.error());

				co_await self->setConfiguration(attrs);

				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				break;
			}
			default: {
				std::cout << "\e[31m" "usb-serial: Unknown ioctl() 0x"
					<< std::hex << req.command() << std::dec << "\e[39m" << std::endl;

				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}
		}

		co_return {};
	}
};

async::result<void> ioctl(void *object, uint32_t, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) {
	auto self = static_cast<Controller *>(object);

	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest
	>(conversation, std::move(msg), Controller::HandleIoctl{}, self);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

static async::result<void> setFileFlags(void *object, int flags) {
	auto self = static_cast<Controller *>(object);

	if(flags & ~O_NONBLOCK) {
		std::cout << "usb-serial: setFileFlags with unknown flags" << std::endl;
		co_return;
	}

	if(flags & O_NONBLOCK)
		self->nonBlock_ = true;
	else
		self->nonBlock_ = false;
	co_return;
}

static async::result<int> getFileFlags(void *object) {
	auto self = static_cast<Controller *>(object);
	if(self->nonBlock_)
		co_return O_NONBLOCK;
	co_return 0;
}

constexpr auto fileOperations = protocols::fs::FileOperations{
	.seekAbs = &seek,
	.seekRel = &seek,
	.seekEof = &seek,
	.write = &write,
	.ioctl = &ioctl,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

Controller::Controller(protocols::usb::Device hw)
		: hw_{std::move(hw)} {
	activeSettings.c_iflag = 0x0000;
	activeSettings.c_oflag = ONLCR;
	activeSettings.c_cflag = B9600 | CREAD | CS8 | CLOCAL | HUPCL;
	activeSettings.c_lflag = ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL | IEXTEN;
	activeSettings.c_line = 0;
	activeSettings.c_cc[VINTR] = CINTR;
	activeSettings.c_cc[VQUIT] = CQUIT;
	activeSettings.c_cc[VERASE] = CERASE;
	activeSettings.c_cc[VKILL] = CKILL;
	activeSettings.c_cc[VEOF] = CEOF;
	activeSettings.c_cc[VTIME] = CTIME;
	activeSettings.c_cc[VMIN] = CMIN;
	activeSettings.c_cc[VSWTC] = _POSIX_VDISABLE;
	activeSettings.c_cc[VSTART] = CSTART;
	activeSettings.c_cc[VSTOP] = CSTOP;
	activeSettings.c_cc[VSUSP] = CSUSP;
	activeSettings.c_cc[VEOL] = CEOL;
	activeSettings.c_cc[VREPRINT] = CREPRINT;
	activeSettings.c_cc[VDISCARD] = CDISCARD;
	activeSettings.c_cc[VWERASE] = CWERASE;
	activeSettings.c_cc[VLNEXT] = CLNEXT;
}

async::detached Controller::flushSends() {
	assert(!sendRequests.empty());

	WriteQueue pending;

	while(!sendRequests.empty()) {
		auto req = sendRequests.front();
		assert(req->progress < req->buffer.size_bytes());
		size_t fifoAvailable = req->controller->sendFifoSize();

		size_t chunk = std::min(req->buffer.size_bytes() - req->progress, fifoAvailable);
		assert(chunk);
		arch::dma_buffer buf{&pool, chunk};
		memcpy(buf.data(), req->buffer.subspan(req->progress).data(), chunk);

		auto err = co_await req->controller->send(protocols::usb::BulkTransfer{protocols::usb::kXferToDevice, buf});

		if(err == protocols::usb::UsbError::none)
			req->progress += chunk;

		// We only complete writes once we have written all bytes;
		// this avoids unnecessary round trips between the UART driver and the application.
		if(req->progress == req->buffer.size_bytes()) {
			sendRequests.pop_front();
			pending.push_back(req);
		}
	}

	while(!pending.empty()) {
		auto req = pending.front();
		pending.pop_front();
		req->event.raise();
	}
}

struct HandleTerminal {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, smarter::shared_ptr<Controller> controller) {
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), std::move(controller), &fileOperations));

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
			throw std::runtime_error("Invalid serveTerminal request!");
		}
		co_return {};
	}
};

async::detached serveTerminal(helix::UniqueLane lane, smarter::shared_ptr<Controller> controller) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest
		>(lane, HandleTerminal{}, controller);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "usb-serial: dispatch error" << std::endl;
			continue;
		}
	}
}

enum class ControllerType {
	None,
	Cp2102,
	Ft232,
	Ch341,
};

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	auto baseEntity = co_await mbus_ng::Instance::global().getEntity(base_id);

	auto properties = (co_await baseEntity.getProperties()).unwrap();
	if(auto subsystem = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);
			!subsystem || subsystem->value != "usb")
		co_return protocols::svrctl::Error::deviceNotSupported;
	if(auto type = std::get_if<mbus_ng::StringItem>(&properties["usb.type"]);
			!type || type->value != "device")
		co_return protocols::svrctl::Error::deviceNotSupported;

	auto vendor = std::get_if<mbus_ng::StringItem>(&properties["usb.vendor"]);
	auto product = std::get_if<mbus_ng::StringItem>(&properties["usb.product"]);
	if(!vendor || !product)
		co_return protocols::svrctl::Error::deviceNotSupported;

	ControllerType type = ControllerType::None;

	if(Cp2102::valid(vendor->value, product->value)) {
		type = ControllerType::Cp2102;
	} else if(Ft232::valid(vendor->value, product->value)) {
		type = ControllerType::Ft232;
	} else if(Ch341::valid(vendor->value, product->value)) {
		type = ControllerType::Ch341;
	} else {
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	auto device = protocols::usb::connect((co_await baseEntity.getRemoteLane()).unwrap());

	std::optional<smarter::shared_ptr<Controller>> controller;

	switch(type) {
		case ControllerType::Cp2102: {
			auto cp2102 = smarter::make_shared<Cp2102>(std::move(device));
			co_await cp2102->initialize();

			controller = std::move(cp2102);
			break;
		}
		case ControllerType::Ft232: {
			auto ft232 = smarter::make_shared<Ft232>(std::move(device));
			co_await ft232->initialize();

			controller = std::move(ft232);
			break;
		}
		case ControllerType::Ch341: {
			auto ch341 = smarter::make_shared<Ch341>(std::move(device));
			co_await ch341->initialize();

			controller = std::move(ch341);
			break;
		}
		default:
			co_return protocols::svrctl::Error::deviceNotSupported;
	}

	Cmdline cmdlineHelper{};

	if(co_await cmdlineHelper.dumpKernelLogs("usb-serial")) {
		// we're fine with using raw mode, but 9600 baud is a bit slow
		struct termios t{};
		memcpy(&t, &controller->get()->activeSettings, sizeof(t));
		// 115200 baud should be universally supported
		cfsetospeed(&t, B115200);
		co_await controller->get()->setConfiguration(t);

		async::detach(dumpKernelMessages(*controller));
	} else {
		mbus_ng::Properties descriptor{
			{"generic.devtype", mbus_ng::StringItem{"block"}},
			{"generic.devname", mbus_ng::StringItem{"ttyUSB"}}
		};

		auto serialEntity = (co_await mbus_ng::Instance::global().createEntity(
			"usb-serial", descriptor)).unwrap();

		[] (auto controller, mbus_ng::EntityManager entity) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				serveTerminal(std::move(localLane), *controller);
			}
		}(controller, std::move(serialEntity));
	}

	controllers.push_back(*controller);

	co_return protocols::svrctl::Error::success;
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

int main() {
	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);

	return 0;
}
