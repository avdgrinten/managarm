#include "common.hpp"
#include "../net.hpp"
#include "../extern_socket.hpp"
#include "../un-socket.hpp"
#include "../netlink/nl-socket.hpp"
#include "../vsock.hpp"
#include <sys/socket.h>
#include <linux/netlink.h>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::NetserverRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	auto [pt_msg] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::RecvInline());

	HEL_CHECK(pt_msg.error());

	logRequest(logRequests, self, "NETSERVER_REQUEST", "ioctl");

	auto pt_preamble = bragi::read_preamble(pt_msg);

	auto [offer, recv_resp] = co_await [&pt_preamble, &pt_msg, conversation]() -> async::result<std::pair<helix_ng::OfferResult, helix_ng::RecvInlineResult>> {
		std::vector<uint8_t> pt_tail(pt_preamble.tail_size());
		auto [recv_tail] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(pt_tail.data(), pt_tail.size())
			);
		HEL_CHECK(recv_tail.error());

		auto [offer, send_req, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
			co_await net::getNetLane(),
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBuffer(pt_msg.data(), pt_msg.size()),
				helix_ng::sendBuffer(pt_tail.data(), pt_tail.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());
		co_return {std::move(offer), recv_resp};
	}();

	auto recv_preamble = bragi::read_preamble(recv_resp);
	assert(!recv_preamble.error());

	if(recv_preamble.id() == managarm::fs::IfreqReply::message_id) {
		std::vector<uint8_t> tail(recv_preamble.tail_size());
		auto [recv_tail] = co_await helix_ng::exchangeMsgs(
				offer.descriptor(),
				helix_ng::recvBuffer(tail.data(), tail.size())
			);
		HEL_CHECK(recv_tail.error());

		auto resp = *bragi::parse_head_tail<managarm::fs::IfreqReply>(recv_resp, tail);

		auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
		);

		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_tail.error());
	} else if(recv_preamble.id() == managarm::fs::IfconfReply::message_id) {
		std::vector<uint8_t> tail(recv_preamble.tail_size());
		auto [recv_tail] = co_await helix_ng::exchangeMsgs(
				offer.descriptor(),
				helix_ng::recvBuffer(tail.data(), tail.size())
			);
		HEL_CHECK(recv_tail.error());

		auto resp = *bragi::parse_head_tail<managarm::fs::IfconfReply>(recv_resp, tail);

		auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
		);

		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_tail.error());
	} else {
		std::cout << "posix: unexpected message in netserver forward" << std::endl;
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SocketRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SOCKET");

	managarm::posix::SocketResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if(req.flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
		co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	smarter::shared_ptr<File, FileHandle> file;
	if(req.domain() == AF_UNIX) {
		if(req.socktype() != SOCK_DGRAM && req.socktype() != SOCK_STREAM
		&& req.socktype() != SOCK_SEQPACKET) {
			std::println("posix: unexpected socket type {:#x}", req.socktype());
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::UNSUPPORTED_SOCKET_TYPE);
			co_return {};
		}

		if(req.protocol()) {
			std::println("posix: unexpected protocol {:#x} for socket", req.protocol());
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		auto un = un_socket::createSocketFile(req.flags() & SOCK_NONBLOCK, req.socktype());

		if(!un) {
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, un.error() | toPosixProtoError);
			co_return {};
		}

		file = std::move(un.value());
	}else if(req.domain() == AF_NETLINK) {
		assert(req.socktype() == SOCK_RAW || req.socktype() == SOCK_DGRAM);
		// NL_ROUTE gets handled by the netserver.
		if(req.protocol() == NETLINK_ROUTE) {
			auto ext = co_await extern_socket::createSocket(
				co_await net::getNetLane(),
				req.domain(),
				req.socktype(), req.protocol(),
				req.flags() & SOCK_NONBLOCK
			);
			if(!ext) {
				co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, ext.error() | toPosixProtoError);
				co_return {};
			}
			file = std::move(ext.value());
		}else if(netlink::nl_socket::protocol_supported(req.protocol()))
			file = netlink::nl_socket::createSocketFile(req.protocol(), req.socktype(), req.flags() & SOCK_NONBLOCK);
		else {
			std::cout << std::format("posix: unhandled netlink protocol 0x{:X}",
				req.protocol()) << std::endl;
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	} else if (req.domain() == AF_INET || req.domain() == AF_PACKET) {
		auto ext = co_await extern_socket::createSocket(
			co_await net::getNetLane(),
			req.domain(),
			req.socktype(), req.protocol(),
			req.flags() & SOCK_NONBLOCK);
		if(!ext) {
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, ext.error() | toPosixProtoError);
			co_return {};
		}
		file = std::move(ext.value());
	}else if(req.domain() == AF_VSOCK) {
		if(req.socktype() != SOCK_STREAM) {
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::UNSUPPORTED_SOCKET_TYPE);
			co_return {};
		}
		if(req.protocol() && req.protocol() != PF_VSOCK) {
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::PROTOCOL_NOT_SUPPORTED);
			co_return {};
		}

		auto lane = vsock::getLane();
		if(!lane) {
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::ADDRESS_FAMILY_NOT_SUPPORTED);
			co_return {};
		}

		auto ext = co_await extern_socket::createSocket(
			*lane,
			req.domain(),
			req.socktype(), req.protocol(),
			req.flags() & SOCK_NONBLOCK);
		if(!ext) {
			co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, ext.error() | toPosixProtoError);
			co_return {};
		}
		file = std::move(ext.value());
	}else{
		std::cout << "posix: SOCKET: Handle unknown protocols families, this is: " << req.domain() << std::endl;
		co_await sendErrorResponse<managarm::posix::SocketResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto fd = self->fileContext()->attachFile(file,
			req.flags() & SOCK_CLOEXEC);

	if (fd) {
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
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
HandleRequest::operator()(managarm::posix::SockpairRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SOCKPAIR");

	assert(!(req.flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

	if(req.domain() != AF_UNIX) {
		std::cout << "\e[31mposix: socketpair() with domain " << req.domain() <<
				" is not implemented correctly\e[39m" << std::endl;
		co_await sendErrorResponse<managarm::posix::SockpairResponse>(conversation, managarm::posix::Errors::ADDRESS_FAMILY_NOT_SUPPORTED);
		co_return {};
	}
	if(req.socktype() != SOCK_DGRAM && req.socktype() != SOCK_STREAM
			&& req.socktype() != SOCK_SEQPACKET) {
		std::cout << "\e[31mposix: socketpair() with socktype " << req.socktype() <<
				" is not implemented correctly\e[39m" << std::endl;
		co_await sendErrorResponse<managarm::posix::SockpairResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}
	if(req.protocol() && req.protocol() != PF_UNSPEC) {
		std::cout << "\e[31mposix: socketpair() with protocol " << req.protocol() <<
				" is not implemented correctly\e[39m" << std::endl;
		co_await sendErrorResponse<managarm::posix::SockpairResponse>(conversation, managarm::posix::Errors::PROTOCOL_NOT_SUPPORTED);
		co_return {};
	}

	auto pair = un_socket::createSocketPair(self.get(), req.flags() & SOCK_NONBLOCK, req.socktype());
	auto fd0 = self->fileContext()->attachFile(std::get<0>(pair),
			req.flags() & SOCK_CLOEXEC);
	auto fd1 = self->fileContext()->attachFile(std::get<1>(pair),
			req.flags() & SOCK_CLOEXEC);

	managarm::posix::SockpairResponse resp;
	if (fd0 && fd1) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fds({fd0.value(), fd1.value()});
	} else {
		resp.set_error((!fd0 ? fd0.error() : fd1.error()) | toPosixProtoError);
		if (fd0)
			self->fileContext()->closeFile(fd0.value());
		if (fd1)
			self->fileContext()->closeFile(fd1.value());
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
HandleRequest::operator()(managarm::posix::AcceptRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "ACCEPT", "fd={}", req.fd());

	auto sockfile = self->fileContext()->getFile(req.fd());
	if(!sockfile) {
		co_await sendErrorResponse<managarm::posix::AcceptResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	auto newfileResult = co_await sockfile->accept(self.get());
	if(!newfileResult) {
		co_await sendErrorResponse<managarm::posix::AcceptResponse>(conversation, newfileResult.error() | toPosixProtoError);
		co_return {};
	}
	auto newfile = newfileResult.value();
	auto fd = self->fileContext()->attachFile(std::move(newfile));

	managarm::posix::AcceptResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
