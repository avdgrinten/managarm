#pragma once

#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <format>
#include <vector>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>
#include <core/clock.hpp>
#include <core/dispatch.hpp>

#include "../debug-options.hpp"
#include "../process.hpp"
#include "../ostrace.hpp"
#include "../clocks.hpp"
#include "../pidfd.hpp"

namespace requests {

inline void logRequest(bool cond, const std::shared_ptr<Process>& self, std::string_view name) {
	if(cond) {
		std::cout << "posix: [" << self->pid() << "] " << name << std::endl;
	}
}

template<class... Args>
inline void logRequest(bool cond, const std::shared_ptr<Process>& self, std::string_view name,
                std::format_string<Args...> fmt, Args&&... args) {
	if(cond) {
		std::cout << "posix: [" << self->pid() << "] " << name << " "
		          << std::format(fmt, std::forward<Args>(args)...) << std::endl;
	}
}

// Checks the errors of sending a reply; a requester that terminated in the meantime is no error.
inline std::expected<void, DispatchError> checkReply(std::initializer_list<HelError> errors) {
	for(auto error : errors) {
		if(error == kHelErrEndOfLane || error == kHelErrLaneShutdown)
			return {};
		if(error != kHelErrNone)
			return std::unexpected{DispatchError::ipcError};
	}
	return {};
}

struct HandleRequest {
	std::uint64_t id = 0;
	timespec requestTimestamp = {};
	protocols::ostrace::Timer timer;

	template <typename T>
	void logBragiRequest(T &req) {
		if(!posix::ostContext.isActive())
			return;

		requestTimestamp = clk::getTimeSinceBoot();
		std::string reqHead;
		std::string reqTail;
		reqHead.resize(req.size_of_head());
		reqTail.resize(req.size_of_tail());
		bragi::limited_writer headWriter{reqHead.data(), reqHead.size()};
		bragi::limited_writer tailWriter{reqTail.data(), reqTail.size()};
		auto headOk = req.encode_head(headWriter);
		auto tailOk = req.encode_tail(tailWriter);
		assert(headOk);
		assert(tailOk);
		posix::ostContext.emitWithTimestamp(
			posix::ostEvtRequest,
			(requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec,
			posix::ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			posix::ostBragi({reinterpret_cast<uint8_t *>(reqHead.data()), reqHead.size()},
			                 {reinterpret_cast<uint8_t *>(reqTail.data()), reqTail.size()})
		);
	}

	void logBragiReply(auto &resp) {
		if(!posix::ostContext.isActive())
			return;

		auto ts = clk::getTimeSinceBoot();
		std::string replyHead;
		std::string replyTail;
		replyHead.resize(resp.size_of_head());
		replyTail.resize(resp.size_of_tail());
		bragi::limited_writer headWriter{replyHead.data(), replyHead.size()};
		bragi::limited_writer tailWriter{replyTail.data(), replyTail.size()};
		auto headOk = resp.encode_head(headWriter);
		auto tailOk = resp.encode_tail(tailWriter);
		assert(headOk);
		assert(tailOk);
		posix::ostContext.emitWithTimestamp(
			posix::ostEvtRequest,
			(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
			posix::ostAttrRequest(id),
			posix::ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			posix::ostBragi({reinterpret_cast<uint8_t *>(replyHead.data()), replyHead.size()},
			                 {reinterpret_cast<uint8_t *>(replyTail.data()), replyTail.size()})
		);
	}

	void logBragiSerializedReply(std::string &ser) {
		if(!posix::ostContext.isActive())
			return;

		auto ts = clk::getTimeSinceBoot();
		posix::ostContext.emitWithTimestamp(
			posix::ostEvtRequest,
			(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
			posix::ostAttrRequest(id),
			posix::ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			posix::ostBragi({reinterpret_cast<uint8_t *>(ser.data()), ser.size()}, {})
		);
	}

	template <typename Response>
	async::result<void> sendErrorResponse(helix::BorrowedDescriptor conversation,
			managarm::posix::Errors err) {
		Response resp;
		resp.set_error(err);

		if (resp.size_of_tail()) {
			auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_tail.error());
		} else {
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
		}
		logBragiReply(resp);
	}

	// From fd.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::Dup2Request &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::DupRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::DupFdQueryRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::IsTtyRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::IoctlFioclexRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::CloseRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::EpollCallRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation, async::cancellation_token cancellation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::EpollCtlRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::EpollWaitRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation, async::cancellation_token cancellation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FdGetFlagsRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FdSetFlagsRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From filesystem.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::ChrootRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::ChdirRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FchdirRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::AccessAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::MkdirAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::MkfifoAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::LinkAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SymlinkAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::ReadlinkAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::RenameAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::UnlinkAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::RmdirRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FstatAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FstatfsRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SynchronizeRequest &&req,
			helix::BorrowedDescriptor conversation, bragi::preamble preamble,
			std::shared_ptr<Process> self, std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FchmodAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::FchownAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::UtimensAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::OpenAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::MknodAtRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::UmaskRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetCwdRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TtyNameRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From special-files.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::InotifyCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::InotifyAddRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::InotifyRmRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::EventfdCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerFdCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerFdSetRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerFdGetRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::PidfdOpenRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::PidfdSendSignalRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::PidfdGetPidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::EpollCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::PipeCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SignalfdCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From memory.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::VmMapRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::VmRemapRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::VmProtectRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::VmUnmapRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::MemFdCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From uid-gid.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetPidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetPpidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetUidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetResuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetResgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetUidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetResuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetResgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetReuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetRegidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetSuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetEuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetEuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetGidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetEgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetGidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetEgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetGroupsRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetGroupsRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetSuidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetSgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From process.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::WaitIdRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::WaitRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation, async::cancellation_token cancellation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetAffinityRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetAffinityRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetPgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetPgidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetSidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetSidRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::ParentDeathSignalRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::ProcessDumpableRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetResourceLimitRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetResourceUsageRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SigactionRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From socket.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::NetserverRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SocketRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SockpairRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::AcceptRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From system.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::RebootRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::MountRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SwaponRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SysconfRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::GetMemoryInformationRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);

	// From timer.cpp
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::SetIntervalTimerRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerSetRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerGetRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::posix::TimerDeleteRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<Process> self,
			std::shared_ptr<Generation> generation);
};

} // namespace requests
