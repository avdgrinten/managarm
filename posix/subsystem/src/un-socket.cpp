
#include <async/cancellation.hpp>
#include <cstddef>
#include <cstring>
#include <format>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <iostream>
#include <print>

#include <asm-generic/socket.h>
#include <async/recurring-event.hpp>
#include <core/clock.hpp>
#include <core/dispatch.hpp>
#include <bragi/helpers-std.hpp>
#include <protocols/fs/common.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include "fs.bragi.hpp"
#include "un-socket.hpp"
#include "pidfd.hpp"
#include "process.hpp"
#include "vfs.hpp"

namespace {

constexpr int shutdownRead = 1;
constexpr int shutdownWrite = 2;

}

namespace un_socket {

static constexpr bool logSockets = false;

struct OpenFile;

// This map associates bound sockets with FS nodes.
// Sockets remove their entries when they are closed; they keep the node alive until then.
std::map<FsNode *, OpenFile *> globalBindMap;
std::unordered_map<std::string, OpenFile *> abstractSocketsBindMap;

std::array<int, 3> supportedSocketTypes = {
	SOCK_STREAM,
	SOCK_DGRAM,
	SOCK_SEQPACKET,
};

struct Packet {
	// Sender process information.
	int senderPid;
	unsigned int senderUid;
	unsigned int senderGid;

	struct timeval recvTimestamp;

	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	std::vector<smarter::shared_ptr<File, FileHandle>> files;

	size_t offset = 0;
};

struct OpenFile : FileWithDefaults {
	enum class State {
		null,
		listening,
		connected,
		remoteShutDown,
		closed
	};

	enum class NameType {
		unnamed,
		path,
		abstract
	};

	async::result<void> raceReceiveTimeout(async::cancellation_token c) {
		if(receiveTimeout_) {
			uint64_t ns = (receiveTimeout_->tv_usec * 1'000) + (receiveTimeout_->tv_sec * 1'000'000'000);
			co_await helix::sleepFor(ns, c);
		} else {
			co_await async::suspend_indefinitely(c);
		}
	};

	async::result<void> raceSendTimeout(async::cancellation_token c) {
		if(sendTimeout_) {
			uint64_t ns = (sendTimeout_->tv_usec * 1'000) + (sendTimeout_->tv_sec * 1'000'000'000);
			co_await helix::sleepFor(ns, c);
		} else {
			co_await async::suspend_indefinitely(c);
		}
	};
public:
	static void connectPair(OpenFile *a, OpenFile *b) {
		assert(a->_currentState == State::null);
		assert(b->_currentState == State::null);
		a->_remote = b->weakFile();
		b->_remote = a->weakFile();
		a->_currentState = State::connected;
		b->_currentState = State::connected;
		a->_statusBell.raise();
		b->_statusBell.raise();
	}

	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(Process *process = nullptr, bool nonBlock = false, int32_t socktype = SOCK_STREAM, bool socketpair = false)
	: FileWithDefaults{FileKind::unknown,  StructName::get("un-socket"), nullptr,
		SpecialLink::makeSpecialLink(VfsType::socket, 0777),
			File::defaultPipeLikeSeek}, _currentState{State::null},
			_currentSeq{1}, _inSeq{0}, _ownerPid{0}, _ownerUid{0}, _ownerGid{0},
			_passCreds{false}, nonBlock_{nonBlock},
			_sockpath{}, _nameType{NameType::unnamed}, _isInherited{false}, socktype_{socktype}, socketpair_{socketpair}, listen_{false} {
		if(process) {
			_ownerPid = process->pid();
			_ownerUid = process->threadGroup()->uid();
			_ownerGid = process->threadGroup()->gid();
		}
	}

	void handleClose() override {
		if(logSockets)
			std::cout << "posix: Closing socket \e[1;34m" << structName() << "\e[0m" << std::endl;
		if (!_isInherited && _nameType == NameType::abstract) {
			auto it = abstractSocketsBindMap.find(_sockpath);
			assert(it != abstractSocketsBindMap.end() && it->second == this);
			abstractSocketsBindMap.erase(it);
		}
		if(_bindNode) {
			auto it = globalBindMap.find(_bindNode.get());
			assert(it != globalBindMap.end() && it->second == this);
			globalBindMap.erase(it);
			_bindNode = nullptr;
		}

		if(_currentState == State::connected) {
			// Datagram sockets connect() one-way; only tear down mutual connections.
			if(auto rf = remote(); rf && rf->remote().get() == this) {
				if(logSockets)
					std::cout << "posix: Remote \e[1;34m" << rf->structName() << "\e[0m" << std::endl;
				rf->_currentState = State::remoteShutDown;
				if(socktype_ == SOCK_STREAM) {
					rf->_hupSeq = ++rf->_currentSeq;
					rf->_statusBell.raise();
				}
				rf->_remote = {};
			}
		}
		_remote = {};

		auto connectors = std::move(_acceptQueue);
		_acceptQueue.clear();

		// Packets can carry files; drop them now such that destructing the socket does not
		// dispose other files. They are dropped at the end, since that can close other sockets.
		auto packets = std::move(_recvQueue);
		_recvQueue.clear();

		_currentState = State::closed;

		// Wake up connectors that wait in our accept queue; they fail with ECONNREFUSED.
		// Only after setting the state since a woken connector can resume inline.
		for(auto &weak : connectors) {
			if(auto connector = weak.lock(); connector)
				static_cast<OpenFile *>(connector.get())->_statusBell.raise();
		}
		_statusBell.raise();
		_cancelServe.cancel();
	}

public:
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ct) override {
		auto rf = remote();
		if(socktype_ == SOCK_STREAM && _recvQueue.empty() && (_currentState == State::remoteShutDown
				|| (rf && rf->shutdownFlags_ & shutdownWrite)))
			co_return std::unexpected{Error::eof};
		if(socktype_ == SOCK_STREAM && _currentState != State::connected && _currentState != State::remoteShutDown)
			co_return std::unexpected{Error::notConnected};

		if(logSockets)
			std::cout << "posix: Read from socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		bool queueWaitCancelled = false;

		if(_recvQueue.empty()) {
			if(nonBlock_) {
				if(logSockets)
					std::cout << "posix: UNIX socket would block" << std::endl;
				co_return std::unexpected{Error::wouldBlock};
			}

			if(shutdownFlags_ & shutdownRead)
				co_return std::unexpected{Error::eof};

			if(ct.is_cancellation_requested())
				co_return std::unexpected{Error::interrupted};

			co_await async::race_and_cancel(
				[&](async::cancellation_token c) {
					return raceReceiveTimeout(c);
				},
				async::lambda([&](async::cancellation_token c) -> async::result<void> {
					while (_recvQueue.empty()) {
						if (!co_await _statusBell.async_wait(c)) {
							queueWaitCancelled = true;
							co_return;
						}
					}
				}),
				async::lambda([&](async::cancellation_token c) -> async::result<void> {
					co_await async::suspend_indefinitely(c, ct);
				})
			);
		}

		if(_recvQueue.empty() && queueWaitCancelled)
			co_return std::unexpected{Error::interrupted};
		else if(_recvQueue.empty())
			co_return std::unexpected{Error::wouldBlock}; // timed out

		auto packet = &_recvQueue.front();
		if(socktype_ == SOCK_STREAM) {
			auto chunk = std::min(packet->buffer.size() - packet->offset, max_length);
			memcpy(data, packet->buffer.data() + packet->offset, chunk);
			packet->offset += chunk;
			if(packet->offset == packet->buffer.size())
				_recvQueue.pop_front();
			co_return chunk;
		} else {
			assert(!packet->offset);
			auto size = packet->buffer.size();
			assert(max_length >= size);
			memcpy(data, packet->buffer.data(), size);
			_recvQueue.pop_front();
			co_return size;
		}
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length) override {
		assert(process);

		if(_currentState != State::connected)
			co_return Error::notConnected;
		if(shutdownFlags_ & shutdownWrite) {
			process->issueThreadSignal(SIGPIPE, {});
			co_return Error::brokenPipe;
		}

		if(logSockets)
			std::cout << "posix: Write to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		Packet packet;
		packet.senderPid = process->pid();
		packet.buffer.resize(length);
		memcpy(packet.buffer.data(), data, length);
		packet.offset = 0;
		auto now = clk::getRealtime();
		TIMESPEC_TO_TIMEVAL(&packet.recvTimestamp, &now);

		auto rf = remote();
		if(!rf || rf->_currentState == State::closed)
			co_return Error::notConnected;
		rf->_recvQueue.push_back(std::move(packet));
		rf->_inSeq = ++rf->_currentSeq;
		rf->_statusBell.raise();
		co_return length;
	}

	async::result<protocols::fs::RecvResult>
	recvMsg(Process *process, uint32_t flags, void *data, size_t max_length,
			void *, size_t, size_t max_ctrl_length) override {
		if((flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_NOSIGNAL | MSG_TRUNC | MSG_PEEK))) {
			std::cout << "posix: Unimplemented flag in un-socket " << std::hex << flags << std::dec << " for pid: " << process->pid() << std::endl;
		}

		if(socktype_ == SOCK_STREAM && _currentState != State::connected && _currentState != State::remoteShutDown)
			co_return protocols::fs::Error::notConnected;

		if(socktype_ == SOCK_STREAM && _recvQueue.empty() && _currentState == State::remoteShutDown)
			co_return protocols::fs::RecvData{{}, 0, 0, 0};

		if(logSockets)
			std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		if(_recvQueue.empty() && ((flags & MSG_DONTWAIT) || nonBlock_)) {
			if(logSockets)
				std::cout << "posix: UNIX socket would block" << std::endl;
			co_return protocols::fs::Error::wouldBlock;
		}

		if(_recvQueue.empty() && shutdownFlags_ & shutdownRead)
			co_return protocols::fs::RecvData{{}, 0, 0, 0};

		co_await async::race_and_cancel(
			[&](async::cancellation_token c) { return raceReceiveTimeout(c); },
			[&](async::cancellation_token c) -> async::result<void> {
				while (_recvQueue.empty() && !c.is_cancellation_requested())
					co_await _statusBell.async_wait(c);
			}
		);

		if(_recvQueue.empty())
			co_return protocols::fs::Error::wouldBlock;

		auto packet = &_recvQueue.front();
		uint32_t reply_flags = 0;
		size_t returned_length = 0;

		protocols::fs::CtrlBuilder ctrl{max_ctrl_length};

		if(_passCreds) {
			struct ucred creds;
			memset(&creds, 0, sizeof(struct ucred));
			creds.pid = packet->senderPid;
			creds.uid = packet->senderUid;
			creds.gid = packet->senderGid;

			auto truncated = ctrl.message(SOL_SOCKET, SCM_CREDENTIALS, sizeof(struct ucred));
			if(truncated)
				reply_flags |= MSG_CTRUNC;
			else
				ctrl.write(creds);
		}

		if(timestamp_) {
			auto truncated = ctrl.message(SOL_SOCKET, SCM_TIMESTAMP, sizeof(struct timeval));
			if(!truncated)
				ctrl.write(packet->recvTimestamp);
		}

		if(!packet->files.empty() && !packet->offset) {
			auto [truncated, payload_len] = ctrl.message_truncated(SOL_SOCKET, SCM_RIGHTS, sizeof(int) * packet->files.size(), sizeof(int));
			assert(!(payload_len % sizeof(int)));
			for(auto &file : packet->files) {
				if(truncated && payload_len < sizeof(int))
					break;

				ctrl.write<int>(
					process->fileContext()->attachFile(std::move(file), flags & MSG_CMSG_CLOEXEC)
					.value_or(-1));

				if(truncated)
					payload_len -= sizeof(int);
			}

			if(truncated)
				reply_flags |= MSG_CTRUNC;

			if(!(flags & MSG_PEEK))
				packet->files.clear();
		}

		// datagram packets are always read from their beginning, so offsets are illegal
		assert(!packet->offset || socktype_ == SOCK_STREAM);
		auto data_length = packet->buffer.size() - packet->offset;
		auto chunk = std::min(packet->buffer.size() - packet->offset, max_length);
		memcpy(data, packet->buffer.data() + packet->offset, chunk);

		if(socktype_ == SOCK_STREAM) {
			returned_length = chunk;
			if(!(flags & MSG_PEEK)) {
				packet->offset += chunk;
				if(packet->offset == packet->buffer.size())
					_recvQueue.pop_front();
			}
		} else {
			returned_length = (flags & MSG_TRUNC) ? data_length : chunk;
			if(!(flags & MSG_PEEK))
				_recvQueue.pop_front();
		}

		if(data_length != returned_length && socktype_ != SOCK_STREAM)
			reply_flags |= MSG_TRUNC;

		co_return protocols::fs::RecvData{ctrl.buffer(), returned_length, 0, reply_flags};
	}

	async::result<frg::expected<protocols::fs::Error, size_t>>
	sendMsg(Process *process, uint32_t flags, const void *data, size_t max_length,
			const void *addr_ptr, size_t addr_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files, struct ucred ucreds) override {
		smarter::shared_ptr<OpenFile> remote;
		assert(!(flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)));

		if(shutdownFlags_ & shutdownWrite) {
			if(!(flags & MSG_NOSIGNAL))
				process->issueThreadSignal(SIGPIPE, {});
			co_return protocols::fs::Error::brokenPipe;
		}

		if(socktype_ == SOCK_STREAM || socktype_ == SOCK_SEQPACKET) {
			if(addr_length != 0 && _currentState == State::connected) {
				co_return protocols::fs::Error::alreadyConnected;
			}

			if(_currentState == State::remoteShutDown)
				co_return protocols::fs::Error::brokenPipe;

			if(_currentState != State::connected)
				co_return protocols::fs::Error::notConnected;

			remote = this->remote();
			if(!remote)
				co_return protocols::fs::Error::notConnected;
		} else if(socktype_ == SOCK_DGRAM) {
			if(addr_length == 0) {
				remote = this->remote();
				if(!remote) {
					// While returning ENOTCONN is not what the man page makes you believe
					// should happen here, this is what Linux does. Linux does not return
					// EDESTADDRREQ for unix sockets, even datagram ones. Make it make sense.
					// A peer that was destructed since we connected refuses, like a closed one.
					if(_currentState == State::connected)
						co_return protocols::fs::Error::connectionRefused;
					co_return protocols::fs::Error::notConnected;
				}
			} else {
				struct sockaddr_un sa;
				assert(addr_length <= sizeof(struct sockaddr_un));
				memcpy(&sa, addr_ptr, addr_length);

				std::string path;

				if(addr_length <= offsetof(struct sockaddr_un, sun_path)) {
					co_return protocols::fs::Error::illegalArguments;
				} else if(sa.sun_path[0] == '\0') {
					path.resize(addr_length - sizeof(sa.sun_family) - 1);
					memcpy(path.data(), sa.sun_path + 1, addr_length - sizeof(sa.sun_family) - 1);

					auto it = abstractSocketsBindMap.find(path);
					if(it == abstractSocketsBindMap.end())
						co_return protocols::fs::Error::connectionRefused;
					remote = it->second->sharedFromThis();
				} else {
					path.resize(strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
					memcpy(path.data(), sa.sun_path, strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
					PathResolver resolver;
					resolver.setup(process->fsContext()->getRoot(),
							process->fsContext()->getWorkingDirectory(), std::move(path), process);
					auto resolveResult = co_await resolver.resolve();
					if(!resolveResult) {
						co_return resolveResult.error();
					}
					assert(resolveResult);
					if(!resolver.currentLink())
						co_return protocols::fs::Error::fileNotFound;

					// Lookup the socket associated with the node.
					auto node = resolver.currentLink()->getTarget();
					auto it = globalBindMap.find(node.get());
					if(it == globalBindMap.end())
						co_return protocols::fs::Error::connectionRefused;
					remote = it->second->sharedFromThis();
				}

			}
		}

		if(logSockets)
			std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		// The peer may have been closed since we connected to it.
		if(remote->_currentState == State::closed)
			co_return protocols::fs::Error::connectionRefused;

		protocols::fs::utils::handleSoPasscred(remote->_passCreds, ucreds, process->pid(), process->threadGroup()->uid(), process->threadGroup()->gid());

		// We ignore MSG_DONTWAIT here as we never block anyway.

		// TODO: Add permission checking for ucred related items
		Packet packet;
		packet.senderPid = ucreds.pid;
		packet.senderUid = ucreds.uid;
		packet.senderGid = ucreds.gid;
		packet.buffer.resize(max_length);
		memcpy(packet.buffer.data(), data, max_length);
		packet.files = std::move(files);
		packet.offset = 0;
		auto now = clk::getRealtime();
		TIMESPEC_TO_TIMEVAL(&packet.recvTimestamp, &now);

		remote->_recvQueue.push_back(std::move(packet));
		remote->_inSeq = ++remote->_currentSeq;
		remote->_statusBell.raise();

		co_return max_length;
	}

	async::result<protocols::fs::Error> listen() override {
		listen_ = true;
		co_return protocols::fs::Error::none;
	}

	async::result<frg::expected<Error, AcceptResult>> accept(Process *process) override {
		smarter::shared_ptr<OpenFile> remote;
		while(!remote) {
			if(_acceptQueue.empty() && nonBlock_) {
				if(logSockets)
					std::cout << "posix: UNIX socket would block on accept" << std::endl;
				co_return Error::wouldBlock;
			}

			co_await async::race_and_cancel(
				[&](async::cancellation_token c) { return raceReceiveTimeout(c); },
				[&](async::cancellation_token c) -> async::result<void> {
					while (_acceptQueue.empty() && !c.is_cancellation_requested())
						co_await _statusBell.async_wait(c);
				}
			);

			if(_acceptQueue.empty())
				co_return Error::wouldBlock;

			// Connectors leave the queue when they give up, but they might be closed already.
			auto file = _acceptQueue.front().lock();
			_acceptQueue.pop_front();
			if(!file || static_cast<OpenFile *>(file.get())->_currentState != State::null)
				continue;
			remote = smarter::static_pointer_cast<OpenFile>(std::move(file));
		}

		// Create a new socket and connect it to the queued one.
		auto local = smarter::make_shared<OpenFile>(process);
		local->_sockpath = _sockpath;
		local->_nameType = _nameType;
		local->_isInherited = true;
		local->setupWeakFile(local);
		OpenFile::serve(local);
		connectPair(remote.get(), local.get());
		co_return File::constructHandle(std::move(local));
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t past_seq, int mask,
			async::cancellation_token cancellation) override {
		assert(past_seq <= _currentSeq);
		int edges = 0;

		while(true) {
			if (_currentState == State::closed)
				co_return Error::fileClosed;

			// For now making sockets always writable is sufficient.
			edges = EPOLLOUT;
			if (socktype_ == SOCK_STREAM || socktype_ == SOCK_SEQPACKET) {
				if (_hupSeq > past_seq)
					edges |= EPOLLHUP | EPOLLIN;
			}

			if (_inSeq > past_seq)
				edges |= EPOLLIN;

			if (shutdownFlags_ & shutdownRead)
				edges |= EPOLLRDHUP;

			if (edges & mask)
				break;

			if (!co_await _statusBell.async_wait(cancellation))
				break;
		}

		// std::println("posix: pollWait({}, {}) on \e[1;34m{}\e[0m returns ({}, {})",
		// 	past_seq, mask, structName(), _currentSeq, edges & mask);

		co_return PollWaitResult(_currentSeq, edges & mask);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		int events = EPOLLOUT;
		if(socktype_ == SOCK_STREAM || socktype_ == SOCK_SEQPACKET) {
			if(_currentState == State::remoteShutDown)
				events |= EPOLLHUP | EPOLLIN;
		}
		if(!_acceptQueue.empty() || !_recvQueue.empty())
			events |= EPOLLIN;
		if(shutdownFlags_ & shutdownRead)
			events |= EPOLLRDHUP;

		co_return PollStatusResult{_currentSeq, events};
	}

	async::result<protocols::fs::Error>
	bind(Process *process, const void *addr_ptr, size_t addr_length) override {
		if(addr_length <= offsetof(struct sockaddr_un, sun_path)) {
			co_return protocols::fs::Error::illegalArguments;
		}

		// Like Linux, sockets can only be bound once (accepted sockets inherit their name).
		// handleClose() relies on this to remove the bind map entries; it has to run after us.
		if(!isOpen() || _nameType != NameType::unnamed)
			co_return protocols::fs::Error::illegalArguments;

		// Create a new socket node in the FS.
		struct sockaddr_un sa;
		assert(addr_length <= sizeof(struct sockaddr_un));
		memcpy(&sa, addr_ptr, addr_length);
		std::string path;
		NameType nameType;

		if(sa.sun_path[0] == '\0') {
			path.resize(addr_length - sizeof(sa.sun_family) - 1);
			memcpy(path.data(), sa.sun_path + 1, addr_length - sizeof(sa.sun_family) - 1);
			nameType = NameType::abstract;
		} else {
			path.resize(strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
			memcpy(path.data(), sa.sun_path, strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
			nameType = NameType::path;
		}
		if(logSockets)
			std::cout << "posix: Bind to " << path << std::endl;

		if (nameType == NameType::abstract) {
			auto res = abstractSocketsBindMap.emplace(path, this);
			if(!res.second)
				co_return protocols::fs::Error::addressInUse;
			_nameType = nameType;
			_sockpath = std::move(path);
			co_return protocols::fs::Error::none;
		} else {
			auto sockpath = path;
			PathResolver resolver;
			resolver.setup(process->fsContext()->getRoot(),
					process->fsContext()->getWorkingDirectory(), std::move(path), process);
			auto resolveResult = co_await resolver.resolve(resolvePrefix | resolveCreatesNonDirectory);
			if(!resolveResult) {
				co_return resolveResult.error();
			}
			assert(resolveResult);
			if(!resolver.currentLink())
				co_return protocols::fs::Error::fileNotFound;

			auto parentLink = resolver.currentLink();
			auto nodeResult = co_await parentLink->getTarget()->mksocket(parentLink.get(),
					resolver.nextComponent(), (0666 & ~process->fsContext()->getUmask()), process->threadGroup()->uid(), process->threadGroup()->gid());
			if(!nodeResult) {
				co_return nodeResult.error() | protocols::fs::toFsProtoError;
			}
			assert(nodeResult);
			auto node = nodeResult.value();
			// A concurrent bind() or close of this socket may have completed while we suspended.
			if(!isOpen() || _nameType != NameType::unnamed)
				co_return protocols::fs::Error::illegalArguments;
			// Associate the current socket with the node.
			auto res = globalBindMap.insert({node->getTarget().get(), this});
			if(!res.second)
				co_return protocols::fs::Error::addressInUse;
			_bindNode = node->getTarget();
			_nameType = nameType;
			_sockpath = std::move(sockpath);
			co_return protocols::fs::Error::none;
		}
	}

	async::result<protocols::fs::Error>
	connect(Process *process, const void *addr_ptr, size_t addr_length) override {
		if (addr_length > sizeof(struct sockaddr_un))
			co_return protocols::fs::Error::illegalArguments;

		// Resolve the socket node in the FS.
		struct sockaddr_un sa;
		memcpy(&sa, addr_ptr, addr_length);
		std::string path;

		if (sa.sun_family == AF_UNSPEC) {
			// POSIX 1003.1-2024: If the sa_family member of address is AF_UNSPEC,
			// the socket's peer address shall be reset.
			_currentState = State::null;
			_remote = {};
			co_return protocols::fs::Error::none;
		} else if(sa.sun_family != AF_UNIX) {
			co_return protocols::fs::Error::illegalArguments;
		}

		if(addr_length <= offsetof(struct sockaddr_un, sun_path)) {
			co_return protocols::fs::Error::illegalArguments;
		} else if(sa.sun_path[0] == '\0') {
			path.resize(addr_length - sizeof(sa.sun_family) - 1);
			memcpy(path.data(), sa.sun_path + 1, addr_length - sizeof(sa.sun_family) - 1);
		} else {
			path.resize(strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
			memcpy(path.data(), sa.sun_path, strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
		}

		if(logSockets)
			std::cout << "posix: Connect to " << path << std::endl;

		// Like Linux, connected stream sockets cannot be connected again, even after the peer closed.
		if(socktype_ != SOCK_DGRAM && (_currentState == State::connected
				|| _currentState == State::remoteShutDown))
			co_return protocols::fs::Error::alreadyConnected;

		if (sa.sun_path[0] == '\0') {
			_ownerPid = process->pid();
			_ownerUid = process->threadGroup()->uid();
			_ownerGid = process->threadGroup()->gid();

			if(!abstractSocketsBindMap.contains(path))
				co_return protocols::fs::Error::connectionRefused;
			auto server = abstractSocketsBindMap.at(path);
			co_return co_await _connectTo(server->sharedFromThis());
		} else {
			PathResolver resolver;
			resolver.setup(process->fsContext()->getRoot(),
					process->fsContext()->getWorkingDirectory(), std::move(path), process);
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				co_return resolveResult.error();
			}
			assert(resolveResult);
			if(!resolver.currentLink())
				co_return protocols::fs::Error::fileNotFound;

			_ownerPid = process->pid();
			_ownerUid = process->threadGroup()->uid();
			_ownerGid = process->threadGroup()->gid();

			// Lookup the socket associated with the node.
			auto node = resolver.currentLink()->getTarget();
			auto it = globalBindMap.find(node.get());
			if(it == globalBindMap.end())
				co_return protocols::fs::Error::connectionRefused;
			auto server = it->second;
			co_return co_await _connectTo(server->sharedFromThis());
		}
	}

private:
	// Connects to a bound socket.
	async::result<protocols::fs::Error> _connectTo(smarter::shared_ptr<OpenFile> server) {
		if(socktype_ == SOCK_DGRAM) {
			_remote = server->weakFile();
			// POSIX 1003.1-2024: Note that despite no connection being made, the term "connected"
			// is used to describe a connectionless-mode socket for which a peer address has been set.
			_currentState = State::connected;

			// connect(2)ing on a datagram socket just sets the address to which datagrams
			// are sent by default; the remote remains untouched.
			co_return protocols::fs::Error::none;
		}

		server->_acceptQueue.push_back(weakFile());
		server->_inSeq = ++server->_currentSeq;
		server->_statusBell.raise();

		co_await async::race_and_cancel(
			async::lambda([&](async::cancellation_token c) { return raceSendTimeout(c); }),
			async::lambda([&](async::cancellation_token c) -> async::result<void> {
				while (_currentState == State::null && server->_currentState != State::closed
						&& !c.is_cancellation_requested())
					co_await _statusBell.async_wait(c);
			})
		);

		if(_currentState == State::connected)
			co_return protocols::fs::Error::none;

		// Leave the accept queue unless accept() already removed us.
		std::erase_if(server->_acceptQueue, [&] (const smarter::weak_ptr<File> &weak) {
			return weak.lock().get() == this;
		});
		if(server->_currentState == State::closed)
			co_return protocols::fs::Error::connectionRefused;
		co_return protocols::fs::Error::wouldBlock;
	}

	// Returns the peer, or null if there is none.
	smarter::shared_ptr<OpenFile> remote() {
		auto file = _remote.lock();
		if(!file)
			return nullptr;
		return smarter::static_pointer_cast<OpenFile>(std::move(file));
	}

	smarter::shared_ptr<OpenFile> sharedFromThis() {
		return smarter::static_pointer_cast<OpenFile>(weakFile().lock());
	}

public:

	async::result<protocols::fs::Error> shutdown(int how) override {
		if(_currentState != State::connected)
			co_return protocols::fs::Error::notConnected;

		if(how == SHUT_RD) {
			shutdownFlags_ |= shutdownRead;
		} else if(how == SHUT_WR) {
			shutdownFlags_ |= shutdownWrite;
		} else if(how == SHUT_RDWR) {
			shutdownFlags_ |= shutdownRead | shutdownWrite;
		} else {
			std::println("posix: unexpected how={} for un-socket shutdown", how);
			co_return protocols::fs::Error::illegalArguments;
		}

		_statusBell.raise();

		co_return protocols::fs::Error::none;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<void> setFileFlags(int flags) override {
		if(flags & ~(O_NONBLOCK | O_RDONLY | O_WRONLY | O_RDWR)) {
			std::cout << std::format("posix: setFileFlags on socket \e[1;34m{}\e[0m called with unknown flags {:x}\n",
					structName(), flags & ~O_NONBLOCK);
			co_return;
		}
		if(flags & O_NONBLOCK)
			nonBlock_ = true;
		else
			nonBlock_ = false;
		co_return;
	}

	async::result<int> getFileFlags() override {
		int flags = O_RDWR;
		if(nonBlock_)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	async::result<frg::expected<protocols::fs::Error>> getSocketOption(Process *process,
			int layer, int number, std::vector<char> &optbuf) override {
		if(layer == SOL_SOCKET && number == SO_PROTOCOL) {
			int protocol = 0;
			memcpy(optbuf.data(), &protocol, std::min(optbuf.size(), sizeof(protocol)));
		} else if(layer == SOL_SOCKET && number == SO_DOMAIN) {
			int domain = AF_UNIX;
			memcpy(optbuf.data(), &domain, std::min(optbuf.size(), sizeof(domain)));
		} else if(layer == SOL_SOCKET && number == SO_PEERCRED) {
			struct ucred creds;

			// man page:
			// "The use of this option is possible only for connected AF_UNIX stream sockets
			// and for AF_UNIX stream and datagram socket pairs created using socketpair(2)."
			auto rf = remote();
			if(rf && ((_currentState == State::connected && socktype_ == SOCK_STREAM) || socketpair_)) {
				creds.pid = rf->_ownerPid;
				creds.uid = rf->_ownerUid;
				creds.gid = rf->_ownerGid;
			} else {
				creds.pid = 0;
				creds.uid = -1;
				creds.gid = -1;
			}

			memcpy(optbuf.data(), &creds, std::min(optbuf.size(), sizeof(creds)));
		} else if(layer == SOL_SOCKET && number == SO_TYPE) {
			int type = socktype_;
			memcpy(optbuf.data(), &type, std::min(optbuf.size(), sizeof(type)));
		} else if(layer == SOL_SOCKET && number == SO_ACCEPTCONN) {
			int listen = listen_;
			memcpy(optbuf.data(), &listen, std::min(optbuf.size(), sizeof(listen)));
		} else if(layer == SOL_SOCKET && number == SO_PEERPIDFD) {
			auto rf = remote();
			pid_t pid = rf ? rf->_ownerPid : 0;
			int result = 0;

			if(!pid) {
				result = -ENODATA;
			} else {
				auto remoteProc = ThreadGroup::findThreadGroup(pid);

				if(remoteProc) {
					auto pidfd = createPidfdFile(remoteProc->weak_from_this(), false);
					result = process->fileContext()->attachFile(pidfd).value_or(-EMFILE);
				} else {
					result = -ENODATA;
				}
			}

			memcpy(optbuf.data(), &result, std::min(optbuf.size(), sizeof(result)));
		} else {
			printf("posix un-socket: unhandled getsockopt layer %d number %d\n", layer, number);
			co_return protocols::fs::Error::invalidProtocolOption;
		}

		co_return {};
	}

	async::result<frg::expected<protocols::fs::Error>> setSocketOption(int layer,
			int number, std::vector<char> optbuf) override {
		if(layer == SOL_SOCKET && number == SO_PASSCRED) {
			if(optbuf.size() >= sizeof(int))
				_passCreds = *reinterpret_cast<int *>(optbuf.data());
		} else if(layer == SOL_SOCKET && number == SO_TIMESTAMP) {
			if(optbuf.size() != sizeof(int))
				co_return protocols::fs::Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			timestamp_ = (val != 0);
		} else if(layer == SOL_SOCKET && number == SO_RCVTIMEO) {
			if(optbuf.size() < sizeof(timeval))
				co_return protocols::fs::Error::illegalArguments;

			receiveTimeout_ = *reinterpret_cast<timeval *>(optbuf.data());

			if(!receiveTimeout_->tv_sec && !receiveTimeout_->tv_usec)
				receiveTimeout_ = std::nullopt;
		} else if(layer == SOL_SOCKET && number == SO_SNDTIMEO) {
			if(optbuf.size() < sizeof(timeval))
				co_return protocols::fs::Error::illegalArguments;

			sendTimeout_ = *reinterpret_cast<timeval *>(optbuf.data());

			if(!sendTimeout_->tv_sec && !sendTimeout_->tv_usec)
				sendTimeout_ = std::nullopt;
		} else {
			std::cout << std::format("un-socket: unknown setsockopt 0x{:x}\n", number);
			co_return protocols::fs::Error::illegalArguments;
		}

		co_return {};
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, OpenFile *self) {
			managarm::fs::GenericIoctlReply resp;

			switch(req.command()) {
				case FIONREAD: {
					resp.set_error(managarm::fs::Errors::SUCCESS);

					if(self->_currentState != State::connected) {
						resp.set_error(managarm::fs::Errors::NOT_CONNECTED);
					} else if(self->_recvQueue.empty()) {
						resp.set_fionread_count(0);
					} else {
						auto packet = &self->_recvQueue.front();
						resp.set_fionread_count(packet->buffer.size() - packet->offset);
					}
					break;
				}
				default: {
					std::cout << "posix: invalid ioctl 0x"
						<< std::hex << req.command() << std::dec
						<< " for un-socket" << std::endl;

					auto [dismissResult] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::dismiss()
					);
					HEL_CHECK(dismissResult.error());
					co_return {};
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

	async::result<void>
	ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override {
		auto res = co_await dispatchRequest<
			managarm::fs::GenericIoctlRequest
		>(conversation, std::move(msg), HandleIoctl{}, this);

		if (!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

private:
	static size_t getNameFor(OpenFile *sock, void *addrPtr, size_t maxAddrLength) {
		sockaddr_un sa;
		size_t outSize = offsetof(sockaddr_un, sun_path) + sock->_sockpath.size() + 1;

		memset(&sa, 0, sizeof(struct sockaddr_un));
		sa.sun_family = AF_UNIX;

		switch (sock->_nameType) {
			case NameType::unnamed:
				outSize = sizeof(sa_family_t);
				break;
			case NameType::abstract:
				sa.sun_path[0] = '\0';
				memcpy(sa.sun_path + 1, sock->_sockpath.data(),
						std::min(sizeof(sa.sun_path) - 1, sock->_sockpath.size()));
				break;
			case NameType::path:
				strncpy(sa.sun_path, sock->_sockpath.data(), sizeof(sa.sun_path));
				break;
		}

		auto destSize = std::min(sizeof(sockaddr_un), maxAddrLength);
		memcpy(addrPtr, &sa, destSize);

		return outSize;
	}

public:
	async::result<frg::expected<protocols::fs::Error, size_t>>
	peername(void *addrPtr, size_t maxAddrLength) override {
		if (_currentState != State::connected) {
			co_return protocols::fs::Error::notConnected;
		}

		auto rf = remote();
		if (!rf)
			co_return protocols::fs::Error::notConnected;
		co_return getNameFor(rf.get(), addrPtr, maxAddrLength);
	}

	async::result<size_t> sockname(void *addrPtr, size_t maxAddrLength) override {
		co_return getNameFor(this, addrPtr, maxAddrLength);
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	State _currentState;

	// Status management for poll().
	async::recurring_event _statusBell;
	uint64_t _currentSeq;
	uint64_t _hupSeq = 0;
	uint64_t _inSeq;

	// Sockets whose connect() waits for us to accept() them.
	std::deque<smarter::weak_ptr<File>> _acceptQueue;

	// The actual receive queue of the socket.
	std::deque<Packet> _recvQueue;

	int _ownerPid;
	int _ownerUid;
	int _ownerGid;

	// For connected sockets, this is the socket we are connected to.
	smarter::weak_ptr<File> _remote;

	// Socket options.
	bool _passCreds;
	bool timestamp_ = false;
	std::atomic<bool> nonBlock_;

	std::string _sockpath;

	// Node that we are bound to, keeps our globalBindMap key alive.
	smarter::shared_ptr<FsNode> _bindNode;

	NameType _nameType;

	bool _isInherited;

	int32_t socktype_;

	bool socketpair_;

	bool listen_;

	std::optional<timeval> receiveTimeout_;
	std::optional<timeval> sendTimeout_;

	int shutdownFlags_ = 0;
};

std::expected<smarter::shared_ptr<File, FileHandle>, Error>
createSocketFile(bool nonBlock, int32_t socktype) {
	if(std::ranges::find(supportedSocketTypes, socktype) == std::end(supportedSocketTypes)) [[unlikely]]
		return std::unexpected{Error::unsupportedSocketType};

	auto file = smarter::make_shared<OpenFile>(nullptr, nonBlock, socktype);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair(Process *process, bool nonBlock, int32_t socktype) {
	auto file0 = smarter::make_shared<OpenFile>(process, nonBlock, socktype, true);
	auto file1 = smarter::make_shared<OpenFile>(process, nonBlock, socktype, true);
	file0->setupWeakFile(file0);
	file1->setupWeakFile(file1);
	OpenFile::serve(file0);
	OpenFile::serve(file1);
	OpenFile::connectPair(file0.get(), file1.get());
	return {File::constructHandle(std::move(file0)), File::constructHandle(std::move(file1))};
}

} // namespace un_socket

