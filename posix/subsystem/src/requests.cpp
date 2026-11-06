#include <type_traits>

#include <async/algorithm.hpp>
#include <async/cancellation.hpp>
#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>

#include <core/dispatch.hpp>

#include "debug-options.hpp"
#include "requests/common.hpp"

namespace {

// Requests that are cancelled when their thread terminates; their handlers tolerate
// failed replies (see requests::checkIpc()).
template<typename Message>
constexpr bool cancelledOnTermination = std::is_same_v<Message, managarm::posix::EpollCallRequest>
		|| std::is_same_v<Message, managarm::posix::EpollWaitRequest>
		|| std::is_same_v<Message, managarm::posix::WaitRequest>;

// Non-const: bragi accessors are not const, a const reference would always yield 0 here.
template<typename Message>
uint64_t cancellationTagOf(Message &req) {
	if constexpr (requires { req.cancellation_id(); })
		return req.cancellation_id();
	return 0;
}

// Registers the cancellation tag of these requests and passes the token to their handlers.
// Their conversations are shut down once the thread terminates, such that replies fail instead
// of waiting for receives (e.g., of EPOLL_CALL's tail) that the thread never posts.
struct HandleThreadRequest {
	template<typename Message>
	auto operator()(Message &&req, helix::BorrowedDescriptor conversation, bragi::preamble preamble,
			std::shared_ptr<Process> self, std::shared_ptr<Generation> generation) {
		if constexpr (cancelledOnTermination<std::remove_cvref_t<Message>>) {
			auto closed = self->cancelEventRegistry().closedToken();
			// The lambdas run when the sender starts; req outlives it, the arguments do not.
			return async::with_cancel_cb(
				async::let(
					[&req, self] {
						return self->cancelEventRegistry().event(self->credentials(),
								cancellationTagOf(req));
					},
					[this, &req, conversation, preamble, self, generation]
							(CancelEventRegistry::CancelEventGuard &cancelEvent) {
						return inner(std::forward<Message>(req), conversation, preamble, self,
								generation, cancelEvent);
					}
				),
				[conversation] {
					HEL_CHECK(helShutdownLane(conversation.getHandle()));
				},
				closed
			);
		} else {
			return inner(std::forward<Message>(req), conversation, preamble, std::move(self),
					std::move(generation));
		}
	}

	requests::HandleRequest inner;
};

} // namespace

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	async::cancellation_token cancellation = generation->cancelServe;

	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(self->posixLane().getHandle()));
	}};

	while(true) {
		auto res = co_await dispatchRequest<
			// From fd.cpp
			managarm::posix::Dup2Request,
			managarm::posix::DupRequest,
			managarm::posix::DupFdQueryRequest,
			managarm::posix::IsTtyRequest,
			managarm::posix::IoctlFioclexRequest,
			managarm::posix::CloseRequest,
			managarm::posix::EpollCallRequest,
			managarm::posix::EpollCtlRequest,
			managarm::posix::EpollWaitRequest,
			managarm::posix::FdGetFlagsRequest,
			managarm::posix::FdSetFlagsRequest,
			// From filesystem.cpp
			managarm::posix::ChrootRequest,
			managarm::posix::ChdirRequest,
			managarm::posix::FchdirRequest,
			managarm::posix::AccessAtRequest,
			managarm::posix::MkdirAtRequest,
			managarm::posix::MkfifoAtRequest,
			managarm::posix::LinkAtRequest,
			managarm::posix::SymlinkAtRequest,
			managarm::posix::ReadlinkAtRequest,
			managarm::posix::RenameAtRequest,
			managarm::posix::UnlinkAtRequest,
			managarm::posix::RmdirRequest,
			managarm::posix::FstatAtRequest,
			managarm::posix::FstatfsRequest,
			managarm::posix::SynchronizeRequest,
			managarm::posix::FchmodAtRequest,
			managarm::posix::FchownAtRequest,
			managarm::posix::UtimensAtRequest,
			managarm::posix::OpenAtRequest,
			managarm::posix::MknodAtRequest,
			managarm::posix::UmaskRequest,
			managarm::posix::GetCwdRequest,
			managarm::posix::TtyNameRequest,
			// From special-files.cpp
			managarm::posix::InotifyCreateRequest,
			managarm::posix::InotifyAddRequest,
			managarm::posix::InotifyRmRequest,
			managarm::posix::EventfdCreateRequest,
			managarm::posix::TimerFdCreateRequest,
			managarm::posix::TimerFdSetRequest,
			managarm::posix::TimerFdGetRequest,
			managarm::posix::PidfdOpenRequest,
			managarm::posix::PidfdSendSignalRequest,
			managarm::posix::PidfdGetPidRequest,
			managarm::posix::EpollCreateRequest,
			managarm::posix::PipeCreateRequest,
			managarm::posix::SignalfdCreateRequest,
			// From memory.cpp
			managarm::posix::VmMapRequest,
			managarm::posix::VmRemapRequest,
			managarm::posix::VmProtectRequest,
			managarm::posix::VmUnmapRequest,
			managarm::posix::MemFdCreateRequest,
			// From uid-gid.cpp
			managarm::posix::GetPidRequest,
			managarm::posix::GetPpidRequest,
			managarm::posix::GetUidRequest,
			managarm::posix::GetResuidRequest,
			managarm::posix::GetResgidRequest,
			managarm::posix::SetUidRequest,
			managarm::posix::SetResuidRequest,
			managarm::posix::SetResgidRequest,
			managarm::posix::SetReuidRequest,
			managarm::posix::SetRegidRequest,
			managarm::posix::SetSuidRequest,
			managarm::posix::GetEuidRequest,
			managarm::posix::SetEuidRequest,
			managarm::posix::GetGidRequest,
			managarm::posix::GetEgidRequest,
			managarm::posix::SetGidRequest,
			managarm::posix::SetEgidRequest,
			managarm::posix::GetGroupsRequest,
			managarm::posix::SetGroupsRequest,
			managarm::posix::GetSuidRequest,
			managarm::posix::GetSgidRequest,
			// From process.cpp
			managarm::posix::WaitIdRequest,
			managarm::posix::WaitRequest,
			managarm::posix::SetAffinityRequest,
			managarm::posix::GetAffinityRequest,
			managarm::posix::GetPgidRequest,
			managarm::posix::SetPgidRequest,
			managarm::posix::GetSidRequest,
			managarm::posix::SetSidRequest,
			managarm::posix::ParentDeathSignalRequest,
			managarm::posix::ProcessDumpableRequest,
			managarm::posix::SetResourceLimitRequest,
			managarm::posix::GetResourceUsageRequest,
			managarm::posix::SigactionRequest,
			// From socket.cpp
			managarm::posix::NetserverRequest,
			managarm::posix::SocketRequest,
			managarm::posix::SockpairRequest,
			managarm::posix::AcceptRequest,
			// From system.cpp
			managarm::posix::RebootRequest,
			managarm::posix::MountRequest,
			managarm::posix::SwaponRequest,
			managarm::posix::SysconfRequest,
			managarm::posix::GetMemoryInformationRequest,
			// From timer.cpp
			managarm::posix::SetIntervalTimerRequest,
			managarm::posix::TimerCreateRequest,
			managarm::posix::TimerSetRequest,
			managarm::posix::TimerGetRequest,
			managarm::posix::TimerDeleteRequest
		>(self->posixLane(), HandleThreadRequest{}, self, generation);

		if(!res) {
			if(res.error() == DispatchError::shutdown)
				break;
			std::cout << "posix: dispatch error" << std::endl;
			continue;
		}
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Exiting serveRequests()\e[39m" << std::endl;
	generation->requestsDone.raise();
}
