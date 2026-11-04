#include <core/bpf.hpp>
#include <linux/netlink.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <linux/filter.h>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>
#include <format>

#include <async/recurring-event.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/common.hpp>
#include "nl-socket.hpp"
#include "nlctrl.hpp"
#include "uevent.hpp"
#include "../process.hpp"

namespace netlink::nl_socket {

using core::netlink::Group;

struct OpenFile;

constexpr bool logSockets = false;
constexpr bool logBroadcasts = false;

// Only written by setupProtocols().
std::map<int, const ops *> globalProtocolOpsMap;

std::mutex globalMutex;

/* (protocol, groupid) -> std::unique_ptr<Group> */
// The groups themselves are only created by setupProtocols(); their subscriptions are
// protected by globalMutex.
std::map<std::pair<int, uint8_t>, std::unique_ptr<Group>> globalGroupMap;

// Protected by globalMutex.
uint32_t nextPort = UINT32_MAX;
// Protected by globalMutex.
std::map<uint32_t, OpenFile *> globalPortMap;

void deliverToGroup(int protocol, uint32_t group, const core::netlink::Packet &packet) {
	std::vector<smarter::shared_ptr<File>> subscribers;
	{
		std::lock_guard lock{globalMutex};
		auto it = globalGroupMap.find({protocol, group});
		if(it == globalGroupMap.end())
			return;
		for(auto socket : it->second->subscriptions) {
			// Closed sockets leave their groups before they are destructed,
			// but the File may already be on its way out.
			if(auto file = static_cast<OpenFile *>(socket)->weakFile().lock(); file)
				subscribers.push_back(std::move(file));
		}
	}

	for(auto &file : subscribers)
		static_cast<OpenFile *>(file.get())->deliver(packet);
}

smarter::shared_ptr<File> findPort(uint32_t port) {
	std::lock_guard lock{globalMutex};
	auto it = globalPortMap.find(port);
	if(it == globalPortMap.end())
		return nullptr;
	return it->second->weakFile().lock();
}

// ----------------------------------------------------------------------------
// OpenFile implementation.
// ----------------------------------------------------------------------------

OpenFile::OpenFile(int protocol, int type, bool nonBlock)
		: FileWithDefaults{FileKind::unknown,  StructName::get("nl-socket"), nullptr,
		SpecialLink::makeSpecialLink(VfsType::socket, 0777), File::defaultPipeLikeSeek},
		_protocol{protocol}, ops_(globalProtocolOpsMap.at(protocol)), _currentSeq{1},
		_inSeq{0}, _socketPort{0}, _passCreds{false}, nonBlock_{nonBlock}, type_{type} { }

void OpenFile::handleClose() {
	{
		std::lock_guard lock{_mutex};
		_isClosed = true;
	}
	_statusBell.raise();
	_cancelServe.cancel();

	std::lock_guard lock{globalMutex};
	if(_socketPort) {
		auto it = globalPortMap.find(_socketPort);
		assert(it != globalPortMap.end() && it->second == this);
		globalPortMap.erase(it);
	}
	for(int i = 0; i < MAX_SUPPORTED_GROUP_ID; i++) {
		if(joinedGroups_ & (1 << i)) {
			// Remove the netlink socket from the subscription vector
			auto it = globalGroupMap.find({_protocol, i + 1});
			assert(it != globalGroupMap.end());
			auto group = it->second.get();
			auto self = std::find(group->subscriptions.begin(), group->subscriptions.end(), this);
			assert(self != group->subscriptions.end());
			group->subscriptions.erase(self);
		}
	}
}

void OpenFile::deliver(core::netlink::Packet packet) {
	{
		std::lock_guard lock{_mutex};
		if(filter_) {
			Bpf bpf{filter_.value()};
			size_t accept_bytes = bpf.run(arch::dma_buffer_view{nullptr, packet.buffer.data(), packet.buffer.size()});

			if(!accept_bytes)
				return;

			if(accept_bytes < packet.buffer.size())
				packet.buffer.resize(accept_bytes);
		}

		_recvQueue.push_back(std::move(packet));
		_inSeq = ++_currentSeq;
	}
	_statusBell.raise();
}

async::result<std::expected<size_t, Error>>
OpenFile::readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) {
	if(logSockets)
		std::cout << "posix: Read from socket " << this << std::endl;

	while(true) {
		{
			std::lock_guard lock{_mutex};
			if(!_recvQueue.empty()) {
				// TODO: Truncate packets (for SOCK_DGRAM) here.
				auto packet = &_recvQueue.front();
				auto size = packet->buffer.size();
				assert(max_length >= size);
				memcpy(data, packet->buffer.data(), size);
				_recvQueue.pop_front();
				co_return size;
			}

			if(nonBlock_) {
				if(logSockets)
					std::cout << "posix: netlink socket would block" << std::endl;
				co_return std::unexpected{Error::wouldBlock};
			}
		}

		auto waitResult = co_await _statusBell.async_wait_if([&] {
			std::lock_guard lock{_mutex};
			return _recvQueue.empty();
		}, ce);
		if (!waitResult)
			co_return std::unexpected{Error::interrupted};
	}
}

async::result<frg::expected<Error, size_t>>
OpenFile::writeAll(Process *, const void *data, size_t length) {
	(void) data;
	(void) length;

	throw std::runtime_error("posix: Fix netlink send()");
/*
	if(logSockets)
		std::cout << "posix: Write to socket " << this << std::endl;

	Packet packet;
	packet.buffer.resize(length);
	memcpy(packet.buffer.data(), data, length);

	_remote->deliver(std::move(packet));
*/

	co_return {};
}

async::result<protocols::fs::RecvResult>
OpenFile::recvMsg(Process *, uint32_t flags, void *data, size_t max_length,
		void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length) {
	(void) max_addr_length;

	using namespace protocols::fs;
	if(logSockets)
		std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;
	if(flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_PEEK | MSG_TRUNC)) {
		std::cout << std::format("posix: Unsupported flags 0x{:x} in recvMsg", flags) << std::endl;
	}

	std::unique_lock lock{_mutex};
	if(_recvQueue.empty() && ((flags & MSG_DONTWAIT) || nonBlock_)) {
		if(logSockets)
			std::cout << "posix: netlink socket would block" << std::endl;
		co_return RecvResult { protocols::fs::Error::wouldBlock };
	}

	while(_recvQueue.empty()) {
		lock.unlock();
		co_await _statusBell.async_wait_if([&] {
			std::lock_guard guard{_mutex};
			return _recvQueue.empty();
		});
		lock.lock();
	}

	// TODO: Truncate packets (for SOCK_DGRAM) here.
	auto packet = &_recvQueue.front();

	auto size = packet->buffer.size();
	auto truncated_size = std::min(size, max_length);
	uint32_t reply_flags = 0;

	auto chunk = std::min(packet->buffer.size() - packet->offset, max_length);
	memcpy(data, packet->buffer.data() + packet->offset, chunk);
	if(!(flags & MSG_PEEK)) {
		packet->offset += chunk;
	}

	if(addr_ptr) {
		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(struct sockaddr_nl));
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = packet->senderPort;
		sa.nl_groups = packet->group ? (1 << (packet->group - 1)) : 0;
		memcpy(addr_ptr, &sa, sizeof(struct sockaddr_nl));
	}

	protocols::fs::CtrlBuilder ctrl{max_ctrl_length};

	if(_passCreds) {
		struct ucred creds;
		memset(&creds, 0, sizeof(struct ucred));
		creds.pid = packet->senderPid;

		auto truncated = ctrl.message(SOL_SOCKET, SCM_CREDENTIALS, sizeof(struct ucred));
		if(!truncated)
			ctrl.write(creds);
		else
			reply_flags |= MSG_CTRUNC;
	}

	if(pktinfo_) {
		struct nl_pktinfo info;
		info.group = packet->group;

		auto truncated = ctrl.message(SOL_NETLINK, NETLINK_PKTINFO, sizeof(info));
		if(truncated)
			reply_flags |= MSG_CTRUNC;
		else
			ctrl.write(info);
	}

	if(!(flags & MSG_PEEK)) {
		_recvQueue.pop_front();
	}

	if(truncated_size < size) {
		reply_flags |= MSG_TRUNC;
	}

	co_return RecvData{ctrl.buffer(), (flags & MSG_TRUNC) ? size : truncated_size,
		sizeof(struct sockaddr_nl), reply_flags};
}

async::result<frg::expected<protocols::fs::Error, size_t>>
OpenFile::sendMsg(Process *process, uint32_t flags, const void *data, size_t max_length,
		const void *addr_ptr, size_t addr_length,
		std::vector<smarter::shared_ptr<File, FileHandle>> files, struct ucred) {
	if(logSockets)
		std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;
	assert(!flags);
	assert(files.empty());

	struct sockaddr_nl sa;
	if(addr_length >= sizeof(struct sockaddr_nl) && addr_ptr) {
		memcpy(&sa, addr_ptr, sizeof(struct sockaddr_nl));
	} else {
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
	}

	size_t grp_idx = 0;
	if(sa.nl_groups) {
		// Linux allows multicast only to a single group at a time.
		grp_idx = __builtin_ffs(sa.nl_groups);
	}

	// Closed sockets do not get a port, and port 0 would claim that the kernel sent this.
	auto port = socketPort();
	if(!port)
		co_return protocols::fs::Error::illegalArguments;

	core::netlink::Packet packet;
	packet.senderPid = process->pid();
	packet.senderPort = port;
	packet.group = grp_idx;
	packet.buffer.resize(max_length);
	memcpy(packet.buffer.data(), data, max_length);

	if(ops_ && ops_->sendMsg) {
		auto res = co_await ops_->sendMsg(this, packet, &sa);

		if(res != protocols::fs::Error::none)
			co_return res;
	} else {
		co_return protocols::fs::Error::illegalOperationTarget;
	}

	co_return max_length;
}

async::result<frg::expected<Error, PollWaitResult>>
OpenFile::pollWait(Process *, uint64_t past_seq, int mask,
		async::cancellation_token cancellation) {
	int edges = 0;
	uint64_t sequence;

	while (true) {
		{
			std::lock_guard lock{_mutex};
			assert(past_seq <= _currentSeq);
			if(_isClosed)
				co_return Error::fileClosed;
			sequence = _currentSeq;

			// For now making sockets always writable is sufficient.
			edges = EPOLLOUT;
			if(_inSeq > past_seq)
				edges |= EPOLLIN;
		}

		if (edges & mask)
			break;

		auto waitResult = co_await _statusBell.async_wait_if([&] {
			std::lock_guard lock{_mutex};
			return !_isClosed && _currentSeq == sequence;
		}, cancellation);
		if (!waitResult)
			break;
	}

	// std::println("posix: pollWait({}, {}) on \e[1;34m{}\e[0m returns ({}, {})",
	// 	past_seq, mask, structName(), sequence, edges & mask);

	co_return PollWaitResult(sequence, edges & mask);
}

async::result<frg::expected<Error, PollStatusResult>> OpenFile::pollStatus(Process *) {
	std::lock_guard lock{_mutex};
	int events = EPOLLOUT;
	if(!_recvQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult(_currentSeq, events);
}

async::result<protocols::fs::Error> OpenFile::bind(Process *,
		const void *addr_ptr, size_t addr_length) {
	if(addr_length < sizeof(struct sockaddr_nl))
		co_return protocols::fs::Error::illegalArguments;

	struct sockaddr_nl sa;
	memcpy(&sa, addr_ptr, addr_length);

	std::lock_guard lock{globalMutex};
	// handleClose() takes globalMutex after isOpen() turns false, so it undoes what we do here.
	if(!isOpen())
		co_return protocols::fs::Error::illegalArguments;
	if(_socketPort) {
		// Like Linux, a bound socket can only be bound to the same port again.
		if(sa.nl_pid != _socketPort)
			co_return protocols::fs::Error::illegalArguments;
	} else if(!sa.nl_pid) {
		_associatePort();
	} else {
		auto res = globalPortMap.insert({sa.nl_pid, this});
		if(!res.second)
			co_return protocols::fs::Error::addressInUse;
		_socketPort = sa.nl_pid;
	}

	if(sa.nl_groups) {
		for(int i = 0; i < MAX_BITMAP_GROUP_ID; i++) {
			if(!(sa.nl_groups & (1 << i)))
				continue;
			if(joinedGroups_ & (1 << i))
				continue;
			// Like Linux, ignore groups that the protocol does not have.
			auto it = globalGroupMap.find({_protocol, i + 1});
			if(it == globalGroupMap.end())
				continue;
			std::cout << "posix: Join netlink group "
					<< _protocol << "." << (i + 1) << std::endl;

			auto group = it->second.get();
			group->subscriptions.push_back(this);
			joinedGroups_ |= 1 << i;
		}
	}

	// Do nothing for now.
	co_return protocols::fs::Error::none;
}

async::result<size_t> OpenFile::sockname(void *addr_ptr, size_t max_addr_length) {
	// TODO: Fill in nl_groups.
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(struct sockaddr_nl));
	sa.nl_family = AF_NETLINK;
	{
		std::lock_guard lock{globalMutex};
		assert(_socketPort);
		sa.nl_pid = _socketPort;
	}
	memcpy(addr_ptr, &sa, std::min(sizeof(struct sockaddr_nl), max_addr_length));

	co_return sizeof(struct sockaddr_nl);
}

async::result<frg::expected<protocols::fs::Error>> OpenFile::setSocketOption(int layer, int number,
		std::vector<char> optbuf) {
	if(layer == SOL_SOCKET && number == SO_ATTACH_FILTER) {
		assert(optbuf.size() % sizeof(struct sock_filter) == 0);

		Bpf bpf{optbuf};
		if(!bpf.validate())
			co_return protocols::fs::Error::illegalArguments;

		std::lock_guard lock{_mutex};
		filter_ = optbuf;
	} else if(layer == SOL_NETLINK && number == NETLINK_ADD_MEMBERSHIP) {
		if(optbuf.size() < sizeof(int))
			co_return protocols::fs::Error::illegalArguments;
		auto val = *reinterpret_cast<int *>(optbuf.data());
		std::cout << "posix: Join netlink group "
					<< _protocol << "." << val << std::endl;
		// Group val is recorded as bit val - 1 of joinedGroups_.
		if(val < 1 || val > MAX_SUPPORTED_GROUP_ID)
			co_return protocols::fs::Error::illegalArguments;
		auto it = globalGroupMap.find({_protocol, val});
		if(it == globalGroupMap.end())
			co_return protocols::fs::Error::illegalArguments;
		std::lock_guard lock{globalMutex};
		// See bind() for why this check has to be under globalMutex.
		if(!isOpen())
			co_return protocols::fs::Error::illegalArguments;
		if(!(joinedGroups_ & (1 << (val - 1)))) {
			auto group = it->second.get();
			group->subscriptions.push_back(this);
			joinedGroups_ |= 1 << (val - 1);
		}
	} else if(layer == SOL_NETLINK && number == NETLINK_PKTINFO) {
		auto val = *reinterpret_cast<int *>(optbuf.data());
		std::lock_guard lock{_mutex};
		pktinfo_ = (val != 0);
	} else if(layer == SOL_SOCKET && number == SO_PASSCRED) {
		if(optbuf.size() >= sizeof(int)) {
			std::lock_guard lock{_mutex};
			_passCreds = *reinterpret_cast<int *>(optbuf.data());
		}
	} else {
		printf("netserver: unhandled setsockopt layer %d number %d\n", layer, number);
		co_return protocols::fs::Error::invalidProtocolOption;
	}

	co_return {};
}

async::result<frg::expected<protocols::fs::Error>> OpenFile::getSocketOption(Process *,
		int layer, int number, std::vector<char> &optbuf) {
	if(layer == SOL_SOCKET && number == SO_PROTOCOL) {
		optbuf.resize(std::min(optbuf.size(), sizeof(_protocol)));
		memcpy(optbuf.data(), &_protocol, optbuf.size());
	} else if(layer == SOL_SOCKET && number == SO_TYPE) {
		optbuf.resize(std::min(optbuf.size(), sizeof(type_)));
		memcpy(optbuf.data(), &type_, optbuf.size());
	} else {
		printf("posix nl-socket: unhandled getsockopt layer %d number %d\n", layer, number);
		co_return protocols::fs::Error::invalidProtocolOption;
	}

	co_return {};
}

async::result<void> OpenFile::setFileFlags(int flags) {
	if (flags & ~O_NONBLOCK) {
		std::cout << "posix: setFileFlags on netlink socket \e[1;34m" << structName() << "\e[0m called with unknown flags" << std::endl;
		co_return;
	}
	if (flags & O_NONBLOCK)
		nonBlock_ = true;
	else
		nonBlock_ = false;
	co_return;
}

async::result<int> OpenFile::getFileFlags() {
	if(nonBlock_)
		co_return O_NONBLOCK;
	co_return 0;
}

uint32_t OpenFile::socketPort() {
	std::lock_guard lock{globalMutex};
	// After handleClose(), nothing would release a port that we take here.
	if(!_socketPort && isOpen())
		_associatePort();
	return _socketPort;
}

// Must be called with globalMutex held.
void OpenFile::_associatePort() {
	assert(!_socketPort);
	// Skip ports that were bound explicitly, and port 0 since it means "unbound".
	while(!nextPort || globalPortMap.contains(nextPort))
		nextPort--;
	_socketPort = nextPort--;
	auto res = globalPortMap.insert({_socketPort, this});
	assert(res.second);
}

// ----------------------------------------------------------------------------
// Free functions.
// ----------------------------------------------------------------------------

void setupProtocols() {
	configure(NETLINK_KOBJECT_UEVENT, 32, &netlink::uevent::ops);
	configure(NETLINK_GENERIC, 32, &netlink::nlctrl::ops);
}

void configure(int protocol, size_t num_groups, const ops *ops) {
	assert(num_groups <= MAX_SUPPORTED_GROUP_ID);

	globalProtocolOpsMap.insert({protocol, ops});

	for(size_t i = 0; i < num_groups; i++) {
		std::pair<int, int> idx{protocol, i + 1};
		auto res = globalGroupMap.insert(std::make_pair(idx, std::make_unique<Group>()));
		assert(res.second);
	}
}

void broadcast(int proto_idx, uint32_t grp_idx, std::string buffer) {
	core::netlink::Packet packet{
		.group = grp_idx,
	};
	packet.buffer.resize(buffer.size());
	memcpy(packet.buffer.data(), buffer.data(), buffer.size());

	assert(globalGroupMap.contains({proto_idx, grp_idx}));
	deliverToGroup(proto_idx, grp_idx, packet);

	if(logBroadcasts)
		std::cout << std::format("posix/netlink: broadcasting \"{}\"", buffer) << std::endl;
}

bool protocol_supported(int protocol) {
	return globalProtocolOpsMap.contains(protocol);
}

smarter::shared_ptr<File, FileHandle> createSocketFile(int protocol, int type, bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(protocol, type, nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

} // namespace netlink::nl_socket

