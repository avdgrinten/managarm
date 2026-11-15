#pragma once

#include <deque>
#include <mutex>
#include <span>
#include <vector>

#include <async/mutex.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <linux/virtio_vsock.h>
#include <linux/vm_sockets.h>
#include <protocols/fs/server.hpp>
#include <smarter.hpp>

namespace vsock {

struct TxBuffer;
struct VsockStack;

// FIFO of bytes whose storage is only allocated once data arrives.
struct ByteRing {
	size_t size() const {
		return size_;
	}

	size_t space() const {
		return capacity_ - size_;
	}

	bool empty() const {
		return !size_;
	}

	// Requires data.size() <= space().
	void push(std::span<const char> data);

	// Copies up to size bytes from the front of the ring and returns the number of bytes copied.
	size_t peek(char *out, size_t size) const;

	void discard(size_t size);

	void clear() {
		head_ = 0;
		size_ = 0;
	}

	// The capacity never shrinks, such that data that is already in flight always fits.
	void growCapacity(size_t capacity);

private:
	size_t capacity_ = 0;
	std::vector<char> buffer_;
	size_t head_ = 0;
	size_t size_ = 0;
};

// A SOCK_STREAM vsock socket.
//
// All mutable state is protected by mutex_. event_ is raised after mutex_ is released
// whenever the state changes; waiters re-check their condition under mutex_.
struct Socket {
	friend struct VsockStack;

	static smarter::shared_ptr<Socket> create(VsockStack *stack, bool nonBlock);

	Socket(VsockStack *stack, bool nonBlock);

	Socket(const Socket &) = delete;

	Socket &operator= (const Socket &) = delete;

	// Serves the file lanes of the socket and closes the socket once the file is closed.
	static async::result<void> serve(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane,
			smarter::shared_ptr<Socket> self);

private:
	enum class State {
		// Not connected; may be bound.
		unconnected,
		// connect() sent a REQUEST and waits for the RESPONSE.
		connecting,
		connected,
		listening,
		// The connection is gone, but buffered data can still be read.
		disconnected,
		// The file was closed after a SHUTDOWN was sent; waits for the peer's RST.
		closing,
		// The file was closed without a connection.
		// Terminal since requests that are still in flight must not bind, listen or connect.
		closed
	};

	// Handles a packet of a published connection; the caller raises event_ afterwards.
	// The reply buffer is consumed if the packet is answered.
	// Requires VsockStack::mutex_ such that the connection cannot be replaced concurrently.
	void handlePacket_(const virtio_vsock_hdr &hdr, std::span<const char> payload,
			TxBuffer &reply);

	// Handles a REQUEST to a listening socket.
	// The reply buffer is consumed if the connection is established.
	// Requires VsockStack::mutex_ since the new connection is published immediately.
	smarter::shared_ptr<Socket> handleRequest_(const virtio_vsock_hdr &hdr, TxBuffer &reply);

	// Moves the socket to the disconnected state without sending any packet.
	void handleReset_();

	async::result<void> close_();

	// Announces the space in the receive buffer unless another packet already did.
	async::result<void> sendCreditUpdate_();

	// Requires mutex_.
	// The buffer is consumed if the socket is connected (i.e., if the peer has to learn the new size).
	void setBufferSize_(uint64_t size, TxBuffer &buffer);
	virtio_vsock_hdr makeHeader_(uint16_t op, uint32_t len = 0);
	// Takes a buffer of TxClass::control or TxClass::reply.
	void sendControl_(TxBuffer buffer, uint16_t op, uint32_t flags = 0);
	// Whether the peer is about to stall since it does not know about space in the receive buffer.
	bool creditUpdateDue_();
	uint32_t credit_();
	int edgesSince_(uint64_t pastSeq);
	int activeEvents_();
	void notify_(int edges);
	bool readable_();
	bool writable_();

	// Requires VsockStack::mutex_ and mutex_.
	void forgetLocked_();

	static async::result<protocols::fs::ReadResult> read(void *object,
			helix_ng::CredentialsView creds, void *data, size_t size,
			async::cancellation_token cancellation);
	static async::result<frg::expected<protocols::fs::Error, size_t>> write(void *object,
			helix_ng::CredentialsView creds, const void *data, size_t size);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t pastSeq, int mask, async::cancellation_token cancellation);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);
	static async::result<protocols::fs::Error> bind(void *object,
			helix_ng::CredentialsView creds, const void *addrPtr, size_t addrLength);
	static async::result<protocols::fs::Error> listen(void *object);
	static async::result<protocols::fs::Error> connect(void *object,
			helix_ng::CredentialsView creds, const void *addrPtr, size_t addrLength);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::AcceptResult>>
	accept(void *object);
	static async::result<size_t> sockname(void *object, void *addrPtr, size_t maxAddrLength);
	static async::result<int> getFileFlags(void *object);
	static async::result<void> setFileFlags(void *object, int flags);
	static async::result<protocols::fs::RecvResult> recvMsg(void *object,
			helix_ng::CredentialsView creds, uint32_t flags, void *data, size_t size,
			void *addrPtr, size_t addrLength, size_t maxCtrlLength);
	static async::result<frg::expected<protocols::fs::Error, size_t>> sendMsg(void *object,
			helix_ng::CredentialsView creds, uint32_t flags, void *data, size_t size,
			void *addrPtr, size_t addrLength, std::vector<uint32_t> fds, struct ucred ucreds);
	static async::result<frg::expected<protocols::fs::Error, size_t>> peername(void *object,
			void *addrPtr, size_t maxAddrLength);
	static async::result<frg::expected<protocols::fs::Error>> setSocketOption(void *object,
			int layer, int number, std::vector<char> optbuf);
	static async::result<frg::expected<protocols::fs::Error>> getSocketOption(void *object,
			helix_ng::CredentialsView creds, int layer, int number, std::vector<char> &optbuf);
	static async::result<protocols::fs::Error> shutdown(void *object, int how);

	static const protocols::fs::FileOperations ops;

	async::result<std::expected<size_t, protocols::fs::Error>> receive_(uint32_t flags,
			void *data, size_t size, async::cancellation_token cancellation);
	async::result<std::expected<size_t, protocols::fs::Error>> send_(uint32_t flags,
			const void *data, size_t size);

	VsockStack *stack_;
	smarter::weak_ptr<Socket> holder_;

	std::mutex mutex_;
	async::recurring_event event_;

	// Serializes write operations such that their data is not interleaved.
	async::mutex writeMutex_;

	// Protected by mutex_.
	bool nonBlock_;
	// Protected by mutex_.
	State state_ = State::unconnected;

	// Whether this socket owns localPort_ in VsockStack::boundPorts_.
	// Protected by mutex_.
	bool ownsPort_ = false;
	// Protected by mutex_.
	uint32_t localCid_ = VMADDR_CID_ANY;
	// Protected by mutex_.
	uint32_t localPort_ = VMADDR_PORT_ANY;
	// Protected by mutex_.
	uint64_t peerCid_ = 0;
	// Protected by mutex_.
	uint32_t peerPort_ = 0;

	// Set if the last connect() was refused or reset before it completed.
	// Protected by mutex_.
	bool refused_ = false;

	// VIRTIO_VSOCK_SHUTDOWN_* bits.
	// Protected by mutex_.
	uint32_t localShutdown_ = 0;
	// Protected by mutex_.
	uint32_t peerShutdown_ = 0;

	// Receive buffer size that we advertise to the peer, and its bounds (as in Linux).
	// Protected by mutex_.
	uint32_t bufferSize_;
	// Protected by mutex_.
	uint64_t bufferMinSize_;
	// Protected by mutex_.
	uint64_t bufferMaxSize_;

	// Protected by mutex_.
	ByteRing rxRing_;
	// Number of bytes that were removed from rxRing_.
	// Protected by mutex_.
	uint32_t fwdCnt_ = 0;
	// Value of fwdCnt_ in the last packet that we sent.
	// Protected by mutex_.
	uint32_t lastFwdCntSent_ = 0;

	// Number of bytes that we sent.
	// Protected by mutex_.
	uint32_t txCnt_ = 0;
	// Protected by mutex_.
	uint32_t peerBufAlloc_ = 0;
	// Protected by mutex_.
	uint32_t peerFwdCnt_ = 0;

	// Connections that are established but not yet accepted.
	// Protected by mutex_.
	std::deque<smarter::shared_ptr<Socket>> acceptQueue_;

	// Sequence numbers for poll().
	// Protected by mutex_.
	uint64_t currentSeq_ = 1;
	// Protected by mutex_.
	uint64_t inSeq_ = 0;
	// Protected by mutex_.
	uint64_t outSeq_ = 0;
	// Protected by mutex_.
	uint64_t hupSeq_ = 0;
	// Protected by mutex_.
	uint64_t rdhupSeq_ = 0;
};

} // namespace vsock
