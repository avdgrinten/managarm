#include <atomic>
#include <print>
#include <sys/socket.h>

#include <async/result.hpp>
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <core/virtio/core.hpp>
#include <frg/std_compat.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

#include "device.hpp"
#include "fs.bragi.hpp"
#include "stack.hpp"

namespace {

// A guest has at most one vsock device.
std::atomic<bool> deviceBound{false};

struct HandleRequest {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble, vsock::VsockStack *stack) {
		if(req.req_type() != managarm::fs::CntReqType::CREATE_SOCKET) {
			std::println("vsock: Unexpected request type {}", static_cast<int>(req.req_type()));
			auto [dismiss] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
			co_return {};
		}

		auto sendError = [&] (managarm::fs::Errors error) -> async::result<void> {
			managarm::fs::SvrResponse resp;
			resp.set_error(error);
			auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
		};

		if(req.domain() != AF_VSOCK) {
			co_await sendError(managarm::fs::Errors::AF_NOT_SUPPORTED);
			co_return {};
		}

		auto [localCtrl, remoteCtrl] = helix::createStream();
		auto [localPt, remotePt] = helix::createStream();

		auto error = stack->serveSocket(std::move(localCtrl), std::move(localPt),
				req.type(), req.flags());
		if(error != managarm::fs::Errors::SUCCESS) {
			co_await sendError(error);
			co_return {};
		}

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		auto [sendResp, pushCtrl, pushPt] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(remoteCtrl, kHelRightInvoke | kHelRightManage),
			helix_ng::pushDescriptor(remotePt, kHelRightInvoke | kHelRightManage)
		);
		HEL_CHECK(sendResp.error());
		HEL_CHECK(pushCtrl.error());
		HEL_CHECK(pushPt.error());
		co_return {};
	}
};

async::result<void> serve(helix::UniqueLane lane, vsock::VsockStack *stack) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest
		>(lane, HandleRequest{}, stack);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::println("vsock: Dispatch error");
		}
	}
}

async::result<void> advertise(vsock::VsockStack *stack) {
	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"vsock"}}
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
		"vsock", descriptor)).unwrap();

	[] (mbus_ng::EntityManager entity, vsock::VsockStack *stack) -> async::detached {
		while(true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			helix::DispatcherPool::global().detach(serve(std::move(localLane), stack));
		}
	}(std::move(entity), stack);
}

async::result<protocols::svrctl::Error> bindDevice(int64_t baseId) {
	auto baseEntity = co_await mbus_ng::Instance::global().getEntity(baseId);

	auto properties = (co_await baseEntity.getProperties()).unwrap();
	auto vendor = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
	auto device = std::get_if<mbus_ng::StringItem>(&properties["pci-device"]);
	if(!vendor || vendor->value != "1af4" || !device || device->value != "1053")
		co_return protocols::svrctl::Error::deviceNotSupported;

	if(deviceBound.exchange(true)) {
		std::println("vsock: Ignoring additional device {}", baseId);
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	protocols::hw::Device hwDevice((co_await baseEntity.getRemoteLane()).unwrap());
	co_await hwDevice.enableBusmaster();
	auto transport = co_await virtio_core::discover(std::move(hwDevice),
			virtio_core::DiscoverMode::modernOnly);
	if(!transport) {
		deviceBound.store(false);
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	auto vsockDevice = new vsock::Device{std::move(transport)};
	auto stack = new vsock::VsockStack{vsockDevice};
	co_await vsockDevice->initialize(stack);
	std::println("vsock: Guest CID is {}", vsockDevice->guestCid());

	co_await advertise(stack);
	co_return protocols::svrctl::Error::success;
}

constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice,
};

} // anonymous namespace

int main() {
	std::println("vsock: Starting driver");

	async::detach(protocols::svrctl::serveControl(&controlOps));
	// The server is thread-safe, but a single thread suffices for now.
	helix::DispatcherPool::global().setThreadCount(1);
	helix::DispatcherPool::global().blockOn(
			async::suspend_indefinitely(async::cancellation_token{}));
}
