#include <cstring>
#include <format>
#include <mutex>
#include <print>

#include <arch/bit.hpp>
#include <arpa/inet.h>
#include <async/algorithm.hpp>
#include <async/basic.hpp>
#include <async/queue.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <core/clock.hpp>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <protocols/fs/server.hpp>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "checksum.hpp"
#include "icmp.hpp"

using namespace protocols::fs;

namespace {

constexpr bool debugIcmp = false;
constexpr bool logDiscards = false;

} // namespace

bool IcmpPacket::parse(smarter::shared_ptr<const Ip4Packet> packet) {
	if (packet->payload().size() < sizeof(header)) {
		if (logDiscards)
			std::println("netserver: Discarding ICMP packet smaller than the header");
		return false;
	}

	Checksum csum;
	csum.update(packet->payload());
	auto sum = csum.finalize();
	if (sum != 0 && sum != 0xFFFF) {
		if (logDiscards)
			std::println("netserver: Discarding ICMP packet with invalid checksum");
		return false;
	}

	auto now = clk::getRealtime();
	TIMESPEC_TO_TIMEVAL(&recvTimestamp, &now);

	this->packet = std::move(packet);
	return true;
}

struct IcmpSocket {
	IcmpSocket(Icmp *parent) : parent_(parent) {}
	~IcmpSocket();

	void handleClose();

	static smarter::shared_ptr<IcmpSocket> make_socket(Icmp *parent);

	static async::result<RecvResult> recvmsg(void *obj,
			helix_ng::CredentialsView creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		using arch::endian, arch::convert_endian;
		(void) creds;
		(void) flags;

		if(flags & ~MSG_DONTWAIT) {
			std::cout << std::format("netserver: unsupported flags {:#x} in ICMP recvmsg", flags) << std::endl;
			co_return Error::illegalArguments;
		}

		auto self = static_cast<IcmpSocket *>(obj);
		if(self->queue_.empty() && flags & MSG_DONTWAIT)
			co_return Error::wouldBlock;

		auto element = co_await self->queue_.async_get();

		auto copySize = std::min(element->payload().size(), len);
		std::memcpy(data, element->payload().data(), copySize);

		sockaddr_in addr {
			.sin_family = AF_INET,
			.sin_addr = { convert_endian<endian::big>(element->packet->header.source) },
		};

		std::memset(addr_buf, 0, addr_size);
		std::memcpy(addr_buf, &addr, std::min(addr_size, sizeof(addr)));

		protocols::fs::CtrlBuilder ctrl{max_ctrl_len};

		if(self->ipPacketInfo_) {
			auto truncated = ctrl.message(IPPROTO_IP, IP_PKTINFO, sizeof(struct in_pktinfo));
			if(!truncated)
				ctrl.write<struct in_pktinfo>({
					.ipi_ifindex = element->link.lock()->index(),
					.ipi_spec_dst = { .s_addr = convert_endian<endian::big>(element->packet->header.destination) },
					.ipi_addr = { .s_addr = convert_endian<endian::big>(element->packet->header.source) },
				});
		}

		if(self->timestamp_) {
			auto truncated = ctrl.message(SOL_SOCKET, SCM_TIMESTAMP, sizeof(struct timeval));
			if(!truncated)
				ctrl.write(element->recvTimestamp);
		}

		if(self->ipRecvTtl_) {
			auto truncated = ctrl.message(SOL_IP, IP_TTL, sizeof(int));
			if(!truncated)
				ctrl.write<int>(element->packet->header.ttl);
		}

		if(self->ipRetOpts_) {
			arch::dma_buffer_view options = element->packet->header_view().subview(sizeof(Ip4Packet::Header));

			if(options.size()) {
				auto truncated = ctrl.message(SOL_IP, IP_RETOPTS, options.size());
				if(!truncated)
					ctrl.write_buffer(options);
			}
		}

		co_return RecvData{ctrl.buffer(), copySize, sizeof(addr), 0};
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendmsg(void *,
			helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t len,
			void *addr_ptr, size_t addr_size,
			std::vector<uint32_t> fds, struct ucred) {
		using arch::endian;
		(void) creds;
		(void) flags;
		(void) fds;

		if(flags) {
			std::cout << std::format("netserver: unsupported flags {:#x} in ICMP sendmsg", flags) << std::endl;
			co_return Error::illegalArguments;
		}

		if(len < sizeof(IcmpPacket::Header))
			co_return Error::illegalArguments;

		IcmpPacket::Header header{};
		memcpy(&header, data, sizeof(header));

		if(header.type != ICMP_ECHO || header.code != 0)
			co_return Error::illegalArguments;

		if(addr_size < sizeof(sockaddr_in))
			co_return Error::illegalArguments;

		sockaddr_in target;
		memcpy(&target, addr_ptr, std::min(addr_size, sizeof(sockaddr_in)));

		auto ti = co_await ip4().targetByRemote(arch::convert_endian<endian::big, endian::native>(target.sin_addr.s_addr));
		if (!ti)
			co_return protocols::fs::Error::netUnreachable;

		// Linux fills in the checksum for ping sockets, so callers may leave it zero.
		std::vector<char> message(len);
		memcpy(message.data(), data, len);
		auto messageHeader = reinterpret_cast<IcmpPacket::Header *>(message.data());
		messageHeader->checksum = 0;

		Checksum csum;
		csum.update(message.data(), message.size());
		messageHeader->checksum = htons(csum.finalize());

		auto error = co_await ip4().sendFrame(std::move(*ti),
			message.data(), message.size(),
			static_cast<uint16_t>(IpProto::icmp));

		if (error != protocols::fs::Error::none)
			co_return error;

		co_return len;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *obj, uint64_t past_seq, int mask, async::cancellation_token cancellation) {
		auto self = static_cast<IcmpSocket *>(obj);
		int edges = 0;

		if(past_seq > self->currentSeq_)
			co_return protocols::fs::Error::illegalArguments;

		while(true) {
			// For now making sockets always writable is sufficient.
			edges = EPOLLOUT;
			if(self->inSeq_ > past_seq)
				edges |= EPOLLIN;

			if (edges & mask)
				break;

			if (!co_await self->statusBell_.async_wait(cancellation))
				break;
		}

		co_return protocols::fs::PollWaitResult(self->currentSeq_, edges & mask);
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *obj) {
		auto self = static_cast<IcmpSocket *>(obj);
		int events = EPOLLOUT;
		if(!self->queue_.empty())
			events |= EPOLLIN;

		co_return protocols::fs::PollStatusResult(self->currentSeq_, events);
	}

	static async::result<frg::expected<Error>> setSocketOption(void *obj,
		int layer, int number, std::vector<char> optbuf) {
		auto self = static_cast<IcmpSocket *>(obj);

		if(layer == SOL_IP && number == IP_PKTINFO) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipPacketInfo_ = (val != 0);
		} else if(layer == SOL_IP && number == IP_RECVTTL) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipRecvTtl_ = (val != 0);
		} else if(layer == SOL_IP && number == IP_RETOPTS) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipRetOpts_ = (val != 0);
		} else if(layer == SOL_SOCKET && number == SO_TIMESTAMP) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->timestamp_ = (val != 0);
		} else {
			printf("netserver: unhandled setsockopt layer %d number %d\n", layer, number);
			co_return protocols::fs::Error::invalidProtocolOption;
		}

		co_return {};
	}

	constexpr static FileOperations ops {
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.recvMsg = &recvmsg,
		.sendMsg = &sendmsg,
		.setSocketOption = &setSocketOption,
	};

public:
	async::queue<IcmpPacket, frg::stl_allocator> queue_;
	Icmp *parent_;

	async::recurring_event statusBell_;
	uint64_t currentSeq_;
	uint64_t inSeq_;

	frg::default_list_hook<IcmpSocket> listHook_;

	bool ipPacketInfo_ = false;
	bool ipRecvTtl_ = false;
	bool ipRetOpts_ = false;
	bool timestamp_ = false;
};

frg::intrusive_list<IcmpSocket, frg::locate_member<IcmpSocket,
	frg::default_list_hook<IcmpSocket>, &IcmpSocket::listHook_>> sockets;

IcmpSocket::~IcmpSocket() { }

void IcmpSocket::handleClose() {
	sockets.erase(this);
}

smarter::shared_ptr<IcmpSocket> IcmpSocket::make_socket(Icmp *parent) {
	auto s = smarter::make_shared<IcmpSocket>(parent);
	sockets.insert(sockets.end(), s.get());
	return s;
}

Icmp::Icmp() {
	async::detach(dispatchIcmp_());
}

// Dispatch ICMP messages that need to be handled by the IP stack
// (as opposed to messages that are simply forwarded to ICMP sockets).
async::result<void> Icmp::dispatchIcmp_() {
	while (true) {
		auto packet = co_await queue_.async_get();
		assert(packet); // Since async_get() is never cancelled.

		auto link = packet->link.lock();
		if (!link) {
			std::println("netserver: Link disappeared during ICMP processing");
			continue;
		}

		IcmpPacket::Header header;
		// Packets in the queue were already validated.
		assert(packet->packet->payload().size() >= sizeof(IcmpPacket::Header));
		memcpy(&header, packet->packet->payload().data(), sizeof(IcmpPacket::Header));

		if (header.type == ICMP_ECHO && header.code == 0) {
			size_t replySize = packet->packet->payload().size();
			arch::dma_buffer replyBuffer(link->dmaPool(), replySize);
			memcpy(replyBuffer.data(), packet->packet->payload().data(), replySize);

			auto replyHeader = new (replyBuffer.data()) IcmpPacket::Header;
			replyHeader->type = ICMP_ECHOREPLY;
			replyHeader->checksum = 0;

			Checksum checksum;
			checksum.update(replyBuffer);
			replyHeader->checksum = htons(checksum.finalize());

			if (debugIcmp)
				std::println("netserver: Sending ICMP echo reply");
			auto remoteIp = packet->packet->header.source;
			auto targetInfo = co_await ip4().targetByRemote(remoteIp);
			if (!targetInfo) {
				std::println("netserver: No route for ICMP echo reply");
				continue;
			}

			auto error = co_await ip4().sendFrame(
				std::move(*targetInfo),
				replyBuffer.data(),
				replyBuffer.size(),
				static_cast<uint16_t>(IpProto::icmp)
			);
			if (error != protocols::fs::Error::none) {
				std::println("netserver: Failed to send ICMP echo reply");
				continue;
			}
		}
	}
}

void Icmp::feedDatagram(smarter::shared_ptr<const Ip4Packet> packet, std::weak_ptr<nic::Link> link) {
	IcmpPacket icmp{ .link = link };
	if (!icmp.parse(std::move(packet))) {
		return;
	}

	IcmpPacket::Header header;
	// This is checked by IcmpPacket::parse().
	assert(icmp.packet->payload().size() >= sizeof(IcmpPacket::Header));
	memcpy(&header, icmp.packet->payload().data(), sizeof(IcmpPacket::Header));

	// Like Linux' icmp_echo_ignore_broadcasts, so that we cannot act as a smurf amplifier.
	bool dispatch = header.type == ICMP_ECHO
			&& ip4().classifyAddress(icmp.packet->header.destination) == Ip4::AddressType::unicast;

	if (!sockets.empty() || dispatch)
		icmp.packet = Ip4Packet::retain(std::move(icmp.packet));

	for(auto s : sockets) {
		s->queue_.emplace(IcmpPacket{icmp});
		s->inSeq_ = ++s->currentSeq_;
		s->statusBell_.raise();
	}

	if (dispatch)
		this->queue_.emplace(icmp);
}

static async::result<void> serveLanes(
	helix::UniqueLane ctrlLane,
	helix::UniqueLane ptLane,
	smarter::shared_ptr<IcmpSocket> sock
) {
	co_await async::race_and_cancel(
		[&](async::cancellation_token) {
			return protocols::fs::serveFile(std::move(ctrlLane),
					sock.get(), &IcmpSocket::ops);
		},
		[&](async::cancellation_token ct) {
			return protocols::fs::servePassthrough(std::move(ptLane),
					sock, &IcmpSocket::ops, ct);
		}
	);
	sock->handleClose();
}

void Icmp::serveSocket(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane) {
	auto sock = IcmpSocket::make_socket(this);
	async::detach(serveLanes(std::move(ctrlLane), std::move(ptLane), std::move(sock)));
}
