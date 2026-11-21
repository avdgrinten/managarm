#include "udp4.hpp"

#include "ip4.hpp"
#include "checksum.hpp"
#include "../receive-budget.hpp"

#include <async/algorithm.hpp>
#include <async/basic.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>
#include <arch/bit.hpp>
#include <protocols/fs/server.hpp>
#include <cstring>
#include <iomanip>
#include <print>
#include <queue>
#include <random>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

namespace {

constexpr bool logSockets = false;
constexpr bool dumpHeader = false;
constexpr bool logDiscards = false;

struct stl_allocator {
	void *allocate(size_t size) {
		return operator new(size);
	}

	void deallocate(void *p, size_t) {
		return operator delete(p);
	}
};

template<typename T>
void maybeFlip(T &x) {
	x = arch::convert_endian<arch::endian::big, arch::endian::native>(x);
}

struct PseudoHeader {
	uint32_t src;
	uint32_t dst;

	uint8_t zero = 0;
	uint8_t proto = static_cast<uint8_t>(IpProto::udp);
	uint16_t len;

	void ensureEndian() {
		maybeFlip(src);
		maybeFlip(dst);
		maybeFlip(proto);
		maybeFlip(len);
	}
};
} // namespace

struct Udp {
	struct Header {
		uint16_t src;
		uint16_t dst;
		uint16_t len;
		uint16_t chk;

		void ensureEndian() {
			maybeFlip(src);
			maybeFlip(dst);
			maybeFlip(len);
			maybeFlip(chk);
		}
	} header;
	static_assert(sizeof(header) == 8, "udp header size wrong");

	arch::dma_buffer_view payload() const {
		return packet->payload().subview(sizeof(header), header.len - sizeof(header));
	}

	bool parse(smarter::shared_ptr<const Ip4Packet> packet) {
		Checksum chk;
		auto payload = packet->payload();
		if (payload.size() < sizeof(header)) {
			if (logDiscards)
				std::println("netserver: Discarding UDP packet smaller than the header");
			return false;
		}
		std::memcpy(&header, payload.data(), sizeof(header));
		header.ensureEndian();
		// The length field covers the UDP header and its payload.
		if (header.len < sizeof(header)) {
			if (logDiscards)
				std::println("netserver: Discarding UDP packet with a length field below the header size");
			return false;
		}
		if (payload.size() < header.len) {
			if (logDiscards)
				std::println("netserver: Discarding UDP packet smaller than its length field");
			return false;
		}
		// Bytes behind the datagram (e.g. Ethernet padding) are not covered by the checksum.
		payload = payload.subview(0, header.len);
		if (header.chk != 0) {
			PseudoHeader phdr;
			phdr.src = packet->header.source;
			phdr.dst = packet->header.destination;
			phdr.proto = packet->header.protocol;
			phdr.len = payload.size();
			phdr.ensureEndian();

			chk.update(&phdr, sizeof(phdr));
			chk.update(payload);
			auto fin = chk.finalize();
			if (fin != 0 && ~fin != 0) {
				if (logDiscards)
					std::println("netserver: Discarding UDP packet with invalid checksum");
				return false;
			}
		}
		this->packet = std::move(packet);
		return true;
	}

	smarter::shared_ptr<const Ip4Packet> packet;

	std::weak_ptr<nic::Link> link;
};

Endpoint &Endpoint::operator=(struct sockaddr_in sa) {
	using arch::convert_endian;
	using arch::endian;
	family = sa.sin_family;
	port = convert_endian<endian::big, endian::native>(sa.sin_port);
	addr = convert_endian<endian::big,
	     endian::native>(sa.sin_addr.s_addr);
	return *this;
}

void Endpoint::ensureEndian() {
	maybeFlip(addr);
	maybeFlip(port);
}


bool operator<(const Endpoint &l, const Endpoint &r) {
	return std::tie(l.port, l.addr) < std::tie(r.port, r.addr);
}

namespace {
auto checkAddress(const void *addr_ptr, size_t addr_len, Endpoint &e) {
	struct sockaddr_in addr;
	if (addr_len < sizeof(addr)) {
		return protocols::fs::Error::illegalArguments;
	}
	std::memcpy(&addr, addr_ptr, sizeof(addr));
	if (addr.sin_family == AF_UNSPEC) {
		e = Endpoint{};
		return protocols::fs::Error::none;
	} else if (addr.sin_family != AF_INET) {
		return protocols::fs::Error::afNotSupported;
	}
	e = addr;
	return protocols::fs::Error::none;
}
} // namespace

using namespace protocols::fs;

struct Udp4Socket {
	Udp4Socket(Udp4 *parent, bool nonBlock) : parent_(parent), nonBlock_(nonBlock) {}

	~Udp4Socket() {
		if (logSockets)
			std::println("netserver: UDP socket destructed");
	}

	void handleClose() {
		if (logSockets)
			std::println("netserver: UDP socket closed");
		parent_->unbind(local_);
	}

	static auto make_socket(Udp4 *parent, int flags) {
		auto s = smarter::make_shared<Udp4Socket>(parent, flags & SOCK_NONBLOCK);
		s->holder_ = s;
		return s;
	}

	static async::result<protocols::fs::Error> connect(void* obj,
			helix_ng::CredentialsView creds,
			const void *addr_ptr, size_t addr_size) {
		(void) creds;

		auto self = static_cast<Udp4Socket *>(obj);
		Endpoint remote;

		if (auto e = checkAddress(addr_ptr, addr_size, remote);
			e != protocols::fs::Error::none) {
			co_return e;
		}

		if (remote.family == AF_UNSPEC) {
			self->remote_ = Endpoint{};
			self->parent_->unbind(self->local_);
			self->local_ = Endpoint{AF_INET, INADDR_ANY, 0};
			co_return protocols::fs::Error::none;
		}

		assert(remote.family == AF_INET);
		uint32_t bindAddr = remote.addr;

		if (remote.addr == INADDR_BROADCAST) {
			if (logSockets)
				std::cout << "netserver: broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		} else if (remote.addr == INADDR_ANY) {
			bindAddr = INADDR_LOOPBACK;
			// We want to get the following (sane) behavior:
			// No matter the port that was supplied in the connect() call, we always want
			// getsockname() to return '127.0.0.1:some-port'.
			// If connect()ing to 0.0.0.0:0 getpeername() should return ENOTCONN.
			// If a port was supplied, we want getpeername() to return '127.0.0.1' and that port.
			// This matches Linux behavior and that of most *BSDs, at least where they aren't
			// obviously wrong or broken.
			if (remote.port)
				remote.addr = INADDR_LOOPBACK;
			else
				remote = Endpoint{};
		} else {
			auto ti = co_await ip4().targetByRemote(remote.addr);
			if (!ti)
				co_return protocols::fs::Error::netUnreachable;

			auto linkAddr = ip4().getCidrByIndex(ti->link->index());
			if (!linkAddr)
				co_return protocols::fs::Error::netUnreachable;

			bindAddr = linkAddr->ip;
		}

		if (!self->local_.port && bindAddr != INADDR_ANY
				&& !self->bindAvailable(bindAddr)) {
			if (logSockets)
				std::cout << "netserver: no source port" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		self->remote_ = remote;

		// POSIX: For non-connection-mode sockets, a `connect(3)` limits the sender for
		// subsequent `recv(3)` calls;
		// We implement this by filtering the packet queue here, and rejecting packets in
		// `feedDatagram` by their source address.
		if (self->remote_.family != AF_UNSPEC && (self->remote_.port || self->remote_.addr != INADDR_ANY)) {
			std::queue<Udp> filteredPackets;

			while (auto packet = self->queue_.maybe_get()) {
				if (!self->rejectPacket(packet.value()))
					filteredPackets.push(*packet);
				else
					self->rcvBudget_.release(packet->packet->data.size());
			}

			while (!filteredPackets.empty()) {
				self->queue_.put(filteredPackets.front());
				filteredPackets.pop();
			}
		}

		co_return protocols::fs::Error::none;
	}

	static async::result<size_t> sockname(void *object, void *addr_ptr, size_t max_addr_length) {
		auto self = static_cast<Udp4Socket *>(object);
		sockaddr_in sa{};
		sa.sin_family = self->local_.family;
		sa.sin_port = htons(self->local_.port);
		sa.sin_addr.s_addr = htonl(self->local_.addr);
		memcpy(addr_ptr, &sa, std::min(sizeof(sockaddr_in), max_addr_length));

		co_return sizeof(sockaddr_in);
	}

	static async::result<Error> listen(void *) {
		co_return protocols::fs::Error::notSupported;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::AcceptResult>>
	accept(void *) {
		co_return protocols::fs::Error::notSupported;
	}

	static async::result<frg::expected<Error, size_t>> peername(void *object, void *addr_ptr, size_t max_addr_length) {
		auto self = static_cast<Udp4Socket *>(object);

		if (self->remote_.family == AF_UNSPEC)
			co_return protocols::fs::Error::notConnected;

		sockaddr_in sa{};
		sa.sin_family = self->remote_.family;
		sa.sin_port = htons(self->remote_.port);
		sa.sin_addr.s_addr = htonl(self->remote_.addr);
		memcpy(addr_ptr, &sa, std::min(sizeof(sockaddr_in), max_addr_length));

		co_return sizeof(sockaddr_in);
	}

	static async::result<protocols::fs::Error> bind(void* obj,
			helix_ng::CredentialsView creds,
			const void *addr_ptr, size_t addr_size) {
		(void) creds;

		auto self = static_cast<Udp4Socket *>(obj);
		Endpoint local;

		if (self->local_.port != 0) {
			co_return protocols::fs::Error::illegalArguments;
		}

		if (auto e = checkAddress(addr_ptr, addr_size, local);
			e != protocols::fs::Error::none) {
			co_return e;
		}

		// TODO(arsen): check other broadcast addresses too
		if (local.addr == INADDR_BROADCAST) {
			if (logSockets)
				std::cout << "netserver: broadcast" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		if (local.addr != INADDR_ANY && !ip4().hasIp(local.addr)) {
			if (logSockets)
				std::cout << "netserver: not local ip" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		if (local.port == 0) {
			if (!self->bindAvailable(local.addr))
				co_return protocols::fs::Error::addressInUse;
		} else if (!self->parent_->tryBind(self->holder_.lock(), local)) {
			if (logSockets)
				std::cout << "netserver: address in use" << std::endl;
			co_return protocols::fs::Error::addressInUse;
		}

		co_return protocols::fs::Error::none;
	}

	static async::result<RecvResult> recvmsg(void *obj,
			helix_ng::CredentialsView creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		(void) creds;

		using arch::convert_endian;
		using arch::endian;

		auto self = static_cast<Udp4Socket *>(obj);
		if(self->shutdownReadSeq_)
			co_return RecvData{{}, 0, 0, 0};
		if(self->queue_.empty() && (flags & MSG_DONTWAIT || self->nonBlock_))
			co_return Error::wouldBlock;

		auto element = co_await self->queue_.async_get();
		self->rcvBudget_.release(element->packet->data.size());
		auto packet = element->payload();
		auto copy_size = std::min(packet.size(), len);
		std::memcpy(data, packet.data(), copy_size);

		sockaddr_in addr {};
		addr.sin_family = AF_INET;
		addr.sin_port = convert_endian<endian::big>(element->header.src);
		addr.sin_addr = {
			convert_endian<endian::big>(element->packet->header.source)
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

		co_return RecvData{ctrl.buffer(), copy_size, sizeof(addr), 0};
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendmsg(void *obj,
			helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t len,
			void *addr_ptr, size_t addr_size,
			std::vector<uint32_t> fds, struct ucred) {
		(void) creds;
		(void) flags;
		(void) fds;

		using arch::convert_endian;
		using arch::endian;
		auto self = static_cast<Udp4Socket *>(obj);
		if(self->shutdownWriteSeq_)
			co_return protocols::fs::Error::brokenPipe;

		if (self->remote_.family != AF_UNSPEC && addr_size)
			co_return protocols::fs::Error::alreadyConnected;

		Endpoint target;
		auto source = self->local_;
		if (addr_size != 0) {
			if (auto e = checkAddress(addr_ptr, addr_size, target);
				e != protocols::fs::Error::none) {
				if (logSockets)
					std::cout << "netserver: trimmed sendmsg addr" << std::endl;
				co_return e;
			}
		} else {
			target = self->remote_;
		}

		if (target.addr == 0) {
			if (logSockets)
				std::println("netserver: udp needs destination address");
			co_return protocols::fs::Error::destAddrRequired;
		}

		if (target.port == 0) {
			if (logSockets)
				std::println("netserver: udp port 0 is reserved");
			co_return protocols::fs::Error::addressNotAvailable;
		}

		if (source.port == 0 && !self->bindAvailable(source.addr)) {
			if (logSockets)
				std::cout << "netserver: no source port" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		source = self->local_;

		if (target.addr == INADDR_BROADCAST) {
			if (logSockets)
				std::cout << "netserver: broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		std::vector<char> buf;
		buf.resize(sizeof(Udp::Header) + len);
		Udp::Header header {
			.src = source.port,
			.dst = target.port,
			.len = static_cast<uint16_t>(len + sizeof(Udp::Header)),
			.chk = 0,
		};
		header.ensureEndian();

		// native endian target IP
		auto targetIpNe = target.addr;
		source.ensureEndian();
		target.ensureEndian();

		auto ti = co_await ip4().targetByRemote(targetIpNe);
		if (!ti)
			co_return protocols::fs::Error::netUnreachable;

		if (self->local_.addr != INADDR_ANY)
			ti->source = self->local_.addr;

		Checksum chk;
		PseudoHeader psh {
			.src = convert_endian<endian::big>(ti->source),
			.dst = target.addr,
			.len = header.len
		};
		chk.update(&psh, sizeof(psh));
		chk.update(&header, sizeof(header));
		chk.update(data, len);
		header.chk = convert_endian<endian::big>(chk.finalize());

		if (dumpHeader)
			std::cout << "netserver:" << std::endl << std::hex
				<< std::setw(8) << psh.src << std::endl
				<< std::setw(8) << psh.dst << std::endl
				<< std::setw(8) << psh.len << std::endl

				<< std::setw(8) << header.src << std::endl
				<< std::setw(8) << header.dst << std::endl
				<< std::setw(8) << header.len << std::endl
				<< std::setw(8) << header.chk << std::endl << std::dec;

		if (header.chk == 0) {
			header.chk = ~header.chk;
		}

		std::memcpy(buf.data(), &header, sizeof(header));
		std::memcpy(buf.data() + sizeof(header), data, len);

		auto error = co_await ip4().sendFrame(std::move(*ti),
			buf.data(), buf.size(), std::to_underlying(IpProto::udp));
		if (error != protocols::fs::Error::none) {
			co_return error;
		}
		co_return len;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *obj, uint64_t past_seq, int mask, async::cancellation_token cancellation) {
		auto self = static_cast<Udp4Socket *>(obj);
		assert(past_seq <= self->_currentSeq);
		int edges = 0;

		while(true) {
			edges = 0;
			if(!(self->shutdownWriteSeq_ > past_seq))
				edges |= EPOLLOUT;
			if(self->_inSeq > past_seq)
				edges |= EPOLLIN;
			if(self->shutdownReadSeq_ > past_seq)
				edges |= EPOLLIN;

			if (edges & mask)
				break;

			if (!co_await self->_statusBell.async_wait(cancellation))
				break;
		}

		co_return protocols::fs::PollWaitResult(self->_currentSeq, edges & mask);
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *obj) {
		auto self = static_cast<Udp4Socket *>(obj);
		int events = 0;
		if(!self->shutdownWriteSeq_)
			events |= EPOLLOUT;
		if(!self->queue_.empty())
			events |= EPOLLIN;
		if(self->shutdownReadSeq_)
			events |= EPOLLIN;

		co_return protocols::fs::PollStatusResult(self->_currentSeq, events);
	}

	static async::result<frg::expected<Error>> setSocketOption(void *obj,
		int layer, int number, std::vector<char> optbuf) {
		auto self = static_cast<Udp4Socket *>(obj);

		if(layer == SOL_IP && number == IP_PKTINFO) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipPacketInfo_ = (val != 0);
		} else if(layer == SOL_SOCKET && number == SO_BINDTODEVICE) {
			std::string ifname{optbuf.data(), optbuf.size()};

			if(ifname.empty()) {
				self->boundInterface_ = {};
			} else {
				auto nic = nic::Link::byName(ifname);

				if(!nic)
					co_return protocols::fs::Error::illegalArguments;

				self->boundInterface_ = nic;
				co_return {};
			}
		} else {
			printf("netserver: unhandled UDP socket setsockopt layer %d number %d\n", layer, number);
			co_return protocols::fs::Error::invalidProtocolOption;
		}

		co_return {};
	}

	static async::result<frg::expected<protocols::fs::Error>> getSocketOption(void *object,
	helix_ng::CredentialsView, int layer, int number, std::vector<char> &optbuf) {
		auto self = static_cast<Udp4Socket *>(object);

		if(layer == SOL_SOCKET && number == SO_TYPE) {
			auto type_ = SOCK_DGRAM;
			optbuf.resize(std::min(optbuf.size(), sizeof(type_)));
			memcpy(optbuf.data(), &type_, optbuf.size());
		} else if(layer == SOL_SOCKET && number == SO_BINDTODEVICE) {
			size_t size = self->boundInterface_ ? self->boundInterface_->name().size() : 0;
			optbuf.resize(size);
			if (size)
				memcpy(optbuf.data(), self->boundInterface_->name().data(), size);
		} else {
			std::cout << std::format("netserver: unhandled UDP socket getsockopt layer {} number {}\n",
				layer, number);
			co_return protocols::fs::Error::invalidProtocolOption;
		}

		co_return {};
	}

	static async::result<void> setFileFlags(void *object, int flags) {
		auto self = static_cast<Udp4Socket *>(object);
		// Like on Linux, F_SETFL ignores the access mode (F_GETFL reports it), such that
		// fcntl(F_SETFL, fcntl(F_GETFL) | O_NONBLOCK) works.
		flags &= ~O_ACCMODE;

		if(flags & ~O_NONBLOCK) {
			std::cout << "posix: setFileFlags on udp socket called with unknown flags" << std::endl;
			co_return;
		}
		if(flags & O_NONBLOCK)
			self->nonBlock_ = true;
		else
			self->nonBlock_ = false;
		co_return;
	}


	static async::result<int> getFileFlags(void *object) {
		auto self = static_cast<Udp4Socket *>(object);
		int flags = O_RDWR;

		if(self->nonBlock_)
			flags |= O_NONBLOCK;

		co_return flags;
	}

	static async::result<Error> shutdown(void *object, int how) {
		auto self = static_cast<Udp4Socket *>(object);

		if (self->remote_.family == AF_UNSPEC)
			co_return Error::notConnected;

		if (how == SHUT_RD) {
			self->shutdownReadSeq_ = ++self->_currentSeq;
		} else if (how == SHUT_WR) {
			self->shutdownWriteSeq_ = ++self->_currentSeq;
		} else if (how == SHUT_RDWR) {
			++self->_currentSeq;
			self->shutdownReadSeq_ = self->_currentSeq;
			self->shutdownWriteSeq_ = self->_currentSeq;
		} else {
			std::println("posix: unexpected how={} for UDP socket shutdown", how);
			co_return protocols::fs::Error::illegalArguments;
		}

		self->_statusBell.raise();

		co_return Error::none;
	}

	constexpr static FileOperations ops {
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.bind = &bind,
		.listen = &listen,
		.connect = &connect,
		.accept = &accept,
		.sockname = &sockname,
		.getFileFlags = &getFileFlags,
		.setFileFlags = &setFileFlags,
		.recvMsg = &recvmsg,
		.sendMsg = &sendmsg,
		.peername = &peername,
		.setSocketOption = &setSocketOption,
		.getSocketOption = &getSocketOption,
		.shutdown = &shutdown
	};

	bool bindAvailable(uint32_t addr = INADDR_ANY) {
		static std::mt19937 rng;
		static std::uniform_int_distribution<uint16_t> dist {
			32768, 60999
		};
		// TODO(arsen): this rng probably is suboptimal, at some point
		// in the future replace it with a CSRNG or a hash function
		// see also: RFC6056, Section 3.3.3
		auto number = dist(rng);
		auto range_size = dist.b() - dist.a();
		auto shared_from_this = holder_.lock();
		// TODO(arsen): optimize to not call lower_bound every time?
		// I believe that such a thing is not needed right now: nearly
		// (read: absolutely) every case is is an immediate miss: we are
		// using next to nothing in this region, or any other region for
		// that manner
		for (int i = 0; i < range_size; i++) {
			uint16_t port = dist.a() + ((number + i) % range_size);
			if (parent_->tryBind(shared_from_this, { AF_INET, addr, port })) {
				return true;
			}
		}
		return false;
	}

private:
	bool rejectPacket(Udp &udp) const {
		if(boundInterface_ && boundInterface_->index() != udp.packet->link.lock()->index())
			return true;

		if (remote_.family == AF_UNSPEC)
			return false;
		if (remote_.port && remote_.port != udp.header.src)
			return true;
		if (remote_.addr != INADDR_ANY && remote_.addr != udp.packet->header.source)
			return true;

		return false;
	}

	friend struct Udp4;

	async::queue<Udp, stl_allocator> queue_;
	ReceiveBudget rcvBudget_;
	Endpoint remote_{};
	Endpoint local_{AF_INET, 0, 0};
	Udp4 *parent_;
	smarter::weak_ptr<Udp4Socket> holder_;

	async::recurring_event _statusBell;
	uint64_t _currentSeq;
	uint64_t _inSeq;
	uint64_t shutdownReadSeq_ = 0;
	uint64_t shutdownWriteSeq_ = 0;

	bool ipPacketInfo_ = false;
	bool nonBlock_ = false;

	std::shared_ptr<nic::Link> boundInterface_ = {};
};

void Udp4::feedDatagram(smarter::shared_ptr<const Ip4Packet> packet, std::weak_ptr<nic::Link> link) {
	Udp udp{ .link = link };
	if (!udp.parse(std::move(packet))) {
		return;
	}

	auto i = binds.lower_bound({ 0, 0, udp.header.dst });
	for (; i != binds.end() && i->first.port == udp.header.dst; i++) {
		auto ep = i->first;
		if (ep.addr == udp.packet->header.destination || ep.addr == INADDR_ANY) {
			if (i->second->rejectPacket(udp))
				continue;

			if (!(i->second->shutdownReadSeq_)
					&& i->second->rcvBudget_.tryCharge(udp.packet->data.size())) {
				if (logSockets)
					std::println("netserver: received udp datagram to port {}", udp.header.dst);
				udp.packet = Ip4Packet::retain(std::move(udp.packet));
				i->second->queue_.emplace(std::move(udp));
				i->second->_inSeq = ++i->second->_currentSeq;
				i->second->_statusBell.raise();
			}

			break;
		}
	}
}

bool Udp4::tryBind(smarter::shared_ptr<Udp4Socket> socket, Endpoint addr) {
	auto i = binds.lower_bound({addr.family, 0, addr.port});
	for (; i != binds.end() && i->first.port == addr.port; i++) {
		auto ep = i->first;
		if (ep.addr == INADDR_ANY || addr.addr == INADDR_ANY
			|| ep.addr == addr.addr) {
			return false;
		}
	}
	socket->local_ = addr;
	binds.emplace(addr, std::move(socket));
	return true;
}

bool Udp4::unbind(Endpoint e) {
	return binds.erase(e) != 0;
}

static async::result<void> serveLanes(
	helix::UniqueLane ctrlLane,
	helix::UniqueLane ptLane,
	smarter::shared_ptr<Udp4Socket> sock
) {
	co_await async::race_and_cancel(
		[&](async::cancellation_token) {
			return protocols::fs::serveFile(std::move(ctrlLane),
					sock.get(), &Udp4Socket::ops);
		},
		[&](async::cancellation_token ct) {
			return protocols::fs::servePassthrough(std::move(ptLane),
					sock, &Udp4Socket::ops, ct);
		}
	);
	sock->handleClose();
}

void Udp4::serveSocket(int flags, helix::UniqueLane ctrlLane, helix::UniqueLane ptLane) {
	auto sock = Udp4Socket::make_socket(this, flags & SOCK_NONBLOCK);
	async::detach(serveLanes(std::move(ctrlLane), std::move(ptLane), std::move(sock)));
}
