#pragma once

#include <async/recurring-event.hpp>
#include <linux/netlink.h>
#include <map>
#include <mutex>

#include "core/netlink.hpp"
#include "../file.hpp"

namespace netlink::nl_socket {

constexpr int MAX_SUPPORTED_GROUP_ID = 32;
constexpr int MAX_BITMAP_GROUP_ID = 32;

void setupProtocols();

struct OpenFile;

struct ops {
	async::result<protocols::fs::Error> (*sendMsg)(nl_socket::OpenFile *f, core::netlink::Packet packet, struct sockaddr_nl *sa);
};

extern std::map<int, const ops *> globalProtocolOpsMap;

// Delivers a copy of the packet to every socket that joined the group.
void deliverToGroup(int protocol, uint32_t group, const core::netlink::Packet &packet);

// Returns the socket that is bound to the given port, or null.
smarter::shared_ptr<File> findPort(uint32_t port);

struct OpenFile : FileWithDefaults, core::netlink::NetlinkFile {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(int protocol, int type, bool nonBlock = false);

	void deliver(core::netlink::Packet packet) override;

	void handleClose() override;

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<protocols::fs::RecvResult>
	recvMsg(Process *, uint32_t flags, void *data, size_t max_length,
			void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length) override;

	async::result<frg::expected<protocols::fs::Error, size_t>>
	sendMsg(Process *process, uint32_t flags,
			const void *data, size_t max_length,
			const void *addr_ptr, size_t addr_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files, struct ucred ucreds) override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t past_seq, int mask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override;

	async::result<protocols::fs::Error>
	bind(Process *, const void *, size_t) override;

	async::result<size_t> sockname(void *, size_t) override;

	async::result<frg::expected<protocols::fs::Error>>
	setSocketOption(int layer, int number, std::vector<char> optbuf) override;

	async::result<frg::expected<protocols::fs::Error>>
	getSocketOption(Process *process, int layer, int number, std::vector<char> &optbuf) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<void> setFileFlags(int flags) override;
	async::result<int> getFileFlags() override;

	// Binds the socket to a free port if it is not bound yet.
	// Returns 0 if the socket is closed and unbound.
	uint32_t socketPort();

private:
	void _associatePort();

	int _protocol;
	const ops *ops_;
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// Leaf lock.
	std::mutex _mutex;

	// Status management for poll().
	async::recurring_event _statusBell;
	// Protected by _mutex.
	bool _isClosed = false;
	// Protected by _mutex.
	uint64_t _currentSeq;
	// Protected by _mutex.
	uint64_t _inSeq;

	// Protected by globalMutex in nl-socket.cpp.
	uint32_t _socketPort;

	// The actual receive queue of the socket.
	// Protected by _mutex.
	std::deque<core::netlink::Packet> _recvQueue;

	// Socket options.
	// Protected by _mutex.
	bool _passCreds;
	std::atomic<bool> nonBlock_;
	// Protected by _mutex.
	bool pktinfo_;

	// BPF filter
	// Protected by _mutex.
	std::optional<std::vector<char>> filter_ = std::nullopt;

	// Group subscriptions
	// TODO(no92): handle group IDs >= MAX_BITMAP_GROUP_ID
	// Protected by globalMutex in nl-socket.cpp.
	uint32_t joinedGroups_ = 0;

	int type_ = 0;
};

// Configures the given netlink protocol.
// TODO: Let this take a callback that is called on receive?
void configure(int proto_idx, size_t num_groups, const ops *ops);

// Broadcasts a kernel message to the given netlink multicast group.
void broadcast(int proto_idx, uint32_t grp_idx, std::string buffer);

bool protocol_supported(int protocol);

smarter::shared_ptr<File, FileHandle> createSocketFile(int proto_idx, int type, bool nonBlock);

} // namespace netlink::nl_socket

