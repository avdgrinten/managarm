#include <algorithm>
#include <expected>
#include <optional>
#include <print>
#include <utility>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <async/algorithm.hpp>
#include <frg/scope_exit.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/timer.hpp>

#include "device.hpp"
#include "socket.hpp"
#include "stack.hpp"

namespace vsock {

namespace {

// Default receive buffer size and bounds, same as in Linux.
constexpr uint32_t defaultBufferSize = 256 * 1024;
constexpr uint64_t defaultBufferMinSize = 128;
constexpr uint64_t defaultBufferMaxSize = 256 * 1024;

// Bounds the memory that a single socket can make the server allocate.
constexpr uint64_t maxBufferSize = 64 * 1024 * 1024;

// Peers stall once their view of our free buffer space is exhausted; update it before that.
constexpr uint32_t creditUpdateThreshold = 64 * 1024;

constexpr size_t maxAcceptBacklog = 128;

// Same timeouts as Linux.
constexpr uint64_t connectTimeout = 2'000'000'000;
constexpr uint64_t closeTimeout = 8'000'000'000;

constexpr uint32_t shutdownBoth = VIRTIO_VSOCK_SHUTDOWN_RCV | VIRTIO_VSOCK_SHUTDOWN_SEND;

std::expected<sockaddr_vm, protocols::fs::Error> parseAddress(const void *addrPtr,
		size_t addrLength) {
	sockaddr_vm sa;
	if(addrLength < sizeof(sockaddr_vm))
		return std::unexpected{protocols::fs::Error::illegalArguments};
	memcpy(&sa, addrPtr, sizeof(sockaddr_vm));
	if(sa.svm_family != AF_VSOCK)
		return std::unexpected{protocols::fs::Error::afNotSupported};
	return sa;
}

size_t writeAddress(void *addrPtr, size_t maxAddrLength, uint32_t cid, uint32_t port) {
	sockaddr_vm sa{};
	sa.svm_family = AF_VSOCK;
	sa.svm_cid = cid;
	sa.svm_port = port;
	if(addrPtr)
		memcpy(addrPtr, &sa, std::min(sizeof(sockaddr_vm), maxAddrLength));
	return sizeof(sockaddr_vm);
}

} // anonymous namespace

// --------------------------------------------------------
// ByteRing
// --------------------------------------------------------

void ByteRing::growCapacity(size_t capacity) {
	if(capacity <= capacity_)
		return;
	if(!buffer_.empty()) {
		std::vector<char> grown(capacity);
		peek(grown.data(), size_);
		buffer_ = std::move(grown);
		head_ = 0;
	}
	capacity_ = capacity;
}

void ByteRing::push(std::span<const char> data) {
	assert(data.size() <= space());
	if(data.empty())
		return;
	if(buffer_.empty())
		buffer_.resize(capacity_);
	auto tail = (head_ + size_) % buffer_.size();
	auto first = std::min(data.size(), buffer_.size() - tail);
	memcpy(buffer_.data() + tail, data.data(), first);
	memcpy(buffer_.data(), data.data() + first, data.size() - first);
	size_ += data.size();
}

size_t ByteRing::peek(char *out, size_t size) const {
	auto n = std::min(size, size_);
	if(!n)
		return 0;
	auto first = std::min(n, buffer_.size() - head_);
	memcpy(out, buffer_.data() + head_, first);
	memcpy(out + first, buffer_.data(), n - first);
	return n;
}

void ByteRing::discard(size_t size) {
	assert(size <= size_);
	if(!size)
		return;
	head_ = (head_ + size) % buffer_.size();
	size_ -= size;
}

// --------------------------------------------------------
// Socket
// --------------------------------------------------------

smarter::shared_ptr<Socket> Socket::create(VsockStack *stack, bool nonBlock) {
	auto socket = smarter::make_shared<Socket>(stack, nonBlock);
	socket->holder_ = socket;
	return socket;
}

Socket::Socket(VsockStack *stack, bool nonBlock)
: stack_{stack}, nonBlock_{nonBlock}, bufferSize_{defaultBufferSize},
		bufferMinSize_{defaultBufferMinSize}, bufferMaxSize_{defaultBufferMaxSize} {
	rxRing_.growCapacity(bufferSize_);
}

async::result<void> Socket::serve(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane,
		smarter::shared_ptr<Socket> self) {
	co_await async::race_and_cancel(
		[&] (async::cancellation_token) {
			return protocols::fs::serveFile(std::move(ctrlLane), self.get(), &ops);
		},
		[&] (async::cancellation_token ct) {
			return protocols::fs::servePassthrough(std::move(ptLane), self, &ops, ct);
		}
	);
	co_await self->close_();
}

void Socket::setBufferSize_(uint64_t size, TxBuffer &buffer) {
	// Same order as in Linux, i.e., the minimum wins if it exceeds the maximum.
	if(size > bufferMaxSize_)
		size = bufferMaxSize_;
	if(size < bufferMinSize_)
		size = bufferMinSize_;
	bufferSize_ = std::min(size, maxBufferSize);
	rxRing_.growCapacity(bufferSize_);
	if(state_ == State::connected)
		sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_CREDIT_UPDATE);
}

virtio_vsock_hdr Socket::makeHeader_(uint16_t op, uint32_t len) {
	virtio_vsock_hdr hdr{};
	hdr.src_cid = stack_->device()->guestCid();
	hdr.dst_cid = peerCid_;
	hdr.src_port = localPort_;
	hdr.dst_port = peerPort_;
	hdr.len = len;
	hdr.type = VIRTIO_VSOCK_TYPE_STREAM;
	hdr.op = op;
	hdr.buf_alloc = bufferSize_;
	hdr.fwd_cnt = fwdCnt_;
	lastFwdCntSent_ = fwdCnt_;
	return hdr;
}

void Socket::sendControl_(TxBuffer buffer, uint16_t op, uint32_t flags) {
	auto hdr = makeHeader_(op);
	hdr.flags = flags;
	stack_->device()->send(std::move(buffer), hdr);
}

bool Socket::creditUpdateDue_() {
	if(state_ != State::connected)
		return false;
	uint32_t unannounced = fwdCnt_ - lastFwdCntSent_;
	if(!unannounced)
		return false;
	// Free space from the peer's point of view.
	uint64_t used = rxRing_.size() + unannounced;
	uint64_t peerViewFree = used < bufferSize_ ? bufferSize_ - used : 0;
	return peerViewFree < creditUpdateThreshold;
}

async::result<void> Socket::sendCreditUpdate_() {
	co_await stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
		std::lock_guard lock{mutex_};
		if(!creditUpdateDue_())
			return true;
		if(!buffer)
			return false;
		sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_CREDIT_UPDATE);
		return true;
	});
	stack_->device()->flushTx();
}

uint32_t Socket::credit_() {
	uint32_t inFlight = txCnt_ - peerFwdCnt_;
	if(inFlight >= peerBufAlloc_)
		return 0;
	return peerBufAlloc_ - inFlight;
}

bool Socket::readable_() {
	return !rxRing_.empty() || !acceptQueue_.empty()
			|| state_ == State::disconnected
			|| (peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)
			|| (localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV);
}

bool Socket::writable_() {
	return state_ == State::connected && credit_()
			&& !(peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV)
			&& !(localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND);
}

int Socket::activeEvents_() {
	int events = 0;
	if(readable_())
		events |= EPOLLIN | EPOLLRDNORM;
	if(writable_())
		events |= EPOLLOUT | EPOLLWRNORM;
	if(state_ == State::disconnected || (peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)
			|| (localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV))
		events |= EPOLLRDHUP;
	// Same condition as in Linux.
	if(state_ == State::disconnected || localShutdown_ == shutdownBoth
			|| ((localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)
				&& (peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)))
		events |= EPOLLHUP;
	return events;
}

int Socket::edgesSince_(uint64_t pastSeq) {
	int edges = 0;
	if(inSeq_ > pastSeq)
		edges |= EPOLLIN | EPOLLRDNORM;
	if(outSeq_ > pastSeq)
		edges |= EPOLLOUT | EPOLLWRNORM;
	if(hupSeq_ > pastSeq)
		edges |= EPOLLHUP;
	if(rdhupSeq_ > pastSeq)
		edges |= EPOLLRDHUP;
	return edges;
}

void Socket::notify_(int edges) {
	// Only report edges of conditions that are actually active.
	edges &= activeEvents_();
	if(!edges)
		return;
	currentSeq_++;
	if(edges & EPOLLIN)
		inSeq_ = currentSeq_;
	if(edges & EPOLLOUT)
		outSeq_ = currentSeq_;
	if(edges & EPOLLHUP)
		hupSeq_ = currentSeq_;
	if(edges & EPOLLRDHUP)
		rdhupSeq_ = currentSeq_;
}

void Socket::forgetLocked_() {
	VsockStack::ConnectionKey key{localPort_, peerCid_, peerPort_};
	auto it = stack_->connections_.find(key);
	if(it != stack_->connections_.end() && it->second.get() == this)
		stack_->connections_.erase(it);
}

void Socket::handlePacket_(const virtio_vsock_hdr &hdr, std::span<const char> payload,
		TxBuffer &reply) {
	bool forget = false;
	{
		std::lock_guard lock{mutex_};

		peerBufAlloc_ = hdr.buf_alloc;
		peerFwdCnt_ = hdr.fwd_cnt;

		switch(state_) {
		case State::connecting:
			if(hdr.op == VIRTIO_VSOCK_OP_RESPONSE) {
				state_ = State::connected;
			}else{
				if(hdr.op != VIRTIO_VSOCK_OP_RST)
					sendControl_(std::move(reply), VIRTIO_VSOCK_OP_RST);
				state_ = State::unconnected;
				refused_ = true;
				forget = true;
			}
			break;
		case State::connected:
			switch(hdr.op) {
			case VIRTIO_VSOCK_OP_RW: {
				auto n = std::min(payload.size(), rxRing_.space());
				if(n < payload.size())
					std::println("vsock: Peer {}:{} exceeded its credit, dropping {} bytes",
							peerCid_, peerPort_, payload.size() - n);
				rxRing_.push(payload.subspan(0, n));
				break;
			}
			case VIRTIO_VSOCK_OP_CREDIT_UPDATE:
				break;
			case VIRTIO_VSOCK_OP_CREDIT_REQUEST:
				sendControl_(std::move(reply), VIRTIO_VSOCK_OP_CREDIT_UPDATE);
				break;
			case VIRTIO_VSOCK_OP_SHUTDOWN:
				peerShutdown_ |= hdr.flags & shutdownBoth;
				// Like Linux, complete the disconnect right away if nothing is left to read.
				if(peerShutdown_ == shutdownBoth && rxRing_.empty()) {
					sendControl_(std::move(reply), VIRTIO_VSOCK_OP_RST);
					state_ = State::disconnected;
					forget = true;
				}
				break;
			case VIRTIO_VSOCK_OP_RST:
				peerShutdown_ = shutdownBoth;
				state_ = State::disconnected;
				forget = true;
				break;
			default:
				std::println("vsock: Ignoring op {} on connection {}:{}",
						hdr.op, peerCid_, peerPort_);
			}
			break;
		case State::closing:
			if(hdr.op == VIRTIO_VSOCK_OP_RST) {
				state_ = State::disconnected;
				forget = true;
			}
			break;
		default:
			// Sockets in other states are not published.
			assert(!"unexpected state for published vsock connection");
		}

		if(forget)
			forgetLocked_();
		notify_(EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP);
	}
}

smarter::shared_ptr<Socket> Socket::handleRequest_(const virtio_vsock_hdr &hdr,
		TxBuffer &reply) {
	std::lock_guard lock{mutex_};

	if(state_ != State::listening || acceptQueue_.size() >= maxAcceptBacklog)
		return nullptr;

	auto child = Socket::create(stack_, false);
	{
		std::lock_guard childLock{child->mutex_};
		child->state_ = State::connected;
		child->localCid_ = hdr.dst_cid;
		child->localPort_ = hdr.dst_port;
		child->peerCid_ = hdr.src_cid;
		child->peerPort_ = hdr.src_port;
		child->peerBufAlloc_ = hdr.buf_alloc;
		child->peerFwdCnt_ = hdr.fwd_cnt;
		child->bufferMinSize_ = bufferMinSize_;
		child->bufferMaxSize_ = bufferMaxSize_;
		child->bufferSize_ = bufferSize_;
		child->rxRing_.growCapacity(bufferSize_);
		child->sendControl_(std::move(reply), VIRTIO_VSOCK_OP_RESPONSE);
	}
	stack_->connections_.insert({{hdr.dst_port, hdr.src_cid, hdr.src_port}, child});

	acceptQueue_.push_back(child);
	notify_(EPOLLIN);
	return child;
}

void Socket::handleReset_() {
	{
		std::lock_guard lock{mutex_};
		if(state_ == State::connecting) {
			state_ = State::unconnected;
			refused_ = true;
		}else if(state_ == State::connected || state_ == State::closing) {
			peerShutdown_ = shutdownBoth;
			state_ = State::disconnected;
		}
		notify_(EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP);
	}
	event_.raise();
}

async::result<void> Socket::close_() {
	std::deque<smarter::shared_ptr<Socket>> orphans;
	bool awaitRst = false;
	co_await stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
		std::lock_guard stackLock{stack_->mutex_};
		std::lock_guard lock{mutex_};

		if(!buffer && (state_ == State::connecting || state_ == State::connected))
			return false;

		switch(state_) {
		case State::unconnected:
			state_ = State::closed;
			break;
		case State::listening:
			stack_->listeners_.erase(localPort_);
			orphans = std::move(acceptQueue_);
			state_ = State::closed;
			break;
		case State::connecting:
			sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_RST);
			forgetLocked_();
			state_ = State::closed;
			refused_ = true;
			break;
		case State::connected:
			if(peerShutdown_ == shutdownBoth) {
				sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_RST);
				forgetLocked_();
				state_ = State::disconnected;
			}else{
				sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_SHUTDOWN, shutdownBoth);
				state_ = State::closing;
				awaitRst = true;
			}
			break;
		default:
			break;
		}
		notify_(EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP);
		return true;
	});
	stack_->device()->flushTx();
	// Requests that are still in flight must not wait for a state that the closed socket never reaches.
	event_.raise();

	// Connections that were never accepted are reset.
	for(auto &orphan : orphans) {
		co_await stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
			std::lock_guard stackLock{stack_->mutex_};
			std::lock_guard lock{orphan->mutex_};
			if(orphan->state_ == State::connected) {
				if(!buffer)
					return false;
				orphan->sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_RST);
				orphan->forgetLocked_();
				orphan->state_ = State::disconnected;
			}
			return true;
		});
		stack_->device()->flushTx();
	}

	if(awaitRst) {
		// A timeout is detected by re-checking the state below.
		(void)(co_await helix::withTimeout([&](async::cancellation_token ct) {
			return event_.async_wait_if([&] {
				std::lock_guard lock{mutex_};
				return state_ == State::closing;
			}, ct);
		}, helix::deadlineAfter(closeTimeout)));

		co_await stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
			std::lock_guard stackLock{stack_->mutex_};
			std::lock_guard lock{mutex_};
			if(state_ == State::closing) {
				if(!buffer)
					return false;
				sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_RST);
				forgetLocked_();
				state_ = State::disconnected;
			}
			notify_(EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP);
			return true;
		});
		stack_->device()->flushTx();
		event_.raise();
	}

	std::lock_guard stackLock{stack_->mutex_};
	std::lock_guard lock{mutex_};
	if(ownsPort_) {
		stack_->boundPorts_.erase(localPort_);
		ownsPort_ = false;
	}
}

async::result<std::expected<size_t, protocols::fs::Error>> Socket::receive_(uint32_t flags,
		void *data, size_t size, async::cancellation_token cancellation) {
	constexpr uint32_t supportedFlags = MSG_PEEK | MSG_DONTWAIT | MSG_WAITALL;
	if(flags & ~supportedFlags)
		std::println("vsock: Ignoring recvmsg() flags {:#x}", flags & ~supportedFlags);

	auto p = static_cast<char *>(data);
	size_t progress = 0;
	while(true) {
		std::optional<std::expected<size_t, protocols::fs::Error>> result;
		bool creditUpdateDue = false;
		{
			std::lock_guard lock{mutex_};
			bool eof = state_ == State::disconnected || state_ == State::closing
					|| (peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)
					|| (localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV);
			if(state_ != State::connected && !eof) {
				result = std::unexpected{protocols::fs::Error::notConnected};
			}else{
				auto n = rxRing_.peek(p + progress, size - progress);
				if(!(flags & MSG_PEEK)) {
					rxRing_.discard(n);
					fwdCnt_ += n;
					creditUpdateDue = creditUpdateDue_();
				}
				progress += n;

				bool done = progress == size
						|| (progress && (!(flags & MSG_WAITALL) || (flags & MSG_PEEK)));
				if(done || (rxRing_.empty() && eof)) {
					result = progress;
				}else if(nonBlock_ || (flags & MSG_DONTWAIT)) {
					if(progress)
						result = progress;
					else
						result = std::unexpected{protocols::fs::Error::wouldBlock};
				}
			}
		}
		if(creditUpdateDue)
			co_await sendCreditUpdate_();
		if(result)
			co_return *result;

		auto waited = co_await event_.async_wait_if([&] {
			std::lock_guard lock{mutex_};
			return rxRing_.empty() && state_ == State::connected
					&& !(peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)
					&& !(localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV);
		}, cancellation);
		if(!waited) {
			if(progress)
				co_return progress;
			co_return std::unexpected{protocols::fs::Error::interrupted};
		}
	}
}

async::result<std::expected<size_t, protocols::fs::Error>> Socket::send_(uint32_t flags,
		const void *data, size_t size) {
	if(flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
		std::println("vsock: Ignoring sendmsg() flags {:#x}", flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL));

	co_await writeMutex_.async_lock();
	frg::scope_exit unlockOnExit{[&] {
		writeMutex_.unlock();
	}};

	auto p = static_cast<const char *>(data);
	size_t progress = 0;
	TxBuffer buffer;
	while(true) {
		bool sent = false;
		bool needBuffer = false;
		{
			std::lock_guard lock{mutex_};
			if(state_ == State::disconnected || state_ == State::closing
					|| (peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV)
					|| (localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND)) {
				if(progress)
					break;
				co_return std::unexpected{protocols::fs::Error::brokenPipe};
			}
			if(state_ != State::connected)
				co_return std::unexpected{protocols::fs::Error::notConnected};
			if(progress == size)
				break;

			if(auto credit = credit_(); credit && !buffer) {
				needBuffer = true;
			}else if(credit) {
				auto chunk = std::min({size - progress, size_t{credit}, Device::maxTxPayload});
				memcpy(buffer.payload().data(), p + progress, chunk);
				stack_->device()->send(std::exchange(buffer, {}), makeHeader_(VIRTIO_VSOCK_OP_RW, chunk));
				txCnt_ += chunk;
				progress += chunk;
				sent = true;
			}else if(nonBlock_ || (flags & MSG_DONTWAIT)) {
				if(progress)
					break;
				co_return std::unexpected{protocols::fs::Error::wouldBlock};
			}
		}

		if(sent) {
			stack_->device()->flushTx();
			continue;
		}
		// Buffers only depend on the device (not on the peer), so non-blocking sockets wait too.
		if(needBuffer) {
			buffer = co_await stack_->device()->allocateTx(TxClass::data);
			continue;
		}

		// Do not keep other sockets from sending while we wait for the peer.
		buffer = {};
		co_await event_.async_wait_if([&] {
			std::lock_guard lock{mutex_};
			return state_ == State::connected && !credit_()
					&& !(peerShutdown_ & VIRTIO_VSOCK_SHUTDOWN_RCV)
					&& !(localShutdown_ & VIRTIO_VSOCK_SHUTDOWN_SEND);
		});
	}

	stack_->device()->flushTx();
	co_return progress;
}

async::result<protocols::fs::ReadResult> Socket::read(void *object,
		helix_ng::CredentialsView, void *data, size_t size,
		async::cancellation_token cancellation) {
	auto self = static_cast<Socket *>(object);
	co_return co_await self->receive_(0, data, size, cancellation);
}

async::result<frg::expected<protocols::fs::Error, size_t>> Socket::write(void *object,
		helix_ng::CredentialsView, const void *data, size_t size) {
	auto self = static_cast<Socket *>(object);
	auto result = co_await self->send_(0, data, size);
	if(!result)
		co_return result.error();
	co_return result.value();
}

async::result<protocols::fs::RecvResult> Socket::recvMsg(void *object,
		helix_ng::CredentialsView, uint32_t flags, void *data, size_t size,
		void *addrPtr, size_t addrLength, size_t) {
	auto self = static_cast<Socket *>(object);
	auto result = co_await self->receive_(flags, data, size, {});
	if(!result)
		co_return result.error();

	size_t writtenAddrLength;
	{
		std::lock_guard lock{self->mutex_};
		writtenAddrLength = writeAddress(addrPtr, addrLength, self->peerCid_, self->peerPort_);
	}
	co_return protocols::fs::RecvData{{}, result.value(), writtenAddrLength, 0};
}

async::result<frg::expected<protocols::fs::Error, size_t>> Socket::sendMsg(void *object,
		helix_ng::CredentialsView, uint32_t flags, void *data, size_t size,
		void *, size_t addrLength, std::vector<uint32_t> fds, struct ucred) {
	auto self = static_cast<Socket *>(object);
	if(!fds.empty())
		co_return protocols::fs::Error::illegalArguments;
	// Like Linux, reject addresses since stream sockets are connection-oriented.
	if(addrLength) {
		std::lock_guard lock{self->mutex_};
		if(self->state_ == State::connected)
			co_return protocols::fs::Error::alreadyConnected;
		co_return protocols::fs::Error::notSupported;
	}
	auto result = co_await self->send_(flags, data, size);
	if(!result)
		co_return result.error();
	co_return result.value();
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
Socket::pollWait(void *object, uint64_t pastSeq, int mask,
		async::cancellation_token cancellation) {
	auto self = static_cast<Socket *>(object);

	{
		std::lock_guard lock{self->mutex_};
		if(pastSeq > self->currentSeq_)
			co_return protocols::fs::Error::illegalArguments;
	}

	while(true) {
		auto waited = co_await self->event_.async_wait_if([&] {
			std::lock_guard lock{self->mutex_};
			return !(self->edgesSince_(pastSeq) & mask);
		}, cancellation);

		std::lock_guard lock{self->mutex_};
		auto edges = self->edgesSince_(pastSeq) & mask;
		if(edges || !waited)
			co_return protocols::fs::PollWaitResult{self->currentSeq_, edges};
	}
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
Socket::pollStatus(void *object) {
	auto self = static_cast<Socket *>(object);
	std::lock_guard lock{self->mutex_};
	co_return protocols::fs::PollStatusResult{self->currentSeq_, self->activeEvents_()};
}

async::result<protocols::fs::Error> Socket::bind(void *object,
		helix_ng::CredentialsView, const void *addrPtr, size_t addrLength) {
	auto self = static_cast<Socket *>(object);

	auto sa = parseAddress(addrPtr, addrLength);
	if(!sa)
		co_return sa.error();

	std::lock_guard stackLock{self->stack_->mutex_};
	std::lock_guard lock{self->mutex_};

	if(self->state_ != State::unconnected || self->localPort_ != VMADDR_PORT_ANY)
		co_return protocols::fs::Error::illegalArguments;
	if(sa->svm_cid != VMADDR_CID_ANY && sa->svm_cid != self->stack_->device()->guestCid())
		co_return protocols::fs::Error::addressNotAvailable;

	uint32_t port;
	if(sa->svm_port == VMADDR_PORT_ANY) {
		auto allocated = self->stack_->allocatePort_();
		if(!allocated)
			co_return protocols::fs::Error::addressInUse;
		port = *allocated;
	}else{
		if(self->stack_->boundPorts_.contains(sa->svm_port))
			co_return protocols::fs::Error::addressInUse;
		self->stack_->boundPorts_.insert(sa->svm_port);
		port = sa->svm_port;
	}

	self->ownsPort_ = true;
	self->localCid_ = sa->svm_cid;
	self->localPort_ = port;
	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Socket::listen(void *object) {
	auto self = static_cast<Socket *>(object);

	std::lock_guard stackLock{self->stack_->mutex_};
	std::lock_guard lock{self->mutex_};

	if(self->state_ == State::listening)
		co_return protocols::fs::Error::none;
	if(self->state_ != State::unconnected || self->localPort_ == VMADDR_PORT_ANY)
		co_return protocols::fs::Error::illegalArguments;

	self->stack_->listeners_.insert({self->localPort_, self->holder_.lock()});
	self->state_ = State::listening;
	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Socket::connect(void *object,
		helix_ng::CredentialsView, const void *addrPtr, size_t addrLength) {
	auto self = static_cast<Socket *>(object);

	auto sa = parseAddress(addrPtr, addrLength);
	if(!sa)
		co_return sa.error();

	auto guestCid = self->stack_->device()->guestCid();
	// There is no loopback transport.
	if(sa->svm_cid == VMADDR_CID_ANY || sa->svm_cid == VMADDR_CID_LOCAL
			|| sa->svm_cid == guestCid)
		co_return protocols::fs::Error::netUnreachable;

	auto request = co_await self->stack_->device()->allocateTx(TxClass::control);
	{
		std::lock_guard stackLock{self->stack_->mutex_};
		std::lock_guard lock{self->mutex_};

		switch(self->state_) {
		case State::unconnected:
			break;
		case State::connected:
		case State::disconnected:
			co_return protocols::fs::Error::alreadyConnected;
		default:
			co_return protocols::fs::Error::illegalArguments;
		}

		if(self->localPort_ == VMADDR_PORT_ANY) {
			auto allocated = self->stack_->allocatePort_();
			if(!allocated)
				co_return protocols::fs::Error::addressNotAvailable;
			self->ownsPort_ = true;
			self->localPort_ = *allocated;
		}
		self->localCid_ = guestCid;

		VsockStack::ConnectionKey key{self->localPort_, sa->svm_cid, sa->svm_port};
		if(self->stack_->connections_.contains(key))
			co_return protocols::fs::Error::addressInUse;

		self->peerCid_ = sa->svm_cid;
		self->peerPort_ = sa->svm_port;
		self->refused_ = false;
		self->localShutdown_ = 0;
		self->peerShutdown_ = 0;
		self->rxRing_.clear();
		self->fwdCnt_ = 0;
		self->txCnt_ = 0;
		self->peerBufAlloc_ = 0;
		self->peerFwdCnt_ = 0;
		self->state_ = State::connecting;
		self->stack_->connections_.insert({key, self->holder_.lock()});
		self->sendControl_(std::move(request), VIRTIO_VSOCK_OP_REQUEST);
	}
	self->stack_->device()->flushTx();

	// A timeout is detected by re-checking the state below.
	(void)(co_await helix::withTimeout([&](async::cancellation_token ct) {
		return self->event_.async_wait_if([&] {
			std::lock_guard lock{self->mutex_};
			return self->state_ == State::connecting;
		}, ct);
	}, helix::deadlineAfter(connectTimeout)));

	auto error = protocols::fs::Error::none;
	co_await self->stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
		std::lock_guard stackLock{self->stack_->mutex_};
		std::lock_guard lock{self->mutex_};
		if(self->state_ == State::connecting) {
			if(!buffer)
				return false;
			// There is no ETIMEDOUT in the fs protocol.
			self->sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_RST);
			self->forgetLocked_();
			self->state_ = State::unconnected;
			error = protocols::fs::Error::hostUnreachable;
		}else if(self->refused_) {
			error = protocols::fs::Error::connectionRefused;
		}
		self->notify_(EPOLLOUT);
		return true;
	});
	self->stack_->device()->flushTx();
	self->event_.raise();
	co_return error;
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::AcceptResult>>
Socket::accept(void *object) {
	auto self = static_cast<Socket *>(object);

	smarter::shared_ptr<Socket> child;
	while(true) {
		{
			std::lock_guard lock{self->mutex_};
			if(self->state_ != State::listening)
				co_return protocols::fs::Error::illegalArguments;
			if(!self->acceptQueue_.empty()) {
				child = std::move(self->acceptQueue_.front());
				self->acceptQueue_.pop_front();
				break;
			}
			if(self->nonBlock_)
				co_return protocols::fs::Error::wouldBlock;
		}

		co_await self->event_.async_wait_if([&] {
			std::lock_guard lock{self->mutex_};
			return self->state_ == State::listening && self->acceptQueue_.empty();
		});
	}

	auto [localCtrl, remoteCtrl] = helix::createStream();
	auto [localPt, remotePt] = helix::createStream();
	helix::DispatcherPool::global().detach(
			serve(std::move(localCtrl), std::move(localPt), std::move(child)));

	co_return protocols::fs::AcceptResult{std::move(remoteCtrl), std::move(remotePt)};
}

async::result<size_t> Socket::sockname(void *object, void *addrPtr, size_t maxAddrLength) {
	auto self = static_cast<Socket *>(object);
	std::lock_guard lock{self->mutex_};
	co_return writeAddress(addrPtr, maxAddrLength, self->localCid_, self->localPort_);
}

async::result<frg::expected<protocols::fs::Error, size_t>> Socket::peername(void *object,
		void *addrPtr, size_t maxAddrLength) {
	auto self = static_cast<Socket *>(object);
	std::lock_guard lock{self->mutex_};
	if(self->state_ != State::connected && self->state_ != State::disconnected)
		co_return protocols::fs::Error::notConnected;
	co_return writeAddress(addrPtr, maxAddrLength, self->peerCid_, self->peerPort_);
}

async::result<int> Socket::getFileFlags(void *object) {
	auto self = static_cast<Socket *>(object);
	std::lock_guard lock{self->mutex_};
	co_return self->nonBlock_ ? O_NONBLOCK : 0;
}

async::result<void> Socket::setFileFlags(void *object, int flags) {
	auto self = static_cast<Socket *>(object);
	if(flags & ~O_NONBLOCK)
		std::println("vsock: Ignoring file flags {:#x}", flags & ~O_NONBLOCK);
	std::lock_guard lock{self->mutex_};
	self->nonBlock_ = flags & O_NONBLOCK;
	co_return;
}

async::result<frg::expected<protocols::fs::Error>> Socket::setSocketOption(void *object,
		int layer, int number, std::vector<char> optbuf) {
	auto self = static_cast<Socket *>(object);

	if(layer == AF_VSOCK && (number == SO_VM_SOCKETS_BUFFER_SIZE
			|| number == SO_VM_SOCKETS_BUFFER_MIN_SIZE
			|| number == SO_VM_SOCKETS_BUFFER_MAX_SIZE)) {
		uint64_t value;
		if(optbuf.size() < sizeof(value))
			co_return protocols::fs::Error::illegalArguments;
		memcpy(&value, optbuf.data(), sizeof(value));

		co_await self->stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
			std::lock_guard lock{self->mutex_};
			if(!buffer && self->state_ == State::connected)
				return false;
			// Same semantics as vsock_update_buffer_size() in Linux.
			if(number == SO_VM_SOCKETS_BUFFER_SIZE) {
				self->setBufferSize_(value, buffer);
			}else if(number == SO_VM_SOCKETS_BUFFER_MIN_SIZE) {
				self->bufferMinSize_ = value;
				self->setBufferSize_(self->bufferSize_, buffer);
			}else{
				self->bufferMaxSize_ = value;
				self->setBufferSize_(self->bufferSize_, buffer);
			}
			return true;
		});
		self->stack_->device()->flushTx();
		co_return {};
	}

	std::println("vsock: Unhandled setsockopt() layer {} number {}", layer, number);
	co_return protocols::fs::Error::invalidProtocolOption;
}

async::result<frg::expected<protocols::fs::Error>> Socket::getSocketOption(void *object,
		helix_ng::CredentialsView, int layer, int number, std::vector<char> &optbuf) {
	auto self = static_cast<Socket *>(object);

	auto put = [&] <typename T> (T value) {
		optbuf.resize(std::min(optbuf.size(), sizeof(T)));
		memcpy(optbuf.data(), &value, optbuf.size());
	};

	if(layer == SOL_SOCKET && number == SO_TYPE) {
		put(int{SOCK_STREAM});
	}else if(layer == SOL_SOCKET && number == SO_ERROR) {
		put(int{0});
	}else if(layer == AF_VSOCK && number == SO_VM_SOCKETS_BUFFER_SIZE) {
		std::lock_guard lock{self->mutex_};
		put(uint64_t{self->bufferSize_});
	}else if(layer == AF_VSOCK && number == SO_VM_SOCKETS_BUFFER_MIN_SIZE) {
		std::lock_guard lock{self->mutex_};
		put(self->bufferMinSize_);
	}else if(layer == AF_VSOCK && number == SO_VM_SOCKETS_BUFFER_MAX_SIZE) {
		std::lock_guard lock{self->mutex_};
		put(self->bufferMaxSize_);
	}else{
		std::println("vsock: Unhandled getsockopt() layer {} number {}", layer, number);
		co_return protocols::fs::Error::invalidProtocolOption;
	}
	co_return {};
}

async::result<protocols::fs::Error> Socket::shutdown(void *object, int how) {
	auto self = static_cast<Socket *>(object);

	uint32_t flags;
	if(how == SHUT_RD)
		flags = VIRTIO_VSOCK_SHUTDOWN_RCV;
	else if(how == SHUT_WR)
		flags = VIRTIO_VSOCK_SHUTDOWN_SEND;
	else if(how == SHUT_RDWR)
		flags = shutdownBoth;
	else
		co_return protocols::fs::Error::illegalArguments;

	auto error = protocols::fs::Error::none;
	co_await self->stack_->device()->withTx(TxClass::control, [&] (TxBuffer &buffer) {
		std::lock_guard lock{self->mutex_};
		if(self->state_ != State::connected) {
			error = protocols::fs::Error::notConnected;
			return true;
		}
		if(!buffer)
			return false;
		self->localShutdown_ |= flags;
		self->sendControl_(std::move(buffer), VIRTIO_VSOCK_OP_SHUTDOWN, flags);
		self->notify_(EPOLLIN | EPOLLHUP | EPOLLRDHUP);
		return true;
	});
	if(error != protocols::fs::Error::none)
		co_return error;
	self->stack_->device()->flushTx();
	self->event_.raise();
	co_return protocols::fs::Error::none;
}

const protocols::fs::FileOperations Socket::ops{
	.read = &read,
	.write = &write,
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
	.shutdown = &shutdown,
};

} // namespace vsock
