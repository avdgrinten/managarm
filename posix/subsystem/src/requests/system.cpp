#include "common.hpp"
#include "../requests.hpp"
#include "../procfs.hpp"
#include "../devserver.hpp"
#include "../device.hpp"
#include "../pts.hpp"
#include "../tmp_fs.hpp"
#include "../cgroupfs.hpp"
#include <unistd.h>
#include <kerncfg.bragi.hpp>
#include <hw.bragi.hpp>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::RebootRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "REBOOT", "command={}", req.cmd());

	if(self->threadGroup()->uid() != 0) {
		co_await sendErrorResponse<managarm::posix::RebootResponse>(conversation, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
		co_return {};
	}

	managarm::hw::RebootRequest hwRequest;
	hwRequest.set_cmd(req.cmd());
	auto pmLane = co_await getPmLane();
	auto [offer, hwSendResp, hwResp] = co_await helix_ng::exchangeMsgs(
		pmLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(hwRequest, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(hwSendResp.error());
	HEL_CHECK(hwResp.error());
	auto hwResult = *bragi::parse_head_only<managarm::hw::SvrResponse>(hwResp);
	hwResp.reset();

	// The pm-interface only responds if it could not power off or restart the system.
	if(hwResult.error() != managarm::hw::Errors::SUCCESS) {
		auto error = managarm::posix::Errors::INTERNAL_ERROR;
		if(hwResult.error() == managarm::hw::Errors::ILLEGAL_ARGUMENTS)
			error = managarm::posix::Errors::ILLEGAL_ARGUMENTS;
		co_await sendErrorResponse<managarm::posix::RebootResponse>(conversation, error);
		co_return {};
	}

	managarm::posix::RebootResponse resp;

	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::MountRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "MOUNT", "fstype={} on={} to={}", req.fs_type(), req.path(), req.target_path());

	auto resolveResult = co_await resolve(self->fsContext()->getRoot(),
			self->fsContext()->getWorkingDirectory(), req.target_path(), self.get());
	if(!resolveResult) {
		co_await sendErrorResponse<managarm::posix::MountResponse>(conversation, resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}
	auto target = resolveResult.value();

	if(req.fs_type() == "procfs" || req.fs_type() == "proc") {
		co_await target.first->mount(target.second, getProcfs());
	}else if(req.fs_type() == "sysfs") {
		co_await target.first->mount(target.second, co_await devserver::getSysfsRoot());
	}else if(req.fs_type() == "devtmpfs") {
		co_await target.first->mount(target.second, getDevtmpfs());
	}else if(req.fs_type() == "tmpfs") {
		auto res = tmp_fs::createRoot(self.get(), req.mount_data());
		if (!res) {
			co_await sendErrorResponse<managarm::posix::MountResponse>(conversation, res.error() | toPosixProtoError);
			co_return {};
		}
		co_await target.first->mount(target.second, *res);
	}else if(req.fs_type() == "devpts") {
		co_await target.first->mount(target.second, pts::getFsRoot());
	}else if(req.fs_type() == "cgroup2") {
		co_await target.first->mount(target.second, getCgroupfs());
	}else{
		if(req.fs_type() != "ext2" && req.fs_type() != "btrfs") {
			std::cout << "posix: Trying to mount unsupported FS of type: " << req.fs_type() << std::endl;
			co_await sendErrorResponse<managarm::posix::MountResponse>(conversation, managarm::posix::Errors::NO_BACKING_DEVICE);
			co_return {};
		}

		auto sourceResult = co_await resolve(self->fsContext()->getRoot(),
				self->fsContext()->getWorkingDirectory(), req.path(), self.get());
		if(!sourceResult) {
			co_await sendErrorResponse<managarm::posix::MountResponse>(conversation, sourceResult.error() | toPosixError | toPosixProtoError);
			co_return {};
		}
		auto source = sourceResult.value();
		assert(source.second);
		assert(source.second->getTarget()->getType() == VfsType::blockDevice);
		auto device = blockRegistry.get(source.second->getTarget()->readDevice());
		auto link = co_await device->mount(req.fs_type());
		if(!link) {
			co_await sendErrorResponse<managarm::posix::MountResponse>(conversation, link.error() | toPosixProtoError);
			co_return {};
		}
		co_await target.first->mount(target.second, std::move(link.value()), source);
	}

	logRequest(logRequests, self, "MOUNT", "succeeded");

	managarm::posix::MountResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SwaponRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	// Swap priorities and discard flags have no effect yet.
	logRequest(logRequests, self, "SWAPON", "path={} flags={:#x}", req.path(), req.flags());

	if(self->threadGroup()->uid() != 0) {
		co_await sendErrorResponse<managarm::posix::SwaponResponse>(conversation, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
		co_return {};
	}

	auto pathResult = co_await resolve(self->fsContext()->getRoot(),
			self->fsContext()->getWorkingDirectory(), req.path(), self.get());
	if(!pathResult) {
		co_await sendErrorResponse<managarm::posix::SwaponResponse>(conversation, pathResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}
	auto node = pathResult.value().second->getTarget();

	// The block device's driver or the file's file system sets up the swap area.
	std::expected<void, Error> result;
	if(node->getType() == VfsType::blockDevice) {
		auto device = blockRegistry.get(node->readDevice());
		result = co_await device->swapon();
	} else if(node->getType() == VfsType::regular) {
		result = co_await node->swapon();
	} else {
		result = std::unexpected{Error::illegalArguments};
	}
	if(!result) {
		co_await sendErrorResponse<managarm::posix::SwaponResponse>(conversation, result.error() | toPosixProtoError);
		co_return {};
	}

	logRequest(logRequests, self, "SWAPON", "succeeded");

	managarm::posix::SwaponResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SysconfRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SYSCONF");

	managarm::posix::SysconfResponse resp;

	// Configured == available == online
	if(req.num() == _SC_NPROCESSORS_CONF || req.num() == _SC_NPROCESSORS_ONLN) {
		managarm::kerncfg::GetNumCpuRequest kerncfgRequest;
		auto [offer, kerncfgSendResp, kerncfgResp] = co_await helix_ng::exchangeMsgs(
		getKerncfgLane(),
		helix_ng::offer(
				helix_ng::sendBragiHeadOnly(kerncfgRequest, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(kerncfgSendResp.error());
		HEL_CHECK(kerncfgResp.error());

		auto kernResp = bragi::parse_head_only<managarm::kerncfg::GetNumCpuResponse>(kerncfgResp);
		kerncfgResp.reset();

		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_value(kernResp->num_cpu());
	} else {
		// Not handled, bubble up EINVAL.
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	}
	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetMemoryInformationRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_MEMORY_INFORMATION");

	managarm::kerncfg::GetMemoryInformationRequest kerncfgRequest;
	auto [offer, kerncfgSendResp, kerncfgResp] = co_await helix_ng::exchangeMsgs(
		getKerncfgLane(),
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(kerncfgRequest, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(kerncfgSendResp.error());
	HEL_CHECK(kerncfgResp.error());

	auto kernResp = bragi::parse_head_only<managarm::kerncfg::GetMemoryInformationResponse>(kerncfgResp);
	kerncfgResp.reset();

	managarm::posix::GetMemoryInformationResponse resp;
	resp.set_total_usable_memory(kernResp->total_usable_memory());
	resp.set_available_memory(kernResp->available_memory());
	resp.set_memory_unit(kernResp->memory_unit());

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
