#include <async/algorithm.hpp>
#include <async/basic.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <arch/bit.hpp>
#include <arch/variable.hpp>
#include <protocols/fs/server.hpp>
#include <cstring>
#include <format>
#include <iomanip>
#include <print>
#include <random>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>

#include "checksum.hpp"
#include "ip4.hpp"
#include "tcp4.hpp"

namespace {

constexpr bool debugTcp = false;
constexpr bool logDiscards = false;

struct stl_allocator {
	void *allocate(size_t size) {
		return operator new(size);
	}

	void deallocate(void *p, size_t) {
		return operator delete(p);
	}
};

struct PseudoHeader {
	arch::scalar_storage<uint32_t, arch::big_endian> src;
	arch::scalar_storage<uint32_t, arch::big_endian> dst;
	uint8_t zero = 0;
	uint8_t proto = static_cast<uint8_t>(IpProto::tcp);
	arch::scalar_storage<uint16_t, arch::big_endian> len;
};

static_assert(sizeof(PseudoHeader) == 12);

struct RingBuffer {
	RingBuffer(int shift)
	: storage_{reinterpret_cast<char *>(operator new (1 << shift))}, shift_{shift} { }

	RingBuffer(const RingBuffer &) = delete;

	~RingBuffer() {
		operator delete(storage_);
	}

	RingBuffer &operator= (const RingBuffer &) = delete;

	size_t spaceForEnqueue() {
		return (size_t{1} << shift_) - (enqPtr_ - deqPtr_);
	}

	size_t availableToDequeue() {
		return enqPtr_ - deqPtr_;
	}

	void enqueue(void *data, size_t size) {
		assert(size <= spaceForEnqueue());
		size_t ringSize = size_t{1} << shift_;
		auto wrappedPtr = enqPtr_ & (ringSize - 1);
		auto p = reinterpret_cast<char *>(data);
		size_t bytesUntilEnd = std::min(size, ringSize - wrappedPtr);
		memcpy(storage_ + wrappedPtr, p, bytesUntilEnd);
		memcpy(storage_, p + bytesUntilEnd, size - bytesUntilEnd);
		enqPtr_ += size;
	}

	void dequeue(void *data, size_t size) {
		dequeueLookahead(0, data, size);
		dequeueAdvance(size);
	}

	void dequeueLookahead(size_t offset, void *data, size_t size) {
		assert(offset + size <= availableToDequeue());
		size_t ringSize = size_t{1} << shift_;
		auto wrappedPtr = (deqPtr_ + offset) & (ringSize - 1);
		auto p = reinterpret_cast<char *>(data);
		size_t bytesUntilEnd = std::min(size, ringSize - wrappedPtr);
		memcpy(p, storage_ + wrappedPtr, bytesUntilEnd);
		memcpy(p + bytesUntilEnd, storage_, size - bytesUntilEnd);
	}

	void dequeueAdvance(size_t size) {
		deqPtr_ += size;
	}

private:
	char *storage_;
	int shift_;
	uint64_t enqPtr_ = 0;
	uint64_t deqPtr_ = 0;
};

// TODO: Use a CSPRNG, see also UDP.
static std::mt19937 globalPrng;

} // namespace

struct TcpHeader {
	static constexpr arch::field<uint16_t, bool> finFlag{0, 1};
	static constexpr arch::field<uint16_t, bool> synFlag{1, 1};
	static constexpr arch::field<uint16_t, bool> ackFlag{4, 1};
	static constexpr arch::field<uint16_t, unsigned int> headerWords{12, 4};

	arch::scalar_storage<uint16_t, arch::big_endian> srcPort;
	arch::scalar_storage<uint16_t, arch::big_endian> destPort;
	arch::scalar_storage<uint32_t, arch::big_endian> seqNumber;
	arch::scalar_storage<uint32_t, arch::big_endian> ackNumber;
	arch::bit_storage<uint16_t, arch::big_endian> flags;
	arch::scalar_storage<uint16_t, arch::big_endian> window;
	arch::scalar_storage<uint16_t, arch::big_endian> checksum;
	arch::scalar_storage<uint16_t, arch::big_endian> urgentPointer;
};

static_assert(sizeof(TcpHeader) == 20);

struct TcpPacket {
	arch::dma_buffer_view payload() {
		auto words = header.flags.load() & TcpHeader::headerWords;
		return packet->payload().subview(words * 4);
	}

	bool parse(smarter::shared_ptr<const Ip4Packet> packet) {
		auto ipPayload = packet->payload();
		if (ipPayload.size() < sizeof(TcpHeader)) {
			if (logDiscards)
				std::println("netserver: Discarding TCP packet smaller than the header");
			return false;
		}

		std::memcpy(&header, ipPayload.data(), sizeof(TcpHeader));
		auto words = header.flags.load() & TcpHeader::headerWords;
		if (words * 4 < sizeof(TcpHeader)) {
			if (logDiscards)
				std::println("netserver: Discarding TCP packet with a data offset below 5");
			return false;
		}
		if (ipPayload.size() < words * 4) {
			if (logDiscards)
				std::println("netserver: Discarding TCP packet smaller than its data offset");
			return false;
		}

		// Unlike UDP, TCP always carries a checksum.
		PseudoHeader pseudo {
			.src = packet->header.source,
			.dst = packet->header.destination,
			.proto = packet->header.protocol,
			.len = ipPayload.size()
		};
		Checksum csum;
		csum.update(&pseudo, sizeof(pseudo));
		csum.update(ipPayload);
		auto result = csum.finalize();
		if (result && ~result) {
			if (logDiscards)
				std::println("netserver: Discarding TCP packet with invalid checksum");
			return false;
		}

		this->packet = std::move(packet);
		return true;
	}

	TcpHeader header;
	smarter::shared_ptr<const Ip4Packet> packet;
};

namespace {

protocols::fs::Error checkAddress(const void *addrPtr, size_t addrLength, TcpEndpoint &e) {
	struct sockaddr_in sa;
	if (addrLength < sizeof(sa))
		return protocols::fs::Error::illegalArguments;

	std::memcpy(&sa, addrPtr, sizeof(sa));
	if (sa.sin_family != AF_INET)
		return protocols::fs::Error::afNotSupported;

	e.port = arch::from_endian<arch::big_endian, uint16_t>(sa.sin_port);
	e.ipAddress = arch::from_endian<arch::big_endian, uint32_t>(sa.sin_addr.s_addr);
	return protocols::fs::Error::none;
}

} // anonymous namespace

static async::result<void> serveLanes(
	helix::UniqueLane ctrlLane,
	helix::UniqueLane ptLane,
	smarter::shared_ptr<Tcp4Socket> sock);

struct Tcp4Socket {
	Tcp4Socket(Tcp4 *parent, bool nonBlock)
	: parent_(parent), nonBlock_{nonBlock}, recvRing_{14}, sendRing_{14} {}

	~Tcp4Socket() {
		parent_->unbind(localEp_);
	}

	async::result<void> disconnect() {
		if(localClosed_)
			co_return;

		if (connectState_ != ConnectState::connected) {
			// TODO: This will require some more effort in the future.
			std::println("netserver: TCP disconnect in non-connected state");
			co_return;
		}

		localClosed_ = true;

		while (localSettledSn_ < localFlushedSn_)
			co_await settleEvent_.async_wait();

		connectState_ = ConnectState::sendFin;
		flushEvent_.raise();

		// TODO: Wait for disconnect to finish?
		while (localSettledSn_ < localFlushedSn_)
			co_await settleEvent_.async_wait();
		std::println("netserver: TCP FIN was acknowledged");
	}

	static auto makeSocket(Tcp4 *parent, bool nonBlock) {
		auto s = smarter::make_shared<Tcp4Socket>(parent, nonBlock);
		s->holder_ = s;
		async::detach(s->flushOutPackets_());
		return s;
	}

	static async::result<protocols::fs::Error> bind(void *object,
			helix_ng::CredentialsView creds,
			const void *addrPtr, size_t addrLength) {
		(void) creds;

		auto self = static_cast<Tcp4Socket *>(object);

		if (self->localEp_.port)
			co_return protocols::fs::Error::illegalArguments;

		// Validate the endpoint.
		TcpEndpoint bindEp;
		if (auto e = checkAddress(addrPtr, addrLength, bindEp); e != protocols::fs::Error::none)
			co_return e;

		if (bindEp.ipAddress == INADDR_BROADCAST) {
			std::cout << "netserver: TCP cannot broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		if (bindEp.ipAddress != INADDR_ANY && !ip4().hasIp(bindEp.ipAddress)) {
			std::cout << "netserver: IP address " << std::setw(8) << std::hex << bindEp.ipAddress << std::dec << " is not available" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		// Bind the socket.
		if (!bindEp.port) {
			if (!self->bindAvailable(bindEp.ipAddress, true)) {
				std::cout << "netserver: No source port" << std::endl;
				co_return protocols::fs::Error::addressInUse;
			}
		} else if (!self->parent_->tryBind(self->holder_.lock(), true, bindEp)) {
			co_return protocols::fs::Error::addressInUse;
		}

		co_return protocols::fs::Error::none;
	}

	static async::result<size_t> sockname(void *object, void *addr_ptr, size_t max_addr_length) {
		auto self = static_cast<Tcp4Socket *>(object);
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_port = htons(self->localEp_.port);
		sa.sin_addr.s_addr = htonl(self->localEp_.ipAddress);
		memcpy(addr_ptr, &sa, std::min(sizeof(sockaddr_in), max_addr_length));

		co_return sizeof(sockaddr_in);
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> peername(void *object, void *addr_ptr, size_t max_addr_length) {
		auto self = static_cast<Tcp4Socket *>(object);
		if(self->connectState_ != ConnectState::connected
				&& self->connectState_ != ConnectState::sendFin
				&& self->connectState_ != ConnectState::finAcked) {
			co_return protocols::fs::Error::notConnected;
		}
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_port = htons(self->remoteEp_.port);
		sa.sin_addr.s_addr = htonl(self->remoteEp_.ipAddress);
		memcpy(addr_ptr, &sa, std::min(sizeof(sockaddr_in), max_addr_length));

		co_return sizeof(sockaddr_in);
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator() (managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, Tcp4Socket *self) {
			managarm::fs::GenericIoctlReply resp;

			switch(req.command()) {
				case FIONREAD: {
					resp.set_error(managarm::fs::Errors::SUCCESS);

					if(self->connectState_ != ConnectState::connected
							&& self->connectState_ != ConnectState::sendFin
							&& self->connectState_ != ConnectState::finAcked) {
						resp.set_error(managarm::fs::Errors::NOT_CONNECTED);
					}else {
						resp.set_fionread_count(self->recvRing_.availableToDequeue());
					}
					break;
				}
				default: {
					std::cout << "Invalid ioctl for tcp-socket" << std::endl;
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					break;
				}
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());

			co_return {};
		}
	};

	static async::result<void> ioctl(void *object, uint32_t, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) {
		auto self = static_cast<Tcp4Socket *>(object);

		auto res = co_await dispatchRequest<
			managarm::fs::GenericIoctlRequest
		>(conversation, std::move(msg), HandleIoctl{}, self);

		if (!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

	static async::result<protocols::fs::Error> connect(void *object,
			helix_ng::CredentialsView creds,
			const void *addrPtr, size_t addrLength) {
		(void) creds;

		auto self = static_cast<Tcp4Socket *>(object);

		if (self->connectState_ != ConnectState::none)
			co_return protocols::fs::Error::illegalArguments;

		// Validate the endpoint.
		TcpEndpoint connectEp;
		if (auto e = checkAddress(addrPtr, addrLength, connectEp); e != protocols::fs::Error::none)
			co_return e;

		if (connectEp.ipAddress == INADDR_BROADCAST) {
			std::cout << "netserver: TCP cannot broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		// Bind the socket if necessary.
		if (!self->localEp_.port && !self->bindAvailable(INADDR_ANY, true)) {
			std::cout << "netserver: No source port" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		// Connect to the remote.
		self->connectState_ = ConnectState::sendSyn;
		self->remoteEp_ = connectEp;
		self->flushEvent_.raise();

		while(true) {
			if(self->connectState_ != ConnectState::sendSyn)
				break;
			co_await self->settleEvent_.async_wait();
		}
		co_return protocols::fs::Error::none;
	}

	static async::result<protocols::fs::Error> listen(void *object) {
		auto self = static_cast<Tcp4Socket *>(object);
		self->listening_ = true;
		// DEBUG(sshd)
		std::cout << std::format("netserver: DEBUG(sshd) listen {} on {:#x}:{}", (void *)self,
				self->localEp_.ipAddress, self->localEp_.port) << std::endl;

		co_return protocols::fs::Error::none;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::AcceptResult>> accept(void *object) {
		auto self = static_cast<Tcp4Socket *>(object);

		if(self->pendingConnections_.empty()) {
			if(self->nonBlock_)
				co_return protocols::fs::Error::wouldBlock;

			while(self->pendingConnections_.empty()) {
				co_await self->pollEvent_.async_wait();
			}
		}

		auto sock = self->pendingConnections_.front();
		self->pendingConnections_.erase(self->pendingConnections_.begin());
		// DEBUG(sshd)
		std::cout << std::format("netserver: DEBUG(sshd) accept on {} -> {}", (void *)self,
				(void *)sock.get()) << std::endl;

		auto [localCtrl, remoteCtrl] = helix::createStream();
		auto [localPt, remotePt] = helix::createStream();

		async::detach(
			serveLanes(std::move(localCtrl), std::move(localPt), std::move(sock))
		);

		co_return std::make_pair(std::move(remoteCtrl), std::move(remotePt));
	}

	static async::result<protocols::fs::ReadResult> read(void *object, helix_ng::CredentialsView creds,
			void *data, size_t size, async::cancellation_token) {
		auto result = co_await recvMsg(object, creds, 0, data, size, nullptr, 0, {});
		if(auto e = std::get_if<protocols::fs::Error>(&result); e)
			co_return std::unexpected{*e};
		co_return std::get<protocols::fs::RecvData>(result).dataLength;
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> write(void *object, helix_ng::CredentialsView creds,
			const void *data, size_t size) {
		co_return co_await sendMsg(object, creds, 0, const_cast<void *>(data), size, nullptr, 0, {}, {});
	}

	static async::result<protocols::fs::RecvResult> recvMsg(void *object,
			helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t size,
			void *addrPtr, size_t addrLength, size_t max_ctrl_len) {
		(void) creds;
		(void) max_ctrl_len;

		auto self = static_cast<Tcp4Socket *>(object);
		auto p = reinterpret_cast<char *>(data);

		if(self->remoteClosed_)
			co_return protocols::fs::RecvData{{}, 0, sizeof(struct sockaddr_in), 0};

		if(flags & ~MSG_PEEK)
			std::cout << "\e[31m" "netserver/tcp: Encountered unexpected recvMsg() flags: "
					<< flags << "\e[39m" << std::endl;

		size_t progress = 0;
		while(progress < size) {
			size_t available = self->recvRing_.availableToDequeue();
			if(!available) {
				if(progress || self->remoteClosed_)
					break;
				if(self->nonBlock_ || flags & MSG_DONTWAIT)
					co_return protocols::fs::Error::wouldBlock;
				co_await self->inEvent_.async_wait();
				continue;
			}
			size_t chunk = std::min(available, size - progress);
			self->recvRing_.dequeueLookahead(0, p + progress, chunk);
			progress += chunk;
			if(flags & MSG_PEEK)
				break;
			self->recvRing_.dequeueAdvance(chunk);
			self->flushEvent_.raise();
		}

		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(struct sockaddr_in));
		sa.sin_port = arch::to_endian<arch::big_endian, uint16_t>(self->remoteEp_.port);
		sa.sin_addr.s_addr = arch::to_endian<arch::big_endian, uint32_t>(self->remoteEp_.ipAddress);
		memcpy(addrPtr, &sa, std::min(sizeof(struct sockaddr_in), addrLength));

		co_return protocols::fs::RecvData{{}, progress, sizeof(struct sockaddr_in), 0};
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendMsg(void *object,
			helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t size,
			void *addrPtr, size_t addrSize,
			std::vector<uint32_t> fds, struct ucred) {
		(void) creds;
		(void) flags;
		(void) addrPtr;
		(void) addrSize;
		(void) fds;

		auto self = static_cast<Tcp4Socket *>(object);
		auto p = reinterpret_cast<char *>(data);

		if(self->localClosed_)
			co_return protocols::fs::Error::brokenPipe;

		size_t progress = 0;
		while(progress < size) {
			size_t space = self->sendRing_.spaceForEnqueue();
			if(!space) {
				if(self->nonBlock_) {
					if(progress)
						break;
					co_return protocols::fs::Error::wouldBlock;
				}
				co_await self->settleEvent_.async_wait();
				continue;
			}
			size_t chunk = std::min(space, size - progress);
			self->sendRing_.enqueue(p + progress, chunk);
			self->flushEvent_.raise();
			progress += chunk;
		}

		co_return progress;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t pastSeq, int mask, async::cancellation_token cancellation) {
		auto self = static_cast<Tcp4Socket *>(object);
		int edges = 0;

		// TODO: Return an error in this case.
		if(pastSeq > self->currentSeq_) {
			std::cout << "netserver: Illegal pastSeq in TCP poll()" << std::endl;
			pastSeq = self->currentSeq_;
		}

		while(true) {
			edges = 0;
			if(self->inSeq_ > pastSeq || self->listenSeq_ > pastSeq)
				edges |= EPOLLIN;
			if(self->outSeq_ > pastSeq)
				edges |= EPOLLOUT;
			if(self->hupSeq_ > pastSeq)
				edges |= EPOLLHUP;

			if (edges & mask)
				break;

			if (!co_await self->pollEvent_.async_wait(cancellation))
				break;
		}

		co_return protocols::fs::PollWaitResult{self->currentSeq_, edges & mask};
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object) {
		auto self = static_cast<Tcp4Socket *>(object);

		int active = 0;
		if(self->recvRing_.availableToDequeue() || !self->pendingConnections_.empty())
			active |= EPOLLIN;
		if(self->sendRing_.spaceForEnqueue())
			active |= EPOLLOUT;
		if(self->remoteClosed_)
			active |= EPOLLHUP;

		co_return protocols::fs::PollStatusResult{self->currentSeq_, active};
	}

	static async::result<void> setFileFlags(void *object, int flags) {
		auto self = static_cast<Tcp4Socket *>(object);
		std::cout << "posix: setFileFlags on tcp socket only supports O_NONBLOCK" << std::endl;
		if(flags & ~O_NONBLOCK) {
			std::cout << "posix: setFileFlags on tcp socket called with unknown flags" << std::endl;
			co_return;
		}
		if(flags & O_NONBLOCK)
			self->nonBlock_ = true;
		else
			self->nonBlock_ = false;
		co_return;
	}

	static async::result<int> getFileFlags(void *object) {
		auto self = static_cast<Tcp4Socket *>(object);
		if(self->nonBlock_)
			co_return O_NONBLOCK;
		co_return 0;
	}

	static async::result<frg::expected<protocols::fs::Error>> setSocketOption(void *object,
		int layer, int number, std::vector<char> optbuf) {
		auto self = static_cast<Tcp4Socket *>(object);

		if(layer == SOL_SOCKET && number == SO_BINDTODEVICE) {
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
		}

		std::cout << std::format("netserver: unhandled TCP socket setsockopt layer {} number {}\n",
			layer, number);

		co_return protocols::fs::Error::invalidProtocolOption;
	}

	static async::result<frg::expected<protocols::fs::Error>> getSocketOption(void *object,
	helix_ng::CredentialsView, int layer, int number, std::vector<char> &optbuf) {
		auto self = static_cast<Tcp4Socket *>(object);

		if(layer == SOL_SOCKET && number == SO_TYPE) {
			auto type_ = SOCK_STREAM;
			optbuf.resize(std::min(optbuf.size(), sizeof(type_)));
			memcpy(optbuf.data(), &type_, optbuf.size());
		} else if(layer == SOL_SOCKET && number == SO_BINDTODEVICE) {
			size_t size = self->boundInterface_ ? self->boundInterface_->name().size() : 0;
			optbuf.resize(size);
			if (size)
				memcpy(optbuf.data(), self->boundInterface_->name().data(), size);
		} else {
			std::cout << std::format("netserver: unhandled TCP socket getsockopt layer {} number {}\n",
				layer, number);
			co_return protocols::fs::Error::invalidProtocolOption;
		}

		co_return {};
	}

	constexpr static protocols::fs::FileOperations ops {
		.read = &read,
		.write = &write,
		.ioctl = &ioctl,
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.bind = &bind,
		.listen = &listen,
		.connect = &connect,
		.accept = &accept,
		.sockname = &sockname,
		.getFileFlags = &getFileFlags,
		.setFileFlags = &setFileFlags,
		.recvMsg = &recvMsg,
		.sendMsg = &sendMsg,
		.peername = &peername,
		.setSocketOption = &setSocketOption,
		.getSocketOption = &getSocketOption,
	};

	bool bindAvailable(uint32_t ipAddress, bool unique) {
		static std::uniform_int_distribution<uint16_t> dist {
			32768, 60999
		};
		auto number = dist(globalPrng);
		auto range = dist.b() - dist.a();
		auto self = holder_.lock();
		for (int i = 0; i < range; i++) {
			uint16_t port = dist.a() + ((number + i) % range);
			if (parent_->tryBind(self, unique, { ipAddress, port }))
				return true;
		}
		return false;
	}

private:
	async::result<void> flushOutPackets_();

	void handleInPacket_(TcpPacket packet);

private:
	friend struct Tcp4;

	enum class ConnectState {
		none,
		sendSyn, // Client-side only.
		sendSynAck, // Server-side only.
		connected,
		sendFin,
		finAcked,
	};

	struct PendingConnection {
		uint32_t localIp;
		uint32_t remoteIp;
		uint16_t remotePort;
		uint32_t sequence;
	};

	async::result<void> handleIncomingConnection(PendingConnection c);

	// All segments of a connection must carry the same source address, even if the route's
	// preferred source changes (e.g., when DHCP adds a second address to the link).
	void pinSource_(Ip4TargetInfo &targetInfo) {
		if(!sourceIp_)
			sourceIp_ = localEp_.ipAddress != INADDR_ANY ? localEp_.ipAddress : targetInfo.source;
		targetInfo.source = sourceIp_;
	}

	Tcp4 *parent_;
	bool nonBlock_;
	TcpEndpoint remoteEp_;
	TcpEndpoint localEp_;
	uint32_t sourceIp_ = 0;
	smarter::weak_ptr<Tcp4Socket> holder_;
	std::vector<smarter::shared_ptr<Tcp4Socket>> pendingConnections_;

	ConnectState connectState_ = ConnectState::none;
	bool remoteClosed_ = false;
	bool localClosed_ = false;
	bool listening_ = false;

	// Out-SN corresponding to the front of sendRing_.
	uint32_t localSettledSn_ = 0;
	// Out-SN that has already been flushed to the IP layer (>= localSettledSn_).
	uint32_t localFlushedSn_ = 0;
	// Out-SN of the end of the remote window (>= localSettledSn_).
	uint32_t localWindowSn_ = 0;
	// In-SN that we already acknowledged.
	uint32_t remoteAckedSn_ = 0;
	// In-SN that we already received (>= remoteAckedSn_).
	uint32_t remoteKnownSn_ = 0;
	// Size of received window that we announced to the remote side.
	uint32_t announcedWindow_ = 0;

	RingBuffer recvRing_;
	RingBuffer sendRing_;

	async::recurring_event inEvent_;
	async::recurring_event flushEvent_;
	async::recurring_event settleEvent_;

	// The following sequence numbers are *not* TCP sequence numbers,
	// they implement the poll() function.
	uint64_t currentSeq_ = 1;
	uint64_t inSeq_ = 1;
	uint64_t outSeq_ = 0;
	uint64_t hupSeq_ = 1;
	uint64_t listenSeq_ = 1;
	async::recurring_event pollEvent_;

	std::shared_ptr<nic::Link> boundInterface_ = {};
};

async::result<void> Tcp4Socket::flushOutPackets_() {
	while(true) {
		if(connectState_ == ConnectState::none) {
			co_await flushEvent_.async_wait();
			continue;
		}

		if(connectState_ == ConnectState::sendSyn) {
			if(localSettledSn_ != localFlushedSn_) {
				co_await flushEvent_.async_wait();
				continue;
			}

			// Obtain a new random sequence number.
			auto randomSn = globalPrng();
			localSettledSn_ = randomSn;
			localFlushedSn_ = randomSn;

			// Construct and transmit the initial SYN packet.
			auto targetInfo = co_await ip4().targetByRemote(remoteEp_.ipAddress, boundInterface_);
			if (!targetInfo) {
				// TODO: Return an error to users.
				std::cout << "netserver: Destination unreachable" << std::endl;
				co_return;
			}
			pinSource_(*targetInfo);

			std::vector<char> buf;
			buf.resize(sizeof(TcpHeader));

			auto header = new (buf.data()) TcpHeader {
				.srcPort = localEp_.port,
				.destPort = remoteEp_.port,
				.seqNumber = localFlushedSn_,
				.ackNumber = 0,
				.flags = {},
				.window = 0,
				.checksum = 0,
				.urgentPointer = 0,
			};
			header->flags.store(TcpHeader::headerWords(sizeof(TcpHeader) / 4)
					| TcpHeader::synFlag(true));

			// Fill in the checksum.
			PseudoHeader pseudo {
				.src = targetInfo->source,
				.dst = remoteEp_.ipAddress,
				.len = buf.size()
			};
			Checksum csum;
			csum.update(&pseudo, sizeof(PseudoHeader));
			csum.update(buf.data(), buf.size());
			header->checksum = csum.finalize();

			++localFlushedSn_;

			if(debugTcp)
				std::cout << "netserver: Sending TCP SYN" << std::endl;
			auto error = co_await ip4().sendFrame(std::move(*targetInfo),
				buf.data(), buf.size(), static_cast<uint16_t>(IpProto::tcp));
			if (error != protocols::fs::Error::none) {
				// TODO: Return an error to users.
				std::cout << "netserver: Could not send TCP packet" << std::endl;
				co_return;
			}
		}else if(connectState_ == ConnectState::sendSynAck) {
			if(localSettledSn_ != localFlushedSn_) {
				co_await flushEvent_.async_wait();
				continue;
			}

			// Obtain a new random sequence number.
			auto randomSn = globalPrng();
			localSettledSn_ = randomSn;
			localFlushedSn_ = randomSn;

			// Construct and transmit the initial SYN-ACK packet.
			auto targetInfo = co_await ip4().targetByRemote(remoteEp_.ipAddress, boundInterface_);
			if (!targetInfo) {
				// TODO: Return an error to users.
				std::cout << "netserver: Destination unreachable" << std::endl;
				co_return;
			}
			pinSource_(*targetInfo);

			std::vector<char> buf;
			buf.resize(sizeof(TcpHeader));

			auto header = new (buf.data()) TcpHeader {
				.srcPort = localEp_.port,
				.destPort = remoteEp_.port,
				.seqNumber = localFlushedSn_,
				.ackNumber = remoteKnownSn_,
				.flags = {},
				.window = 0,
				.checksum = 0,
				.urgentPointer = 0,
			};
			header->flags.store(TcpHeader::headerWords(sizeof(TcpHeader) / 4)
					| TcpHeader::synFlag(true)
					| TcpHeader::ackFlag(true));

			// Fill in the checksum.
			PseudoHeader pseudo {
				.src = targetInfo->source,
				.dst = remoteEp_.ipAddress,
				.len = buf.size()
			};
			Checksum csum;
			csum.update(&pseudo, sizeof(PseudoHeader));
			csum.update(buf.data(), buf.size());
			header->checksum = csum.finalize();

			++localFlushedSn_;

			if(debugTcp)
				std::cout << "netserver: Sending TCP SYN-ACK" << std::endl;
			auto error = co_await ip4().sendFrame(std::move(*targetInfo),
				buf.data(), buf.size(), static_cast<uint16_t>(IpProto::tcp));
			if (error != protocols::fs::Error::none) {
				// TODO: Return an error to users.
				std::cout << "netserver: Could not send TCP packet" << std::endl;
				co_return;
			}
		}else{
			assert(connectState_ == ConnectState::connected
				|| connectState_ == ConnectState::sendFin
				|| connectState_ == ConnectState::finAcked);

			auto targetInfo = co_await ip4().targetByRemote(remoteEp_.ipAddress);
			if (!targetInfo) {
				// TODO: Return an error to users.
				std::cout << "netserver: Destination unreachable" << std::endl;
				co_return;
			}
			pinSource_(*targetInfo);

			size_t flushPointer = localFlushedSn_ - localSettledSn_;
			size_t windowPointer = localWindowSn_ - localSettledSn_;

			size_t chunk = 0; // Size of payload that we are going to send.
			if (connectState_ == ConnectState::connected) {
				size_t bytesAvailable = sendRing_.availableToDequeue();
				assert(bytesAvailable >= flushPointer);

				if (bytesAvailable > flushPointer && windowPointer > flushPointer) {
					chunk = std::min({
						bytesAvailable - flushPointer,
						windowPointer - flushPointer,
						size_t{1280} // TODO: Perform path MTU discovery.
					});
				}
			}

			bool sendFin = false;
			if (connectState_ == ConnectState::sendFin) {
				// If flushPointer != 0, we sent a FIN already.
				if (!flushPointer)
					sendFin = true;
			}

			// Check whether we need to send a packet.
			// TODO: Add retransmission here.
			bool wantAck = (remoteAckedSn_ != remoteKnownSn_);
			bool wantWindowUpdate = (announcedWindow_ < recvRing_.spaceForEnqueue());

			if(chunk == 0 && !sendFin && !wantAck && !wantWindowUpdate) {
				co_await flushEvent_.async_wait();
				continue;
			}

			// Construct and transmit the TCP packet.
			std::vector<char> buf;
			buf.resize(sizeof(TcpHeader) + chunk);

			auto header = new (buf.data()) TcpHeader {
				.srcPort = localEp_.port,
				.destPort = remoteEp_.port,
				.seqNumber = localFlushedSn_,
				.ackNumber = remoteKnownSn_,
				.flags = {},
				.window = std::min(recvRing_.spaceForEnqueue(), size_t{0xFFFF}),
				.checksum = 0,
				.urgentPointer = 0,
			};
			header->flags.store(TcpHeader::headerWords(sizeof(TcpHeader) / 4)
					| TcpHeader::ackFlag(true)
					| TcpHeader::finFlag(sendFin));

			if (chunk)
				sendRing_.dequeueLookahead(flushPointer, buf.data() + sizeof(TcpHeader), chunk);

			// Fill in the checksum.
			PseudoHeader pseudo {
				.src = targetInfo->source,
				.dst = remoteEp_.ipAddress,
				.len = buf.size()
			};
			Checksum csum;
			csum.update(&pseudo, sizeof(PseudoHeader));
			csum.update(buf.data(), buf.size());
			header->checksum = csum.finalize();

			localFlushedSn_ += chunk;
			if (sendFin)
				++localFlushedSn_;

			remoteAckedSn_ = remoteKnownSn_;
			announcedWindow_ = recvRing_.spaceForEnqueue();

			if(debugTcp)
				std::cout << "netserver: Sending TCP data (" << chunk << " bytes)" << std::endl;
			auto error = co_await ip4().sendFrame(std::move(*targetInfo),
				buf.data(), buf.size(),
				static_cast<uint16_t>(IpProto::tcp));
			if (error != protocols::fs::Error::none) {
				// TODO: Return an error to users.
				std::cout << "netserver: Could not send TCP packet" << std::endl;
				co_return;
			}
		}
	}
}

async::result<void> Tcp4Socket::handleIncomingConnection(PendingConnection c) {
	auto sock = Tcp4Socket::makeSocket(this->parent_, 0);

	sock->remoteEp_.ipAddress = c.remoteIp;
	sock->remoteEp_.port = c.remotePort;

	TcpEndpoint ep{
		.ipAddress = c.localIp,
		.port = this->localEp_.port
	};

	if(!this->parent_->tryBind(sock, false, ep)) {
		std::println("netserver: No source port in accept");
		co_return;
	}

	// Connect to the remote.
	sock->connectState_ = ConnectState::sendSynAck;
	sock->remoteAckedSn_ = c.sequence + 1;
	sock->remoteKnownSn_ = c.sequence + 1;

	sock->flushEvent_.raise();

	while(true) {
		if(sock->connectState_ != ConnectState::sendSynAck)
			break;
		co_await sock->settleEvent_.async_wait();
	}

	pendingConnections_.push_back(std::move(sock));
	listenSeq_ = ++currentSeq_;
	pollEvent_.raise();
	// DEBUG(sshd)
	std::cout << std::format("netserver: DEBUG(sshd) queued on listener {} (pending {}, listenSeq {})",
			(void *)this, pendingConnections_.size(), listenSeq_) << std::endl;

	co_return;
}

void Tcp4Socket::handleInPacket_(TcpPacket packet) {
	if(boundInterface_ && boundInterface_->index() != packet.packet->link.lock()->index())
		return;

	if(listening_) {
		if(packet.header.flags.load() & TcpHeader::synFlag) {
			auto ip = packet.packet->header.source;
			auto localIp = packet.packet->header.destination;
			auto port = packet.header.srcPort.load();

			for(auto &pending : pendingConnections_) {
				if(pending->remoteEp_.ipAddress == ip && pending->remoteEp_.port == port) {
					std::cout << "netserver: Rejecting duplicate SYN packet on listening socket"
							<< std::endl;
					return;
				}
			}

			// DEBUG(sshd)
			std::cout << std::format("netserver: DEBUG(sshd) SYN on listener {} for {:#x}:{}",
					(void *)this, localIp, this->localEp_.port) << std::endl;
			async::detach(handleIncomingConnection({
				.localIp = localIp,
				.remoteIp = ip,
				.remotePort = port,
				.sequence = packet.header.seqNumber.load()
			}));
		}else {
			std::cout << "netserver: Rejecting packet on listening socket"
					<< std::endl;
		}

		return;
	}

	if(connectState_ == ConnectState::sendSyn) {
		if(localSettledSn_ == localFlushedSn_) {
			std::cout << "netserver: Rejecting packet before SYN is sent [sendSyn]"
					<< std::endl;
			return;
		}

		if(!(packet.header.flags.load() & TcpHeader::synFlag)) {
			std::cout << "netserver: Rejecting packet without SYN [sendSyn]"
					<< std::endl;
			return;
		}else if(!(packet.header.flags.load() & TcpHeader::ackFlag)) {
			std::cout << "netserver: Rejecting SYN packet without ACK [sendSyn]"
					<< std::endl;
			return;
		}

		if(packet.header.ackNumber.load() != localSettledSn_ + 1) {
			std::cout << "netserver: Rejecting packet with bad ack-number [sendSyn]"
					<< std::endl;
			return;
		}

		++localSettledSn_;
		localWindowSn_ = localSettledSn_ + packet.header.window.load();
		remoteAckedSn_ = packet.header.seqNumber.load();
		remoteKnownSn_ = packet.header.seqNumber.load() + 1; // SYN counts as one byte.
		connectState_ = ConnectState::connected;
		flushEvent_.raise();
		settleEvent_.raise();
	}else if(connectState_ == ConnectState::sendSynAck) {
		if(localSettledSn_ == localFlushedSn_) {
			std::cout << "netserver: Rejecting packet before SYN-ACK is sent [sendSynAck]"
					<< std::endl;
			return;
		}

		if(!(packet.header.flags.load() & TcpHeader::ackFlag)) {
			std::cout << "netserver: Rejecting packet without ACK [sendSynAck]"
					<< std::endl;
			return;
		}

		if(packet.header.ackNumber.load() != localSettledSn_ + 1) {
			std::cout << "netserver: Rejecting packet with bad ack-number [sendSynAck]"
					<< std::endl;
			return;
		}

		if(packet.header.seqNumber.load() != remoteKnownSn_) {
			std::cout << "netserver: Rejecting packet with bad sequence [sendSynAck]"
					<< std::endl;
			return;
		}

		++localSettledSn_;
		localWindowSn_ = localSettledSn_ + packet.header.window.load();
		connectState_ = ConnectState::connected;
		flushEvent_.raise();
		settleEvent_.raise();
	}else if(connectState_ == ConnectState::connected
			|| connectState_ == ConnectState::sendFin
			|| connectState_ == ConnectState::finAcked) {
		if(packet.header.seqNumber.load() == remoteKnownSn_) {
			bool gotUpdate = false;

			auto payload = packet.payload();
			size_t chunk = std::min(payload.size(), recvRing_.spaceForEnqueue());
			if(chunk) {
				recvRing_.enqueue(payload.data(), chunk);
				remoteKnownSn_ += chunk;
				if(announcedWindow_ < chunk) {
					announcedWindow_ = 0;
				}else{
					announcedWindow_ -= chunk;
				}

				inSeq_ = ++currentSeq_;
				gotUpdate = true;
			}

			if(packet.header.flags.load() & TcpHeader::finFlag) {
				++remoteKnownSn_; // FIN counts as one byte.
				remoteClosed_ = true;

				hupSeq_ = ++currentSeq_;
				gotUpdate = true;
			}

			if(gotUpdate) {
				inEvent_.raise();
				flushEvent_.raise();
				pollEvent_.raise();
			}
		}

		if(packet.header.flags.load() & TcpHeader::ackFlag) {
			if (connectState_ == ConnectState::connected) {
				size_t validWindow = localFlushedSn_ - localSettledSn_;
				size_t ackPointer = packet.header.ackNumber.load() - localSettledSn_;
				if(ackPointer <= validWindow) {
					localSettledSn_ += ackPointer;
					localWindowSn_ = localSettledSn_ + packet.header.window.load();
					sendRing_.dequeueAdvance(ackPointer);
					outSeq_ = ++currentSeq_;
					settleEvent_.raise();
					pollEvent_.raise();
				}else{
					std::cout << "netserver: Rejecting ack-number outside of valid window"
							<< std::endl;
				}
			} else if (connectState_ == ConnectState::sendFin) {
				if(packet.header.ackNumber.load() == localSettledSn_ + 1) {
					connectState_ = ConnectState::finAcked;
					++localSettledSn_;
					localWindowSn_ = localSettledSn_ + packet.header.window.load();
					settleEvent_.raise();
				}else if(packet.header.ackNumber.load() != localSettledSn_) {
					std::cout << "netserver: Rejecting packet with bad ack-number [sendFin]"
							<< std::endl;
				}
			}
		}
	}
}

void Tcp4::feedDatagram(smarter::shared_ptr<const Ip4Packet> packet) {
	TcpPacket tcp;
	if (!tcp.parse(std::move(packet))) {
		return;
	}

	// RFC 1122 4.2.3.10: connections can only ever involve unicast addresses.
	if (ip4().classifyAddress(tcp.packet->header.destination) != Ip4::AddressType::unicast) {
		if (logDiscards)
			std::println("netserver: Discarding TCP segment addressed to a broadcast or multicast address");
		return;
	}

	if(debugTcp)
		std::cout << "netserver: Received TCP packet at port " << tcp.header.destPort.load()
				<< " (" << tcp.payload().size() << " bytes)" << std::endl;

	// Look for non-listening sockets.
	for (auto it = binds.lower_bound({ 0, tcp.header.destPort.load() });
			it != binds.end() && it->first.port == tcp.header.destPort.load(); it++) {
		auto existingEp = it->first;
		auto &sock = it->second;

		if (sock->listening_)
			continue;

		if (tcp.packet->header.source != sock->remoteEp_.ipAddress
				|| tcp.header.srcPort.load() != sock->remoteEp_.port)
			continue;

		if (existingEp.ipAddress == tcp.packet->header.destination
				|| existingEp.ipAddress == INADDR_ANY) {
			it->second->handleInPacket_(std::move(tcp));
			return;
		}
	}

	// Look for listening sockets (and do not care about their remote endpoints).
	for (auto it = binds.lower_bound({ 0, tcp.header.destPort.load() });
			it != binds.end() && it->first.port == tcp.header.destPort.load(); it++) {
		auto existingEp = it->first;
		auto &sock = it->second;

		if (!sock->listening_)
			continue;

		if (existingEp.ipAddress == tcp.packet->header.destination
				|| existingEp.ipAddress == INADDR_ANY) {
			it->second->handleInPacket_(std::move(tcp));
			return;
		}
	}
}

bool Tcp4::tryBind(smarter::shared_ptr<Tcp4Socket> socket, bool unique, TcpEndpoint wantedEp) {
	if (unique) {
		auto it = binds.lower_bound(wantedEp);
		for (; it != binds.end() && it->first.port == wantedEp.port; it++) {
			auto existingEp = it->first;
			if (existingEp.ipAddress == INADDR_ANY || wantedEp.ipAddress == INADDR_ANY
					|| existingEp.ipAddress == wantedEp.ipAddress) {
				return false;
			}
		}
	}
	// DEBUG(sshd)
	std::cout << std::format("netserver: DEBUG(sshd) bind {} to {:#x}:{} (unique {}, {} already)",
			(void *)socket.get(), wantedEp.ipAddress, wantedEp.port, unique, binds.count(wantedEp)) << std::endl;
	socket->localEp_ = wantedEp;
	binds.emplace(wantedEp, std::move(socket));
	return true;
}

bool Tcp4::unbind(TcpEndpoint e) {
	// DEBUG(sshd)
	std::cout << std::format("netserver: DEBUG(sshd) unbind {:#x}:{} ({} entries)", e.ipAddress, e.port,
			binds.count(e)) << std::endl;
	return binds.erase(e) != 0;
}

static async::result<void> serveLanes(
	helix::UniqueLane ctrlLane,
	helix::UniqueLane ptLane,
	smarter::shared_ptr<Tcp4Socket> sock
) {
	co_await async::race_and_cancel(
		[&](async::cancellation_token) {
			return protocols::fs::serveFile(std::move(ctrlLane),
					sock.get(), &Tcp4Socket::ops);
		},
		[&](async::cancellation_token ct) {
			return protocols::fs::servePassthrough(std::move(ptLane),
					sock, &Tcp4Socket::ops, ct);
		}
	);
	std::println("netserver: TCP socket closed");
	co_await sock->disconnect();
}

void Tcp4::serveSocket(int flags, helix::UniqueLane ctrlLane, helix::UniqueLane ptLane) {
	auto sock = Tcp4Socket::makeSocket(this, flags & SOCK_NONBLOCK);
	async::detach(serveLanes(std::move(ctrlLane), std::move(ptLane), std::move(sock)));
}
