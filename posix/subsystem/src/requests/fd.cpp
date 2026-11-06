#include <fcntl.h>
#include <sys/poll.h>

#include "common.hpp"
#include "../epoll.hpp"

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::Dup2Request &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "DUP2", "fd={}", req.fd());

	auto file = self->fileContext()->getFile(req.fd());

	managarm::posix::Dup2Response resp;

	if (!file || req.newfd() < 0) {
		resp.set_error(managarm::posix::Errors::NO_SUCH_FD);
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
		co_return {};
	}

	if(req.flags()) {
		if(!(req.flags() & O_CLOEXEC)) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

				HEL_CHECK(send_resp.error());
				co_return {};
		}
	}
	bool closeOnExec = (req.flags() & O_CLOEXEC);

	std::expected<int, Error> result = req.newfd();
	if(req.fcntl_mode())
		result = self->fileContext()->attachFile(file, closeOnExec, req.newfd());
	else
		result = self->fileContext()->attachFile(req.newfd(), file, closeOnExec)
			.transform([&]() {
				return req.newfd();
			});

	if (result) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(result.value());
	} else {
		resp.set_error(result.error() | toPosixProtoError);
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
HandleRequest::operator()(managarm::posix::DupRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "DUP", "fd={}", req.fd());

	auto file = self->fileContext()->getFile(req.fd());

	if (!file) {
		co_await sendErrorResponse<managarm::posix::DupResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)) {
		co_await sendErrorResponse<managarm::posix::DupResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto newfd = self->fileContext()->attachFile(file,
			req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

	managarm::posix::DupResponse resp;
	if (newfd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(newfd.value());
	} else {
		resp.set_error(newfd.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::DupFdQueryRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "DUPFD_QUERY", "fd={}, query_fd={}", req.fd(), req.query_fd());

	auto file = self->fileContext()->getFile(req.fd());
	if (!file) {
		co_await sendErrorResponse<managarm::posix::DupFdQueryResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	auto query_file = self->fileContext()->getFile(req.query_fd());
	if (!query_file) {
		co_await sendErrorResponse<managarm::posix::DupFdQueryResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	managarm::posix::DupFdQueryResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_result(file.get() == query_file.get() ? 1 : 0);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::IsTtyRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "IS_TTY", "fd={}", req.fd());

	auto file = self->fileContext()->getFile(req.fd());
	if(!file) {
		co_await sendErrorResponse<managarm::posix::IsTtyResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	managarm::posix::IsTtyResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_mode(file->isTerminal());

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::IoctlFioclexRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "FIOCLEX");

	if(self->fileContext()->setDescriptor(req.fd(), true) != Error::success) {
		co_await sendErrorResponse<managarm::posix::IoctlFioclexResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	managarm::posix::IoctlFioclexResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto ser = resp.SerializeAsString();
	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()));
	HEL_CHECK(send_resp.error());
	logBragiSerializedReply(ser);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::CloseRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "CLOSE", "fd={}", req.fd());

	auto closeErr = self->fileContext()->closeFile(req.fd());

	if(closeErr != Error::success) {
		if(closeErr == Error::noSuchFile) {
			co_await sendErrorResponse<managarm::posix::CloseResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		} else {
			std::cout << "posix: Unhandled error returned from closeFile" << std::endl;
			co_return {};
		}
	}

	managarm::posix::CloseResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::EpollCallRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>,
		async::cancellation_token cancellation) {
	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return std::unexpected(tailRes.error());
	}

	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "EPOLL_CALL");

	// Since file descriptors may appear multiple times in a poll() call,
	// we need to de-duplicate them here.
	struct PollEvents {
		unsigned int events;
		unsigned int revents;
	};
	std::unordered_map<int, PollEvents> fdsToEvents;

	auto epfile = epoll::createFile();
	assert(req.fds_size() == req.events_size());

	auto timeout = req.timeout();

	// First pass: build the deduplication map, merging events for duplicate FDs.
	for(size_t i = 0; i < req.fds_size(); i++) {
		// if fd is < 0, `events` shall be ignored and revents set to 0
		if (req.fds(i) < 0) {
			fdsToEvents.insert({req.fds(i), {0, 0}});
			continue;
		}

		// Translate POLL events to EPOLL events.
		if(req.events(i) & ~(POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP
				| POLLNVAL | POLLWRNORM | POLLRDNORM)) {
			std::println(
			    "\e[31mposix: Unexpected events {:#x} for poll()\e[39m",
			    req.events(i)
			        & ~(POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP | POLLNVAL
			            | POLLWRNORM | POLLRDNORM)
			);
			co_await sendErrorResponse<managarm::posix::EpollCallResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		unsigned int mask = 0;
		if(req.events(i) & POLLIN) mask |= EPOLLIN;
		if(req.events(i) & POLLRDNORM) mask |= EPOLLIN;
		if(req.events(i) & POLLOUT) mask |= EPOLLOUT;
		if(req.events(i) & POLLWRNORM) mask |= EPOLLOUT;
		if(req.events(i) & POLLPRI) mask |= EPOLLPRI;
		if(req.events(i) & POLLRDHUP) mask |= EPOLLRDHUP;
		if(req.events(i) & POLLERR) mask |= EPOLLERR;
		if(req.events(i) & POLLHUP) mask |= EPOLLHUP;

		auto [mapIt, inserted] = fdsToEvents.insert({req.fds(i), {mask, 0}});
		if(!inserted)
			mapIt->second.events |= mask; // Merge events for duplicate FDs.
	}

	// Second pass: add items to epoll and handle invalid FDs.
	for(auto &[fd, pollEv] : fdsToEvents) {
		if(fd < 0)
			continue;

		auto file = self->fileContext()->getFile(fd);
		if(!file) {
			// poll() is supposed to fail on a per-FD basis.
			pollEv.revents = POLLNVAL;
			timeout = 0;
			continue;
		}
		auto locked = file->weakFile().lock();
		assert(locked);

		Error ret = epoll::addItem(epfile.get(), self.get(),
			std::move(locked), fd, pollEv.events, fd);
		assert(ret == Error::success);
	}

	struct epoll_event events[16] = {};
	size_t k = 0;
	bool interrupted = false;

	if(timeout < 0) {
		co_await async::race_and_cancel(
			async::lambda([&](auto c) -> async::result<void> {
				co_await async::suspend_indefinitely(c, cancellation);
				// if the cancellation was requested, we consider this wait to have been
				// interrupted.
				if (cancellation.is_cancellation_requested())
					interrupted = true;
			}),
			async::lambda([&](auto c) -> async::result<void> {
				if (req.has_signal_seq() && self->enteredSignalSeq() != req.signal_seq()) {
					// a signal was already raised since the request's
					// signal seqnum
					interrupted = true;
					co_return;
				}
				co_await async::suspend_indefinitely(c);
			}),
			async::lambda([&](auto c) -> async::result<void> {
				k = co_await epoll::wait(epfile.get(), events, 16, c);
			})
		);
	}else if(!timeout) {
		// Do not bother to set up a timer for zero timeouts.
		async::cancellation_event cancel_wait;
		cancel_wait.cancel();
		k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
	}else{
		assert(timeout > 0);
		co_await async::race_and_cancel(
			async::lambda([&](auto c) -> async::result<void> {
				// if the timeout runs to completion, i.e. the sleep does not return
				// false to signal cancellation, we DO NOT consider the call to have
				// been interrupted.
				co_await helix::sleepFor(static_cast<uint64_t>(timeout), c);
			}),
			async::lambda([&](auto c) -> async::result<void> {
				co_await async::suspend_indefinitely(c, cancellation);
				// if the cancellation was requested, we consider this wait to have been
				// interrupted.
				if (cancellation.is_cancellation_requested())
					interrupted = true;
			}),
			async::lambda([&](auto c) -> async::result<void> {
				if (req.has_signal_seq() && self->enteredSignalSeq() != req.signal_seq()) {
					// a signal was already raised since the request's
					// signal seqnum
					interrupted = true;
					co_return;
				}
				co_await async::suspend_indefinitely(c);
			}),
			async::lambda([&](auto c) -> async::result<void> {
				k = co_await epoll::wait(epfile.get(), events, 16, c);
			})
		);
	}

	// Assigned the returned events to each FD.
	for(size_t j = 0; j < k; ++j) {
		auto it = fdsToEvents.find(events[j].data.fd);
		assert(it != fdsToEvents.end());

		// Translate EPOLL events back to POLL events.
		assert(!it->second.revents);
		if(events[j].events & EPOLLIN) it->second.revents |= POLLIN;
		if(events[j].events & EPOLLOUT) it->second.revents |= POLLOUT;
		if(events[j].events & EPOLLPRI) it->second.revents |= POLLPRI;
		if(events[j].events & EPOLLRDHUP) it->second.revents |= POLLRDHUP;
		if(events[j].events & EPOLLERR) it->second.revents |= POLLERR;
		if(events[j].events & EPOLLHUP) it->second.revents |= POLLHUP;
	}

	managarm::posix::EpollCallResponse resp;
	bool hasEvents = false;

	for(size_t i = 0; i < req.fds_size(); ++i) {
		auto it = fdsToEvents.find(req.fds(i));
		assert(it != fdsToEvents.end());
		auto maskedEvents = it->second.revents & (req.events(i) | POLLHUP | POLLERR | POLLNVAL);
		resp.add_events(maskedEvents);
		if (!hasEvents && maskedEvents)
			hasEvents = true;
	}

	if (!hasEvents && interrupted) {
		resp.set_error(managarm::posix::Errors::INTERRUPTED);
	} else {
		resp.set_error(managarm::posix::Errors::SUCCESS);
	}

	auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
	);
	if(auto replied = checkReply({send_resp.error(), send_tail.error()}); !replied)
		co_return replied;
	logBragiReply(resp);

	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::EpollCtlRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "EPOLL_CTL");

	auto epfile = self->fileContext()->getFile(req.fd());
	auto file = self->fileContext()->getFile(req.newfd());
	if(!file || !epfile) {
		co_await sendErrorResponse<managarm::posix::EpollCtlResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	managarm::posix::EpollCtlResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if (req.op() == managarm::posix::EpollCtlOp::EPOLL_ADD) {
		auto locked = file->weakFile().lock();
		assert(locked);
		resp.set_error(
		    epoll::addItem(
		        epfile.get(), self.get(), std::move(locked), req.newfd(), req.flags(), req.cookie()
		    )
		    | toPosixProtoError
		);
	} else if (req.op() == managarm::posix::EpollCtlOp::EPOLL_MODIFY) {
		resp.set_error(
		    epoll::modifyItem(epfile.get(), file.get(), req.newfd(), req.flags(), req.cookie())
		    | toPosixProtoError
		);
	} else if (req.op() == managarm::posix::EpollCtlOp::EPOLL_DELETE) {
		resp.set_error(
		    epoll::deleteItem(epfile.get(), file.get(), req.newfd(), req.flags())
		    | toPosixProtoError
		);
	} else {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);

	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::EpollWaitRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>,
		async::cancellation_token cancellation) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "EPOLL_WAIT", "epollfd={}", req.fd());

	uint64_t former = self->signalMask();

	auto epfile = self->fileContext()->getFile(req.fd());
	if(!epfile) {
		co_await sendErrorResponse<managarm::posix::EpollWaitResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	if(req.sigmask_needed()) {
		self->setSignalMask(req.sigmask());
	}

	struct epoll_event events[16];
	size_t k = 0;
	if(req.timeout() < 0) {
		k = co_await epoll::wait(epfile.get(), events,
				std::min(req.size(), uint32_t(16)), cancellation);
	}else if(!req.timeout()) {
		// Do not bother to set up a timer for zero timeouts.
		async::cancellation_event cancel_wait;
		cancel_wait.cancel();
		k = co_await epoll::wait(epfile.get(), events,
				std::min(req.size(), uint32_t(16)), cancel_wait);
	}else{
		assert(req.timeout() > 0);
		co_await async::race_and_cancel(
			async::lambda([&](auto c) -> async::result<void> {
				co_await helix::sleepFor(static_cast<uint64_t>(req.timeout()), c);
			}),
			async::lambda([&](auto c) -> async::result<void> {
				co_await async::suspend_indefinitely(c, cancellation);
			}),
			async::lambda([&](auto c) -> async::result<void> {
				k = co_await epoll::wait(epfile.get(), events, 16, c);
			})
		);
	}
	if(req.sigmask_needed()) {
		self->setSignalMask(former);
	}

	managarm::posix::EpollWaitResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
		helix_ng::sendBuffer(events, k * sizeof(struct epoll_event))
	);
	if(auto replied = checkReply({send_resp.error(), send_data.error()}); !replied)
		co_return replied;
	logBragiReply(resp);

	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FdGetFlagsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "FD_GET_FLAGS");

	auto descriptor = self->fileContext()->getDescriptor(req.fd());
	if(!descriptor) {
		co_await sendErrorResponse<managarm::posix::FdGetFlagsResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	int flags = 0;
	if(descriptor->closeOnExec)
		flags |= FD_CLOEXEC;

	managarm::posix::FdGetFlagsResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_flags(flags);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);

	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FdSetFlagsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "FD_SET_FLAGS");

	if(req.flags() & ~FD_CLOEXEC) {
		std::cout << "posix: FD_SET_FLAGS unknown flags: " << req.flags() << std::endl;
		co_await sendErrorResponse<managarm::posix::FdSetFlagsResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	int closeOnExec = req.flags() & FD_CLOEXEC;
	if(self->fileContext()->setDescriptor(req.fd(), closeOnExec) != Error::success) {
		co_await sendErrorResponse<managarm::posix::FdSetFlagsResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	managarm::posix::FdSetFlagsResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto ser = resp.SerializeAsString();
	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()));
	HEL_CHECK(send_resp.error());
	logBragiSerializedReply(ser);

	co_return {};
}

} // namespace requests
