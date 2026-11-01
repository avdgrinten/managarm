#include "devserver.hpp"

#include <iostream>
#include <linux/netlink.h>

#include <async/oneshot-event.hpp>
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <frg/std_compat.hpp>
#include <protocols/fs/client.hpp>
#include <protocols/mbus/client.hpp>

#include "device.hpp"
#include "extern_fs.hpp"
#include "netlink/nl-socket.hpp"

#include "devserver.bragi.hpp"
#include "fs.bragi.hpp"

namespace devserver {
namespace {

smarter::shared_ptr<FsLink, LinkRc> sysfsRoot;
helix::UniqueLane eventsLane;

// A UnixDevice that is provided by an external server.
struct ExternDevice final : UnixDevice, std::enable_shared_from_this<ExternDevice> {
	ExternDevice(VfsType type, std::string nodePath, helix::UniqueLane lane)
	: UnixDevice{type}, nodePath_{std::move(nodePath)}, lane_{std::move(lane)} { }

	std::string nodePath() override {
		return nodePath_;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(lane_, std::move(mount), std::move(link), semantic_flags);
	}

	async::result<std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error>> mount(std::string fs_type) override {
		return mountExternalDevice(lane_, shared_from_this(), fs_type);
	}

	async::result<std::expected<void, Error>> swapon() override {
		return swaponExternal(lane_);
	}

	async::result<Error> synchronize(protocols::fs::SynchronizeFlags flags) override {
		auto e = co_await protocols::fs::synchronize(lane_, flags);
		// Like UnixDevice::synchronize(), devices without a sync operation have nothing to flush.
		if(e == protocols::fs::Error::notSupported)
			co_return Error::success;
		co_return e | toPosixError;
	}

private:
	// Path of the device in devtmpfs.
	std::string nodePath_;
	helix::UniqueLane lane_;
};

void handleInstallNode(managarm::devserver::InstallNodeRequest &req, helix::UniqueLane lane) {
	auto type = req.node_type() == managarm::devserver::NodeType::BLOCK_DEVICE
			? VfsType::blockDevice : VfsType::charDevice;
	auto device = std::make_shared<ExternDevice>(type, req.path(), std::move(lane));
	device->assignId({static_cast<int>(req.major()), static_cast<int>(req.minor())});
	if(type == VfsType::blockDevice) {
		blockRegistry.install(device);
	} else {
		assert(type == VfsType::charDevice);
		charRegistry.install(device);
	}
}

async::result<void> sendResponse(helix::BorrowedDescriptor conversation, managarm::devserver::Errors error) {
	managarm::devserver::GenericResponse resp;
	resp.set_error(error);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
}

struct HandleDeviceEvent {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::devserver::EmitUeventRequest &&req,
			helix::BorrowedDescriptor conversation, bragi::preamble preamble) {
		auto tailResult = co_await dispatchTail(req, conversation, preamble);
		if(!tailResult)
			co_return std::unexpected(tailResult.error());

		netlink::nl_socket::broadcast(NETLINK_KOBJECT_UEVENT, 1, req.buffer());
		co_await sendResponse(conversation, managarm::devserver::Errors::SUCCESS);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::devserver::InstallNodeRequest &&req,
			helix::BorrowedDescriptor conversation, bragi::preamble preamble) {
		auto tailResult = co_await dispatchTail(req, conversation, preamble,
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage));
		if(!tailResult)
			co_return std::unexpected(tailResult.error());
		auto [pullLane] = std::move(*tailResult);

		handleInstallNode(req, pullLane.descriptor());
		co_await sendResponse(conversation, managarm::devserver::Errors::SUCCESS);
		co_return {};
	}
};

async::result<void> serveDeviceEvents() {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::devserver::EmitUeventRequest,
			managarm::devserver::InstallNodeRequest
		>(eventsLane, HandleDeviceEvent{});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "posix: dispatch error on the devserver events lane" << std::endl;
		}
	}
}

} // anonymous namespace

async::result<void> enumerate() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "devserver"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	std::cout << "POSIX: found devserver" << std::endl;
	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	auto lane = (co_await entity.getRemoteLane()).unwrap();

	managarm::devserver::LaunchRequest req;

	auto [offer, sendReq, recvResp, pullSysfs, pullEvents] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullSysfs.error());
	HEL_CHECK(pullEvents.error());

	managarm::devserver::GenericResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());
	recvResp.reset();
	assert(resp.error() == managarm::devserver::Errors::SUCCESS);

	helix::UniqueLane sysfsLane = pullSysfs.descriptor();
	eventsLane = pullEvents.descriptor();

	// Mount sysfs as extern_fs.
	managarm::fs::MountRequest mountReq;
	mountReq.set_fs_type("sysfs");

	auto [mountOffer, sendHead, sendTail, recvMountResp, pullNode] = co_await helix_ng::exchangeMsgs(
		sysfsLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(mountReq, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
		)
	);
	HEL_CHECK(mountOffer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(recvMountResp.error());
	HEL_CHECK(pullNode.error());

	managarm::fs::MountResponse mountResp;
	mountResp.ParseFromArray(recvMountResp.data(), recvMountResp.length());
	recvMountResp.reset();
	assert(mountResp.error() == managarm::fs::Errors::SUCCESS);

	sysfsRoot = extern_fs::createRoot(std::move(sysfsLane), pullNode.descriptor(),
			nullptr, mountResp.caps(), "sysfs", mountResp.root_inode());

	async::detach(serveDeviceEvents());
}

async::result<smarter::shared_ptr<FsLink, LinkRc>> getSysfsRoot() {
	assert(sysfsRoot);
	co_return sysfsRoot;
}

} // namespace devserver
