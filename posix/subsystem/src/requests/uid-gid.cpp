#include "common.hpp"
#include <iostream>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetPidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_PID", "pid={}", self->pid());

	managarm::posix::GetPidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(self->pid());

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetPpidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_PPID", "ppid={}", self->getParent()->pid());

	managarm::posix::GetPpidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(self->getParent()->pid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetUidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_UID", "uid={}", self->threadGroup()->uid());

	managarm::posix::GetUidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->uid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetResuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	const auto uid = self->threadGroup()->uid();
	const auto euid = self->threadGroup()->euid();
	const auto suid = self->threadGroup()->suid();
	logRequest(logRequests, self, "GET_RESUID", "ruid={} euid={} suid={}", uid, euid, suid);

	managarm::posix::GetResuidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_ruid(uid);
	resp.set_euid(euid);
	resp.set_suid(suid);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetResgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	const auto gid = self->threadGroup()->gid();
	const auto egid = self->threadGroup()->egid();
	const auto sgid = self->threadGroup()->sgid();
	logRequest(logRequests, self, "GET_RESGID", "rgid={} egid={} sgid={}", gid, egid, sgid);

	managarm::posix::GetResgidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_rgid(gid);
	resp.set_egid(egid);
	resp.set_sgid(sgid);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetUidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_UID", "uid={}", req.uid());

	Error err = self->threadGroup()->setUid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse<managarm::posix::SetUidResponse>(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse<managarm::posix::SetUidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse<managarm::posix::SetUidResponse>(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetResuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_RESUID", "ruid={} euid={} suid={}",
			req.ruid(), req.euid(), req.suid());

	Error err = self->threadGroup()->setResuid(req.ruid(), req.euid(), req.suid());
	co_await sendErrorResponse<managarm::posix::SetResuidResponse>(conversation, err | toPosixProtoError);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetResgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_RESGID", "rgid={} egid={} sgid={}",
			req.rgid(), req.egid(), req.sgid());

	Error err = self->threadGroup()->setResgid(req.rgid(), req.egid(), req.sgid());
	co_await sendErrorResponse<managarm::posix::SetResgidResponse>(conversation, err | toPosixProtoError);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetReuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_REUID", "ruid={} euid={}", req.ruid(), req.euid());

	Error err = self->threadGroup()->setReuid(req.ruid(), req.euid());
	co_await sendErrorResponse<managarm::posix::SetReuidResponse>(conversation, err | toPosixProtoError);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetRegidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_REGID", "rgid={} egid={}", req.rgid(), req.egid());

	Error err = self->threadGroup()->setRegid(req.rgid(), req.egid());
	co_await sendErrorResponse<managarm::posix::SetRegidResponse>(conversation, err | toPosixProtoError);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetSuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_SUID", "suid={}", req.suid());

	Error err = self->threadGroup()->setSuid(req.suid());
	co_await sendErrorResponse<managarm::posix::SetSuidResponse>(conversation, err | toPosixProtoError);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetEuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_EUID", "euid={}", self->threadGroup()->euid());

	managarm::posix::GetEuidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->euid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetEuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_EUID", "euid={}", req.uid());

	Error err = self->threadGroup()->setEuid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse<managarm::posix::SetEuidResponse>(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse<managarm::posix::SetEuidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse<managarm::posix::SetEuidResponse>(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetGidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_GID", "gid={}", self->threadGroup()->gid());

	managarm::posix::GetGidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_gid(self->threadGroup()->gid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetEgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_EGID", "egid={}", self->threadGroup()->egid());

	managarm::posix::GetEgidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_gid(self->threadGroup()->egid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetGidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_GID");

	Error err = self->threadGroup()->setGid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse<managarm::posix::SetGidResponse>(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse<managarm::posix::SetGidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse<managarm::posix::SetGidResponse>(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetEgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_EGID");

	Error err = self->threadGroup()->setEgid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse<managarm::posix::SetEgidResponse>(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse<managarm::posix::SetEgidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse<managarm::posix::SetEgidResponse>(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetGroupsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_GROUPS");

	auto groups = self->threadGroup()->supplementaryGroups();
	size_t sendEntries = groups.size();

	managarm::posix::GetGroupsResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if (req.size()) {
		if (groups.size() > req.size()) {
			co_await sendErrorResponse<managarm::posix::GetGroupsResponse>(conversation,
					managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		resp.set_entries(groups.size());
	} else {
		resp.set_entries(groups.size());
		sendEntries = 0;
	}

	auto [send_resp, send_list] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
		helix_ng::sendBuffer(groups.data(), sizeof(gid_t) * sendEntries)
	);
	HEL_CHECK(send_resp.error());
	HEL_CHECK(send_list.error());

	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetGroupsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_GROUPS");

	std::vector<gid_t> list;
	list.resize(req.entries());

	auto [recv_list] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(list.data(), req.entries() * sizeof(gid_t))
		);
	HEL_CHECK(recv_list.error());

	auto err = self->threadGroup()->setSupplementaryGroups(std::move(list));

	managarm::posix::SetGroupsResponse resp;
	resp.set_error(err | toPosixProtoError);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetSuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_SUID", "suid={}", self->threadGroup()->suid());

	managarm::posix::GetSuidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->suid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetSgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_SGID", "sgid={}", self->threadGroup()->sgid());

	managarm::posix::GetSgidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_gid(self->threadGroup()->sgid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
