#include "netlink.hpp"

#include <linux/rtnetlink.h>
#include <memory>
#include <sys/epoll.h>

extern std::optional<helix::UniqueDescriptor> posixLane;

using core::netlink::Group;

namespace {

constexpr bool logGroups = false;
constexpr bool logSocket = false;

/* groupid -> std::unique_ptr<Group> */
std::map<unsigned, std::unique_ptr<Group>> globalGroupMap;

} /* namespace */

namespace nl {

NetlinkSocket::NetlinkSocket(int flags, int protocol)
: protocol{protocol}, flags(flags)
{ }

void NetlinkSocket::handleClose() {
	core::netlink::NetlinkFile *self = this;
	for(auto &[id, group] : globalGroupMap) {
		std::erase(group->subscriptions, self);
	}
}

async::result<frg::expected<protocols::fs::Error, size_t>> NetlinkSocket::sockname(void *, void *addr_ptr, size_t max_addr_length) {
	// TODO: Fill in nl_groups.
	struct sockaddr_nl sa{};
	sa.nl_family = AF_NETLINK;
	memcpy(addr_ptr, &sa, std::min(sizeof(struct sockaddr_nl), max_addr_length));

	co_return sizeof(struct sockaddr_nl);
};

async::result<protocols::fs::RecvResult> NetlinkSocket::recvMsg(void *obj,
		helix_ng::CredentialsView creds, uint32_t flags, void *data,
		size_t len, void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
	(void) creds;

	auto *self = static_cast<NetlinkSocket *>(obj);
	if(logSocket)
		std::cout << "netserver: Recv from netlink socket" << std::endl;

	if(self->_recvQueue.empty() && (self->_nonBlock || (flags & MSG_DONTWAIT)))
		co_return protocols::fs::Error::wouldBlock;

	while(self->_recvQueue.empty())
		co_await self->_statusBell.async_wait();

	auto &packet = self->_recvQueue.front();

	const auto size = packet.buffer.size();
	auto truncated_size = std::min(size, len);

	if(size && data != nullptr)
		memcpy(data, packet.buffer.data(), truncated_size);

	if(addr_size >= sizeof(struct sockaddr_nl) && addr_buf != nullptr) {
		struct sockaddr_nl sa{};
		sa.nl_family = AF_NETLINK;
		sa.nl_groups = packet.group ? (1U << (packet.group - 1)) : 0;

		memcpy(addr_buf, &sa, sizeof(struct sockaddr_nl));
	}

	protocols::fs::CtrlBuilder ctrl{max_ctrl_len};
	uint32_t reply_flags = 0;

	if(self->_passCreds) {
		struct ucred ucreds;
		auto senderPid = 0;

		managarm::fs::ResolveCredentialsToPidReq creds_resolve_req{};

		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(*posixLane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(creds_resolve_req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		recv_resp.reset();

		memset(&ucreds, 0, sizeof(struct ucred));
		ucreds.pid = senderPid;

		auto truncated = ctrl.message(SOL_SOCKET, SCM_CREDENTIALS, sizeof(struct ucred));
		if(truncated)
			reply_flags |= MSG_CTRUNC;
		else
			ctrl.write(ucreds);
	}

	if(self->pktinfo_) {
		struct nl_pktinfo info;
		info.group = packet.group;
		auto truncated = ctrl.message(SOL_NETLINK, NETLINK_PKTINFO, sizeof(info));
		if(!truncated)
			ctrl.write(info);
		else
			reply_flags |= MSG_CTRUNC;
	}

	if(!(flags & MSG_PEEK))
		self->_recvQueue.pop_front();


	if(!(flags & MSG_TRUNC) && truncated_size < size) {
		reply_flags |= MSG_TRUNC;
	}

	co_return protocols::fs::RecvData{ctrl.buffer(), size, sizeof(struct sockaddr_nl), reply_flags};
}

async::result<frg::expected<protocols::fs::Error, size_t>> NetlinkSocket::sendMsg(void *obj,
			helix_ng::CredentialsView creds, uint32_t flags, void *data, size_t len,
			void *addr_ptr, size_t addr_size, std::vector<uint32_t> fds, struct ucred) {
	(void) creds;
	(void) addr_ptr;
	(void) addr_size;

	if(logSocket)
		std::cout << "netserver: sendMsg on netlink socket!" << std::endl;
	const auto orig_len = len;
	auto self = static_cast<NetlinkSocket *>(obj);

	if(flags) {
		std::cout << "netserver: flags in netlink sendMsg unsupported, returning EINVAL"
			<< std::endl;
		co_return protocols::fs::Error::illegalArguments;
	}

	if(!fds.empty()) {
		std::cout << "netserver: fds in netlink sendMsg unsupported, returning EINVAL"
			<< std::endl;
		co_return protocols::fs::Error::illegalArguments;
	}

	auto hdr = static_cast<struct nlmsghdr *>(data);
	for(; NLMSG_OK(hdr, len); hdr = NLMSG_NEXT(hdr, len)) {
		if(hdr->nlmsg_type == NLMSG_DONE)
			co_return orig_len;

		// TODO: maybe send an error packet back instead of erroring here?
		if(hdr->nlmsg_type == NLMSG_ERROR)
			co_return protocols::fs::Error::illegalArguments;

		// Like Linux, only handle requests; anything else is at most acknowledged.
		if(!(hdr->nlmsg_flags & NLM_F_REQUEST)) {
			if(hdr->nlmsg_flags & NLM_F_ACK)
				sendAck(self, hdr);
			continue;
		}

		if(hdr->nlmsg_type == RTM_NEWROUTE) {
			self->newRoute(hdr);
		} else if(hdr->nlmsg_type == RTM_GETROUTE) {
			self->getRoute(hdr);
		} else if(hdr->nlmsg_type == RTM_DELROUTE) {
			self->deleteRoute(hdr);
		} else if(hdr->nlmsg_type == RTM_NEWLINK) {
			sendError(self, hdr, EPERM);
		} else if(hdr->nlmsg_type == RTM_GETLINK) {
			self->getLink(hdr);
		} else if(hdr->nlmsg_type == RTM_DELLINK) {
			sendError(self, hdr, EPERM);
		} else if(hdr->nlmsg_type == RTM_NEWADDR) {
			self->newAddr(hdr);
		} else if(hdr->nlmsg_type == RTM_GETADDR) {
			self->getAddr(hdr);
		} else if(hdr->nlmsg_type == RTM_DELADDR) {
			self->deleteAddr(hdr);
		} else if(hdr->nlmsg_type == RTM_GETNEIGH) {
			self->getNeighbor(hdr);
		} else {
			std::cout << "netlink: unknown nlmsg_type " << hdr->nlmsg_type << std::endl;
			sendError(self, hdr, EOPNOTSUPP);
		}
	}

	co_return orig_len;
}

async::result<protocols::fs::Error> NetlinkSocket::bind(void *obj, helix_ng::CredentialsView creds,
		const void *addr_ptr, size_t addr_length) {
	(void) creds;

	auto self = static_cast<NetlinkSocket *>(obj);

	if(addr_length < sizeof(struct sockaddr_nl))
		co_return protocols::fs::Error::illegalArguments;

	struct sockaddr_nl sa;
	memcpy(&sa, addr_ptr, addr_length);

	if(sa.nl_groups) {
		for(int i = 0; i < 32; i++) {
			if(!(sa.nl_groups & (1 << i)))
				continue;
			if(logGroups)
				std::cout << std::format("netserver: joining netlink group 0x{:x}\n", (i + 1));

			auto it = globalGroupMap.find(i + 1);
			assert(it != globalGroupMap.end());
			auto group = it->second.get();
			group->subscriptions.push_back(self);
			self->groupMemberships_.set(i + 1);
		}
	}

	co_return protocols::fs::Error::none;
}

async::result<void> NetlinkSocket::setFileFlags(void *object, int flags) {
	auto self = static_cast<NetlinkSocket *>(object);
	if(flags & ~(O_NONBLOCK | O_RDONLY | O_WRONLY | O_RDWR)) {
		std::cout << std::format("posix: setFileFlags on rtnetlink socket called with unknown flags 0x{:x}\n", flags & ~O_NONBLOCK);
		co_return;
	}
	if(flags & O_NONBLOCK)
		self->_nonBlock = true;
	else
		self->_nonBlock = false;
	co_return;
}

async::result<int> NetlinkSocket::getFileFlags(void *object) {
	auto self = static_cast<NetlinkSocket *>(object);

	int flags = O_RDWR;
	if(self->_nonBlock)
		flags |= O_NONBLOCK;
	co_return flags;
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
NetlinkSocket::pollWait(void *obj, uint64_t past_seq, int mask, async::cancellation_token ct) {
	auto self = static_cast<NetlinkSocket *>(obj);
	assert(past_seq <= self->_currentSeq);
	int edges = 0;

	while (true) {
		if(self->_isClosed)
			co_return protocols::fs::Error::internalError;

		// For now making sockets always writable is sufficient.
		edges = EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
		if(self->_inSeq > past_seq)
			edges |= EPOLLIN | EPOLLRDNORM;

		if (edges & mask)
			break;

		if (!co_await self->_statusBell.async_wait(ct))
			break;
	}

	// std::println("posix: pollWait({}, {}) on \e[1;34mnetserver.nl-socket\e[0m returns ({}, {})",
	// 	past_seq, mask, self->_currentSeq, edges & mask);

	co_return protocols::fs::PollWaitResult(self->_currentSeq, edges & mask);
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>> NetlinkSocket::pollStatus(void *obj) {
	auto self = static_cast<NetlinkSocket *>(obj);
	int events = EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
	if(!self->_recvQueue.empty())
		events |= EPOLLIN | EPOLLRDNORM;

	co_return protocols::fs::PollStatusResult(self->_currentSeq, events);
}

async::result<frg::expected<protocols::fs::Error>> NetlinkSocket::setSocketOption(void *object,
		int layer, int number, std::vector<char> optbuf) {
	auto self = static_cast<NetlinkSocket *>(object);

	if(layer == SOL_SOCKET && number == SO_PASSCRED) {
		if(optbuf.size() >= sizeof(int))
			self->_passCreds = *reinterpret_cast<int *>(optbuf.data());
		co_return {};
	}

	if(layer != SOL_NETLINK)
		co_return protocols::fs::Error::illegalArguments;

	switch(number) {
		case NETLINK_ADD_MEMBERSHIP: {
			auto val = *reinterpret_cast<int *>(optbuf.data());
			if(!val)
				co_return protocols::fs::Error::illegalArguments;

			if(!globalGroupMap.contains(val)) {
				std::cout << std::format("netserver: attempt to join invalid netlink group 0x{:x}\n", val);
				co_return protocols::fs::Error::illegalArguments;
			}

			auto it = globalGroupMap.find(val);
			assert(it != globalGroupMap.end());
			auto group = it->second.get();
			group->subscriptions.push_back(self);
			self->groupMemberships_.set(val);

			if(logGroups)
				std::cout << std::format("netserver: joining netlink group 0x{:x}\n", val);
			break;
		}
		case NETLINK_PKTINFO: {
			auto val = *reinterpret_cast<int *>(optbuf.data());
			self->pktinfo_ = val;
			break;
		}
		default:
			std::cout << std::format("netserver: unknown setsockopt 0x{:x}\n", number);
			co_return protocols::fs::Error::illegalArguments;
	}

	co_return {};
}

async::result<frg::expected<protocols::fs::Error>> NetlinkSocket::getSocketOption(void *object,
helix_ng::CredentialsView, int layer, int number, std::vector<char> &optbuf) {
	auto self = static_cast<NetlinkSocket *>(object);

	if(layer == SOL_SOCKET && number == SO_PROTOCOL) {
		memcpy(optbuf.data(), &self->protocol, std::min(optbuf.size(), sizeof(self->protocol)));
	} else if(layer == SOL_NETLINK && number == NETLINK_LIST_MEMBERSHIPS) {
		auto written = self->groupMemberships_.writeList(std::span<uint32_t>{
			reinterpret_cast<uint32_t *>(optbuf.data()), optbuf.size() / sizeof(uint32_t)
		});
		optbuf.resize(written * sizeof(uint32_t));
	} else if(layer == SOL_SOCKET && number == SO_TYPE) {
		// Netlink is datagram-oriented, and the protocol does not differentiate between
		// SOCK_RAW and SOCK_DGRAM, so we unconditionally return SOCK_DGRAM here.
		auto type_ = SOCK_DGRAM;
		optbuf.resize(std::min(optbuf.size(), sizeof(type_)));
		memcpy(optbuf.data(), &type_, optbuf.size());
	} else {
		std::cout << std::format("netserver: unhandled netlink socket getsockopt layer {} number {}\n",
				layer, number);
		co_return protocols::fs::Error::invalidProtocolOption;
	}

	co_return {};
}

void broadcast(core::netlink::Packet packet) {
	if(!packet.group)
		return;

	auto it = globalGroupMap.find(packet.group);
	assert(it != globalGroupMap.end());
	auto group = it->second.get();
	group->carbonCopy(packet);
}

std::array<rtnetlink_groups, 34> supported_groups = {
	RTNLGRP_LINK,
	RTNLGRP_NOTIFY,
	RTNLGRP_NEIGH,
	RTNLGRP_TC,
	RTNLGRP_IPV4_IFADDR,
	RTNLGRP_IPV4_MROUTE,
	RTNLGRP_IPV4_ROUTE,
	RTNLGRP_IPV4_RULE,
	RTNLGRP_IPV6_IFADDR,
	RTNLGRP_IPV6_MROUTE,
	RTNLGRP_IPV6_ROUTE,
	RTNLGRP_IPV6_IFINFO,
	RTNLGRP_DECnet_IFADDR,
	RTNLGRP_DECnet_ROUTE,
	RTNLGRP_DECnet_RULE,
	RTNLGRP_IPV6_PREFIX,
	RTNLGRP_IPV6_RULE,
	RTNLGRP_ND_USEROPT,
	RTNLGRP_PHONET_IFADDR,
	RTNLGRP_PHONET_ROUTE,
	RTNLGRP_DCB,
	RTNLGRP_IPV4_NETCONF,
	RTNLGRP_IPV6_NETCONF,
	RTNLGRP_MDB,
	RTNLGRP_MPLS_ROUTE,
	RTNLGRP_NSID,
	RTNLGRP_MPLS_NETCONF,
	RTNLGRP_IPV4_MROUTE_R,
	RTNLGRP_IPV6_MROUTE_R,
	RTNLGRP_NEXTHOP,
	RTNLGRP_BRVLAN,
	RTNLGRP_MCTP_IFADDR,
	RTNLGRP_TUNNEL,
	RTNLGRP_STATS,
};

void initialize() {
	for(auto group : supported_groups) {
		auto res = globalGroupMap.insert({group, std::make_unique<Group>()});
		assert(res.second);
	}
}

} // namespace nl
