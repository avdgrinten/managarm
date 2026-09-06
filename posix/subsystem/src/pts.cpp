#include <asm/ioctls.h>
#include <async/cancellation.hpp>
#include <linux/magic.h>
#include <limits.h>
#include <numeric>
#include <termios.h>
#include <sys/epoll.h>
#include <signal.h>
#include <print>

#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>

#include <core/dispatch.hpp>
#include "core/tty.hpp"
#include "file.hpp"
#include "process.hpp"
#include "pts.hpp"
#include "fs.bragi.hpp"

#include <bitset>

namespace pts {

namespace {

struct MasterFile;
struct RootLink;
struct DeviceNode;
struct RootNode;

bool logReadWrite = false;
bool logAttrs = false;

int nextPtsIndex = 0;

extern smarter::shared_ptr<RootLink, LinkRc> globalRootLink;

//-----------------------------------------------------------------------------

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	size_t offset = 0;
};

struct Channel {
	Channel(int pts_index)
	: ptsIndex{pts_index}, currentSeq{1}, masterInSeq{0}, slaveInSeq{0} {
		memset(&activeSettings, 0, sizeof(struct termios));
		// cflag: Linux also stores a baud rate here.
		// lflag: Linux additionally sets ECHOCTL, ECHOKE (which we do not have).
		activeSettings.c_iflag = ICRNL | IXON;
		activeSettings.c_oflag = OPOST | ONLCR;
		activeSettings.c_cflag = CS8 | CREAD | HUPCL;
		activeSettings.c_lflag = TTYDEF_LFLAG | ECHOK;
		activeSettings.c_cc[VINTR] = CINTR;
		activeSettings.c_cc[VEOF] = CEOF;
		activeSettings.c_cc[VKILL] = CKILL;
		activeSettings.c_cc[VSTART] = CSTART;
		activeSettings.c_cc[VSTOP] = CSTOP;
		activeSettings.c_cc[VSUSP] = CSUSP;
		activeSettings.c_cc[VQUIT] = CQUIT;
		activeSettings.c_cc[VERASE] = CERASE; // DEL character.
		activeSettings.c_cc[VMIN] = CMIN;
		activeSettings.c_cc[VDISCARD] = CDISCARD;
		activeSettings.c_cc[VLNEXT] = CLNEXT;
		activeSettings.c_cc[VWERASE] = CWERASE;
		activeSettings.c_cc[VREPRINT] = CRPRNT;
		cfsetispeed(&activeSettings, B38400);
		cfsetospeed(&activeSettings, B38400);
	}

	async::result<void> commonIoctl(managarm::fs::GenericIoctlRequest req, helix::BorrowedDescriptor conversation);

	int ptsIndex;
	ControllingTerminalState cts;

	struct termios activeSettings;

	int width = 80;
	int height = 25;
	int pixelWidth = 8 * 80;
	int pixelHeight = 16 * 25;

	// Status management for poll().
	async::recurring_event statusBell;
	uint64_t currentSeq;
	uint64_t masterInSeq;
	uint64_t slaveInSeq;

	// The actual queue of this pipe.
	std::deque<Packet> masterQueue;
	std::deque<Packet> slaveQueue;

	size_t masterCount = 0;
	size_t slaveCount = 0;
};

namespace {

void processOut(const char c, Packet &packet, std::shared_ptr<Channel> channel) {
	if(!(channel->activeSettings.c_oflag & OPOST)) {
		packet.buffer.push_back(c);
		return;
	}

	if((channel->activeSettings.c_oflag & ONLCR) && c == '\n') {
		packet.buffer.push_back('\r');
		packet.buffer.push_back('\n');
		return;
	}

	packet.buffer.push_back(c);

	return;
}

void processIn(const char character, Packet &packet, std::shared_ptr<Channel> channel) {
	auto enqueuePacket = [&channel](Packet packet) {
		channel->slaveQueue.push_back(std::move(packet));
		channel->slaveInSeq = ++channel->currentSeq;
		channel->statusBell.raise();
	};

	auto enqueueOut = [&channel](Packet packet) {
		Packet parsed{};

		for(auto c : packet.buffer) {
			processOut(c, parsed, channel);
		}

		channel->masterQueue.push_back(std::move(parsed));
		channel->masterInSeq = ++channel->currentSeq;
		channel->statusBell.raise();
	};

	auto is_control_char = [](char c) -> bool {
		return c < 32 || c == 0x7F;
	};

	auto erase_char = [&](bool erase) {
		if(!packet.buffer.empty()) {
			size_t chars = 1;
			char c = packet.buffer.back();
			packet.buffer.pop_back();

			if(is_control_char(c))
				chars = 2;

			if((channel->activeSettings.c_lflag & ECHO) && erase) {
				Packet echopacket;
				for(size_t i = 0; i < chars; i++) {
					echopacket.buffer.push_back('\b');
					echopacket.buffer.push_back(' ');
					echopacket.buffer.push_back('\b');
				}
				enqueueOut(std::move(echopacket));
			}
		}
	};

	char c = character;

	if(channel->activeSettings.c_iflag & ISTRIP)
		c &= 0x7F;

	if(c == '\r') {
		if(channel->activeSettings.c_iflag & IGNCR)
			return;

		if(channel->activeSettings.c_iflag & ICRNL)
			c = '\n';
	} else if(c == '\n') {
		if(channel->activeSettings.c_iflag & INLCR)
			c = '\r';
	}

	if((channel->activeSettings.c_iflag & IUCLC) && (c >= 'A' && c <= 'Z'))
		c = c - 'A' + 'a';

	if(channel->activeSettings.c_lflag & ISIG) {
		std::optional<int> signal = {};

		if(c == static_cast<char>(channel->activeSettings.c_cc[VINTR])) {
			signal = SIGINT;
		} else if(c == static_cast<char>(channel->activeSettings.c_cc[VQUIT])) {
			signal = SIGQUIT;
		} else if(c == static_cast<char>(channel->activeSettings.c_cc[VSUSP])) {
			signal = SIGTSTP;
		}

		if(signal.has_value()) {
			UserSignal info;
			channel->cts.issueSignalToForegroundGroup(signal.value(), info);
			return;
		}
	}

	if(channel->activeSettings.c_lflag & ICANON) {
		if(c == static_cast<char>(channel->activeSettings.c_cc[VKILL])) {
			while(!packet.buffer.empty()) {
				erase_char(channel->activeSettings.c_lflag & ECHOK);
			}

			return;
		}

		if(c == static_cast<char>(channel->activeSettings.c_cc[VERASE])) {
			erase_char(channel->activeSettings.c_lflag & ECHOE);
			return;
		}

		if((channel->activeSettings.c_lflag & IEXTEN) &&
		c == static_cast<char>(channel->activeSettings.c_cc[VWERASE])) {
			// remove trailing whitespace
			while(!packet.buffer.empty() && packet.buffer.back() == ' ') {
				erase_char(channel->activeSettings.c_lflag & ECHOE);
			}

			// remove last word
			while(!packet.buffer.empty() && packet.buffer.back() != ' ') {
				erase_char(channel->activeSettings.c_lflag & ECHOE);
			}

			return;
		}

		if(c == static_cast<char>(channel->activeSettings.c_cc[VEOF])) {
			enqueuePacket(std::move(packet));
			packet = Packet{};

			return;
		}
	}

	char echo_char = (channel->activeSettings.c_lflag & ECHO) ? c : '\0';

	if((channel->activeSettings.c_lflag & ECHOCTL) && (channel->activeSettings.c_lflag & ECHO)
		&& c < 32 && c != '\n' && c != '\t') {
		Packet echopacket;
		echopacket.buffer.push_back('^');
		echopacket.buffer.push_back(c + 0x40);
		enqueueOut(std::move(echopacket));
		echo_char = '\0';
	}

	if(channel->activeSettings.c_lflag & ICANON) {
		packet.buffer.push_back(c);

		if(echo_char) {
			Packet echopacket;
			if(is_control_char(c) && c != '\n') {
				echopacket.buffer.push_back('^');
				echopacket.buffer.push_back(('@' + c) % 128);
			} else {
				echopacket.buffer.push_back(c);
			}
			enqueueOut(std::move(echopacket));
		}

		if(c == '\n'
		|| c == static_cast<char>(channel->activeSettings.c_cc[VEOL])
		|| c == static_cast<char>(channel->activeSettings.c_cc[VEOL2])) {
			if(!(channel->activeSettings.c_lflag & ECHO) && (channel->activeSettings.c_lflag & ECHONL)) {
				Packet echopacket;
				echopacket.buffer.push_back(c);
				enqueueOut(std::move(echopacket));
			}
			enqueuePacket(std::move(packet));
			packet = Packet{};
			return;
		}

		return;
	} else if(channel->activeSettings.c_lflag & ECHO) {
		Packet echopacket;
		echopacket.buffer.push_back(c);
		enqueueOut(std::move(echopacket));
	}

	// Not a special character. Emit to the slave.
	packet.buffer.push_back(c);

	return;
}

struct PtsSuperblock final : FsSuperblock {
public:
	PtsSuperblock() {
		deviceMinor_ = getUnnamedDeviceIdAllocator().allocate();
	}

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *) override {
		std::cout << "posix: createRegular on PtsSuperblock unsupported" << std::endl;
		co_return nullptr;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
			rename(FsLink *, FsLink *, std::string) override {
		co_return Error::noSuchFile;
	}

	async::result<frg::expected<Error, FsStats>> getFsStats() override {
		FsStats stats{};
		stats.fsType = DEVPTS_SUPER_MAGIC;
		co_return stats;
	}

	std::string getFsType() override {
		return "devpts";
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

private:
	unsigned int deviceMinor_;
};

PtsSuperblock ptsSuperblock{};

} // namespace

//-----------------------------------------------------------------------------
// Device and file structs.
//-----------------------------------------------------------------------------

struct MasterDevice final : UnixDevice {
	MasterDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({5, 2});
	}

	std::string nodePath() override {
		return "ptmx";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;
};

struct SlaveDevice final : UnixDevice, std::enable_shared_from_this<SlaveDevice> {
	SlaveDevice(std::shared_ptr<Channel> channel);

	std::string nodePath() override {
		return std::string{};
	}

	async::result<frg::expected<Error, SharedFilePtr>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;

private:
	std::shared_ptr<Channel> _channel;
};

struct MasterFile final : FileWithDefaults {
	struct HandleIoctl;

public:
	static void serve(smarter::shared_ptr<MasterFile> file) {
		assert(!file->_passthrough);

		file->_channel->masterCount++;

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->cancelServe_));
	}

	MasterFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			bool nonBlocking);

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, ControllingTerminalState *>>
	getControllingTerminal() override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override;

	async::result<void>
	ioctl(Process *process, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override;

	async::result<void> setFileFlags(int flags) override {
		if (flags & ~O_NONBLOCK) {
			std::cout << "posix: setFileFlags on pty \e[1;34m" << structName() << "\e[0m called with unknown flags" << std::endl;
			co_return;
		}
		if (flags & O_NONBLOCK)
			_nonBlocking = true;
		else
			_nonBlocking = false;
		co_return;
	}

	async::result<int> getFileFlags() override {
		if(_nonBlocking)
			co_return O_NONBLOCK;
		co_return 0;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void handleClose() override;

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event cancelServe_;

	std::shared_ptr<Channel> _channel;
	Packet _packet{};

	bool _nonBlocking;
};

struct SlaveFile final : FileWithDefaults {
	struct HandleIoctl;

public:
	static void serve(smarter::shared_ptr<SlaveFile> file) {
		assert(!file->_passthrough);

		file->_channel->slaveCount++;

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->cancelServe_));
	}

	SlaveFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			std::shared_ptr<Channel> channel, bool nonBlock, bool read, bool write);

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, ControllingTerminalState *>>
	getControllingTerminal() override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override;

	async::result<void>
	ioctl(Process *process, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override;

	async::result<int> getFileFlags() override {
		int flags = 0;

		if(nonBlock_)
			flags |= O_NONBLOCK;

		if(read_ && write_)
			flags |= O_RDWR;
		else if(read_)
			flags |= O_RDONLY;
		else if(write_)
			flags |= O_WRONLY;

		co_return flags;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<frg::expected<Error, std::string>>
	ttyname() override;

	void handleClose() override;

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event cancelServe_;

	std::shared_ptr<Channel> _channel;
	Packet _packet{};

	bool nonBlock_;
	bool read_;
	bool write_;
};

//-----------------------------------------------------------------------------
// File system structs.
//-----------------------------------------------------------------------------

struct Link final : FsLink {
public:
	explicit Link(smarter::shared_ptr<FsLink, LinkRc> root, std::string name,
			smarter::shared_ptr<DeviceNode> device)
	: _root{std::move(root)}, _name{std::move(name)}, _device{std::move(device)} { }

	smarter::shared_ptr<FsLink, LinkRc> getParent() override;

	std::string getName() override;

	smarter::shared_ptr<FsNode> getTarget() override;

private:
	smarter::shared_ptr<FsLink, LinkRc> _root;
	std::string _name;
	smarter::shared_ptr<DeviceNode> _device;
};

struct RootLink final : FsLink {
	RootLink();

	RootNode *rootNode() {
		return _root.get();
	}

	smarter::shared_ptr<FsLink, LinkRc> getParent() override {
		return nullptr;
	}

	std::string getName() override {
		throw std::logic_error("posix: pts RootLink has no name");
	}

	smarter::shared_ptr<FsNode> getTarget() override;

private:
	smarter::shared_ptr<RootNode> _root;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const smarter::shared_ptr<Link, LinkRc> &link, const std::string &name) const {
		return link->getName() < name;
	}
	bool operator() (const std::string &name, const smarter::shared_ptr<Link, LinkRc> &link) const {
		return name < link->getName();
	}

	bool operator() (const smarter::shared_ptr<Link, LinkRc> &a, const smarter::shared_ptr<Link, LinkRc> &b) const {
		return a->getName() < b->getName();
	}
};

struct DeviceNode final : FsNode {
public:
	DeviceNode(DeviceId id)
	: FsNode(&ptsSuperblock), _type{VfsType::charDevice}, _id{id} { }

	VfsType getType() override {
		return _type;
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix pts DeviceNode::getStats()\e[39m" << std::endl;
		FileStats stats{};
		stats.uid = uid_;
		stats.gid = gid_;

		co_return stats;
	}

	DeviceId readDevice() override {
		return _id;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *process, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openDevice(process, _type, _id, std::move(mount), std::move(link), semantic_flags);
	}

	async::result<std::expected<void, Error>>
	chown(std::optional<uid_t> uid, std::optional<gid_t> gid) override {
		if(uid)
			uid_ = *uid;
		if(gid)
			gid_ = *gid;
		co_return {};
	}

private:
	VfsType _type;
	DeviceId _id;

	uid_t uid_ = 0;
	gid_t gid_ = 0;
};

struct RootNode final : FsNode {
	friend struct Superblock;
	friend struct DirectoryFile;

public:
	RootNode() : FsNode(&ptsSuperblock) {}

	VfsType getType() override {
		return VfsType::directory;
	}

	void linkDevice(FsLink *parent, std::string name,
			smarter::shared_ptr<DeviceNode> node) {
		auto link = makeFsShared<Link>(parent->sharedFromThis(), name, std::move(node));
		_entries.insert(std::move(link));
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix pts RootNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	getLink(FsLink *, std::string name) override {
		auto it = _entries.find(name);
		if(it != _entries.end())
			co_return *it;
		co_return Error::noSuchFile;
	}

private:
	std::set<smarter::shared_ptr<Link, LinkRc>, LinkCompare> _entries;
};

async::result<void>
Channel::commonIoctl(managarm::fs::GenericIoctlRequest req, helix::BorrowedDescriptor conversation) {
	if(req.command() == TIOCSCTTY) {
		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto creds = extractCreds.credentials();
		auto maybeProcess = findProcessWithCredentials(creds);
		if(!maybeProcess) {
			std::cout << "posix: TIOCSCTTY with unknown process credentials" << std::endl;
			managarm::fs::GenericIoctlReply resp;
			resp.set_error(Error::badProcessCredentials | protocols::fs::toFsProtoError | protocols::fs::toFsError);
			auto ser = resp.SerializeAsString();
			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(sendResp.error());
			co_return;
		}
		auto process = *maybeProcess;

		managarm::fs::GenericIoctlReply resp;
		if(auto e = cts.assignSessionOf(process.get()); e == Error::illegalArguments) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}else if(e == Error::insufficientPermissions) {
			resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
		}else{
			assert(e == Error::success);
			cts.getSession()->setForegroundGroup(process->pgPointer().get());
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto [sendResp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(sendResp.error());
	}else if(req.command() == TIOCNOTTY) {
		managarm::fs::GenericIoctlReply resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto creds = extractCreds.credentials();
		auto maybeProcess = findProcessWithCredentials(creds);
		if(!maybeProcess) {
			std::cout << "posix: TIOCNOTTY with unknown process credentials" << std::endl;
			resp.set_error(Error::badProcessCredentials | protocols::fs::toFsProtoError | protocols::fs::toFsError);
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto process = *maybeProcess;

		if(&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
			resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
		} else {
			auto group = cts.getSession()->getForegroundGroup();
			if (group && process->pgPointer()->getSession()->getSessionId() == process->pid())
				group->issueSignalToGroup(SIGHUP, {});

			cts.dropSession(process->pgPointer()->getSession());
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	}else if(req.command() == TIOCGPGRP) {
		managarm::fs::GenericIoctlReply resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto creds = extractCreds.credentials();
		auto maybeProcess = findProcessWithCredentials(creds);
		if(!maybeProcess) {
			std::cout << "posix: TIOCGPGRP with unknown process credentials" << std::endl;
			resp.set_error(Error::badProcessCredentials | protocols::fs::toFsProtoError | protocols::fs::toFsError);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto process = *maybeProcess;

		if(&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
			resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
		} else {
			auto group = cts.getSession()->getForegroundGroup();
			if (group) {
				resp.set_pid(group->getHull()->getPid());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				// If there is no foreground process group, tcgetpgrp() shall return
				// a value greater than 1 that does not match the process group ID of any
				// existing process group.
				resp.set_pid(INT_MAX);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
	}else if(req.command() == TIOCSPGRP) {
		managarm::fs::GenericIoctlReply resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		if (req.pgid() < 0) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto creds = extractCreds.credentials();
		auto maybeProcess = findProcessWithCredentials(creds);
		if(!maybeProcess) {
			std::cout << "posix: TIOCSPGRP with unknown process credentials" << std::endl;
			resp.set_error(Error::badProcessCredentials | protocols::fs::toFsProtoError | protocols::fs::toFsError);
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto process = *maybeProcess;
		if (!cts.getSession()
				|| process->pgPointer()->getSession()->getSessionId()
						!= cts.getSession()->getSessionId()
				|| !process->pgPointer()->getSession()->getControllingTerminal()
				|| process->pgPointer()->getSession()->getControllingTerminal() != &cts) {

			resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}

		auto group = process->pgPointer()->getSession()->getProcessGroupById(req.pgid());
		if(!group) {
			resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
		} else {
			auto sigttouHandler = process->threadGroup()->signalContext()->getHandler(SIGTTOU);
			if (process->pgPointer().get() == process->pgPointer()->getSession()->getForegroundGroup()
					|| sigttouHandler.disposition == SignalDisposition::ignore
					|| (process->signalMask() & (1 << (SIGTTOU - 1)))) {
				// POSIX: If the calling thread is blocking SIGTTOU signals or the process is
				// ignoring SIGTTOU signals, the process shall be allowed to perform
				// the operation.

				auto ret = cts.getSession()->setForegroundGroup(group.get());
				resp.set_error(ret | protocols::fs::toFsProtoError | protocols::fs::toFsError);
			} else if (process->pgPointer()->isOrphaned()) {
				resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				co_return;
			} else {
				// TODO: if process is in a background PG, send SIGTTOU
				// process->pgPointer()->issueSignalToGroup(SIGTTOU, {});
			}
		}

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	}else if(req.command() == TIOCGSID) {
		managarm::fs::GenericIoctlReply resp;

		auto [extractCreds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extractCreds.error());

		auto creds = extractCreds.credentials();
		auto maybeProcess = findProcessWithCredentials(creds);
		if(!maybeProcess) {
			std::cout << "posix: TIOCGSID with unknown process credentials" << std::endl;
			resp.set_error(Error::badProcessCredentials | protocols::fs::toFsProtoError | protocols::fs::toFsError);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return;
		}
		auto process = *maybeProcess;

		if(&cts != process->pgPointer()->getSession()->getControllingTerminal()) {
			resp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);
		} else {
			resp.set_pid(cts.getSession()->getSessionId());
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
	}else{
		std::cout << "\e[31m" "posix: Rejecting unknown PTS ioctl (commonIoctl) " << req.command()
				<< "\e[39m" << std::endl;
	}
}

//-----------------------------------------------------------------------------
// MasterDevice implementation.
//-----------------------------------------------------------------------------

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
MasterDevice::open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: pts MasterDevice open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<MasterFile>(std::move(mount), std::move(link),
			semantic_flags & semanticNonBlock);
	file->setupWeakFile(file);
	MasterFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

MasterFile::MasterFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		bool nonBlocking)
: FileWithDefaults{FileKind::unknown,  StructName::get("pts.master"), std::move(mount), std::move(link), File::defaultPipeLikeSeek},
		_channel{std::make_shared<Channel>(nextPtsIndex++)}, _nonBlocking{nonBlocking} {
	auto slave_device = std::make_shared<SlaveDevice>(_channel);
	charRegistry.install(std::move(slave_device));

	globalRootLink->rootNode()->linkDevice(globalRootLink.get(), std::to_string(_channel->ptsIndex),
			makeFsShared<DeviceNode>(DeviceId{136, _channel->ptsIndex}));
}

async::result<std::expected<size_t, Error>>
MasterFile::readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) {
	if(logReadWrite)
		std::cout << std::format("posix: Read from tty {}\n", structName());
	if(!maxLength)
		co_return std::unexpected{Error::eof};

	// Linux emits EIO if there are no slaves.
	// This takes precedence over EAGAIN.
	if (_channel->masterQueue.empty() && !_channel->slaveCount)
		co_return std::unexpected{Error::ioError};

	if (_channel->masterQueue.empty() && _nonBlocking)
		co_return std::unexpected{Error::wouldBlock};

	while (_channel->masterQueue.empty()) {
		if (!_channel->slaveCount)
			co_return std::unexpected{Error::ioError};
		if (!co_await _channel->statusBell.async_wait(ce))
			co_return std::unexpected{Error::interrupted};
	}

	auto packet = &_channel->masterQueue.front();
	size_t chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
	assert(chunk); // Otherwise, we return above due to !maxLength.
	memcpy(data, packet->buffer.data() + packet->offset, chunk);
	packet->offset += chunk;
	if(packet->offset == packet->buffer.size())
		_channel->masterQueue.pop_front();
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MasterFile::writeAll(Process *, const void *data, size_t length) {
	if(logReadWrite)
		std::cout << std::format("posix: Write to tty {} of size {}\n", structName(), length);

	auto enqueuePacket = [this](Packet packet) {
		_channel->slaveQueue.push_back(std::move(packet));
		_channel->slaveInSeq = ++_channel->currentSeq;
		_channel->statusBell.raise();
	};

	auto s = reinterpret_cast<const char *>(data);
	for(size_t i = 0; i < length; i++) {
		processIn(s[i], _packet, _channel);
	}

	// Check whether all data was discarded above.
	if(!(_channel->activeSettings.c_lflag & ICANON)) {
		enqueuePacket(std::move(_packet));
		_packet = Packet{};
	}

	co_return length;
}

async::result<frg::expected<Error, ControllingTerminalState *>>
MasterFile::getControllingTerminal() {
	co_return &_channel->cts;
}

async::result<frg::expected<Error, PollWaitResult>>
MasterFile::pollWait(Process *, uint64_t past_seq, int mask,
		async::cancellation_token cancellation) {
	assert(past_seq <= _channel->currentSeq);
	int edges = 0;

	while(true) {
		edges = 0;
		if(_channel->slaveCount == 0)
			edges |= EPOLLHUP;
		else
			edges |= EPOLLOUT;

		if(_channel->masterInSeq > past_seq)
			edges |= EPOLLIN;

		if (edges & mask)
			break;

		if (!co_await _channel->statusBell.async_wait(cancellation))
			break;
	}

	co_return PollWaitResult{_channel->currentSeq, edges & mask};
}

async::result<frg::expected<Error, PollStatusResult>>
MasterFile::pollStatus(Process *) {
	// For now making pts files always writable is sufficient.
	int events = 0;
	if(_channel->slaveCount == 0)
		events |= EPOLLHUP;
	else
		events |= EPOLLOUT;

	if(!_channel->masterQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult{_channel->currentSeq, events};
}

struct MasterFile::HandleIoctl {
	async::result<std::expected<void, DispatchError>> operator()(
			managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble, MasterFile *self) {
		if(req.command() == TIOCGPTN) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_pts_index(self->_channel->ptsIndex);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req.command() == TIOCSWINSZ) {
			managarm::fs::GenericIoctlReply resp;

			if(logAttrs)
				std::cout << "posix: PTS window size is now "
						<< req.pts_width() << "x" << req.pts_height()
						<< " chars, "
						<< req.pts_pixel_width() << "x" << req.pts_pixel_height()
						<< " pixels (set by master)" << std::endl;

			self->_channel->width = req.pts_width();
			self->_channel->height = req.pts_height();
			self->_channel->pixelWidth = req.pts_pixel_width();
			self->_channel->pixelHeight = req.pts_pixel_height();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());

			// XXX: This should deliver SIGWINCH to the parent under certain conditions
			UserSignal info;
			self->_channel->cts.issueSignalToForegroundGroup(SIGWINCH, info);
		}else if(req.command() == FIONREAD) {
			managarm::fs::GenericIoctlReply resp;

			size_t count = std::transform_reduce(self->_channel->masterQueue.begin(), self->_channel->masterQueue.end(), size_t{0}, std::plus<>(), [] (const Packet &p) {
				return p.buffer.size() - p.offset;
			});

			resp.set_fionread_count(count);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req.command() == TIOCSCTTY || req.command() == TIOCGPGRP
				|| req.command() == TIOCSPGRP || req.command() == TIOCGSID
				|| req.command() == TIOCNOTTY) {
			co_await self->_channel->commonIoctl(std::move(req), conversation);
		}else if(req.command() == TCGETS) {
			managarm::fs::GenericIoctlReply resp;
			struct termios attrs;

			memset(&attrs, 0, sizeof(struct termios));
			ttyCopyTermios(self->_channel->activeSettings, attrs);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_attrs] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(&attrs, sizeof(struct termios))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_attrs.error());
		}else if(req.command() == TCSETS) {
			struct termios attrs;
			managarm::fs::GenericIoctlReply resp;

			auto [recv_attrs] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(&attrs, sizeof(struct termios))
			);
			HEL_CHECK(recv_attrs.error());
			ttyCopyTermios(attrs, self->_channel->activeSettings);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			std::cout << "\e[31m" "posix: Rejecting unknown PTS master ioctl " << req.command()
					<< "\e[39m" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		co_return {};
	}
};

async::result<void> MasterFile::ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest
	>(conversation, std::move(msg), HandleIoctl{}, this);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

void MasterFile::handleClose() {
	cancelServe_.cancel();
	_passthrough = {};

	if (--_channel->masterCount == 0) {
		_channel->currentSeq++;
		_channel->statusBell.raise();
		_channel->cts.issueSignalToForegroundGroup(SIGHUP, {});
	}
}

//-----------------------------------------------------------------------------
// SlaveDevice implementation.
//-----------------------------------------------------------------------------

SlaveDevice::SlaveDevice(std::shared_ptr<Channel> channel)
: UnixDevice(VfsType::charDevice), _channel{std::move(channel)} {
	assignId({136, _channel->ptsIndex});
}

async::result<frg::expected<Error, SharedFilePtr>>
SlaveDevice::open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: pts SlaveDevice open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	_channel->cts.controllingTerminal_ = weak_from_this();

	auto file = smarter::make_shared<SlaveFile>(std::move(mount), std::move(link), _channel,
			semantic_flags & semanticNonBlock, semantic_flags & semanticRead, semantic_flags & semanticWrite);
	file->setupWeakFile(file);
	SlaveFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

SlaveFile::SlaveFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		std::shared_ptr<Channel> channel, bool nonBlock, bool read, bool write)
: FileWithDefaults{FileKind::unknown,  StructName::get("pts.slave"), std::move(mount), std::move(link),
		File::defaultIsTerminal | File::defaultPipeLikeSeek},
		_channel{std::move(channel)}, nonBlock_{nonBlock}, read_{read}, write_{write} { }

async::result<std::expected<size_t, Error>>
SlaveFile::readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) {
	if(logReadWrite)
		std::cout << "posix: Read from tty " << structName() << std::endl;
	if(!maxLength || _channel->masterCount == 0)
		co_return std::unexpected{Error::eof};

	while(_channel->slaveQueue.empty()){
		if(nonBlock_){
			if(logReadWrite)
				std::cout << "posix: tty would block" << std::endl;
			co_return std::unexpected{Error::wouldBlock};
		}

		if (!co_await _channel->statusBell.async_wait(ce)) {
			if (logReadWrite)
				std::cout << "posix: tty read interrupted" << std::endl;
			co_return std::unexpected{Error::interrupted};
		}
	}

	auto packet = &_channel->slaveQueue.front();
	auto chunk = std::min(packet->buffer.size() - packet->offset, maxLength);
	if(chunk)
		memcpy(data, packet->buffer.data() + packet->offset, chunk);
	packet->offset += chunk;
	if(packet->offset == packet->buffer.size())
		_channel->slaveQueue.pop_front();
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
SlaveFile::writeAll(Process *, const void *data, size_t length) {
	if(logReadWrite)
		std::cout << std::format("posix: Write to tty {}\n", structName());

	if(_channel->masterCount == 0)
		co_return Error::ioError;
	if(!length)
		co_return {};

	// Perform output processing.
	for(size_t i = 0; i < length; i++) {
		char c;
		memcpy(&c, reinterpret_cast<const char *>(data) + i, 1);
		processOut(c, _packet, _channel);
	}

	_channel->masterQueue.push_back(std::move(_packet));
	_channel->masterInSeq = ++_channel->currentSeq;
	_channel->statusBell.raise();
	_packet = Packet{};
	co_return length;
}

async::result<frg::expected<Error, ControllingTerminalState *>>
SlaveFile::getControllingTerminal() {
	co_return &_channel->cts;
}

async::result<frg::expected<Error, PollWaitResult>>
SlaveFile::pollWait(Process *, uint64_t past_seq, int mask,
		async::cancellation_token cancellation) {
	assert(past_seq <= _channel->currentSeq);
	int edges = 0;

	while(true) {
		edges = 0;
		if(_channel->masterCount == 0)
			edges |= EPOLLHUP | EPOLLERR | EPOLLIN;
		else
			edges |= EPOLLOUT;

		if(_channel->slaveInSeq > past_seq)
			edges |= EPOLLIN;

		if (edges & mask)
			break;

		if (!co_await _channel->statusBell.async_wait(cancellation))
			break;
	}

	co_return PollWaitResult{_channel->currentSeq, edges & mask};
}

async::result<frg::expected<Error, PollStatusResult>>
SlaveFile::pollStatus(Process *) {
	int events = 0;
	if(_channel->masterCount == 0)
		events |= EPOLLHUP | EPOLLERR | EPOLLIN;
	else
		events |= EPOLLOUT;

	if(!_channel->slaveQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult{_channel->currentSeq, events};
}

struct SlaveFile::HandleIoctl {
	async::result<std::expected<void, DispatchError>> operator()(
			managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble, SlaveFile *self) {
		if(req.command() == TCGETS) {
			managarm::fs::GenericIoctlReply resp;
			struct termios attrs;

			memset(&attrs, 0, sizeof(struct termios));
			ttyCopyTermios(self->_channel->activeSettings, attrs);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_attrs] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(&attrs, sizeof(struct termios))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_attrs.error());
		}else if(req.command() == TCSETS) {
			struct termios attrs;
			managarm::fs::GenericIoctlReply resp;

			auto [recv_attrs] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(&attrs, sizeof(struct termios))
			);
			HEL_CHECK(recv_attrs.error());

			auto prettyPrintFlags = [](tcflag_t flags, std::map<tcflag_t, std::string> map) -> std::string {
				std::string ret = "";
				tcflag_t leftover = flags;
				for(auto &[val, name] : map) {
					if(flags & val) {
						leftover &= ~val;
						ret.append(std::format("{} ", name));
					}
				}

				if(leftover)
					ret.append(std::format("0o{:o}", leftover));

				return ret;
			};

			if(logAttrs) {
				std::map<tcflag_t, std::string> iflags = {
					{ INLCR, "INLCR" },
					{ ICRNL, "ICRNL" },
					{ IXON, "IXON" },
					{ IUTF8, "IUTF8" },
				};

				std::map<tcflag_t, std::string> oflags = {
					{ OPOST, "OPOST" },
					{ ONLCR, "ONLCR" },
				};

				std::map<tcflag_t, std::string> cflags = {
					{ CREAD, "CREAD" },
					{ HUPCL, "HUPCL" },
				};

				std::map<tcflag_t, std::string> lflags = {
					{ ISIG, "ISIG" },
					{ ICANON, "ICANON" },
					{ XCASE, "XCASE" },
					{ ECHO, "ECHO" },
					{ ECHOE, "ECHOE" },
					{ ECHOK, "ECHOK" },
					{ ECHONL, "ECHONL" },
					{ ECHOCTL, "ECHOCTL" },
					{ ECHOPRT, "ECHOPRT" },
					{ ECHOKE, "ECHOKE" },
					{ NOFLSH, "NOFLSH" },
					{ TOSTOP, "TOSTOP" },
					{ PENDIN, "PENDIN" },
					{ IEXTEN, "IEXTEN" },
				};

				std::cout << "posix: TCSETS request\n"
						<< "   iflag: " << prettyPrintFlags(attrs.c_iflag, iflags) << '\n'
						<< "   oflag: " << prettyPrintFlags(attrs.c_oflag, oflags) << '\n'
						<< "   cflag: " << prettyPrintFlags(attrs.c_cflag, cflags) << '\n'
						<< "   lflag: " << prettyPrintFlags(attrs.c_lflag, lflags) << '\n';
				for(int i = 0; i < NCCS; i++) {
					std::cout << std::dec << "   cc[" << i << "]: 0x"
							<< std::hex << (int)attrs.c_cc[i];
					if(i + 1 < NCCS)
						std::cout << '\n';
				}
				std::cout << std::dec << std::endl;
			}

			ttyCopyTermios(attrs, self->_channel->activeSettings);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req.command() == TIOCGWINSZ) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_pts_width(self->_channel->width);
			resp.set_pts_height(self->_channel->height);
			resp.set_pts_pixel_width(self->_channel->pixelWidth);
			resp.set_pts_pixel_height(self->_channel->pixelHeight);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req.command() == TIOCSWINSZ) {
			managarm::fs::GenericIoctlReply resp;

			if(logAttrs)
				std::cout << "posix: PTS window size is now "
						<< req.pts_width() << "x" << req.pts_height()
						<< " chars, "
						<< req.pts_pixel_width() << "x" << req.pts_pixel_height()
						<< " pixels (set by slave)" << std::endl;

			self->_channel->width = req.pts_width();
			self->_channel->height = req.pts_height();
			self->_channel->pixelWidth = req.pts_pixel_width();
			self->_channel->pixelHeight = req.pts_pixel_height();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());

			// XXX: This should deliver SIGWINCH to the parent under certain conditions
			UserSignal info;
			self->_channel->cts.issueSignalToForegroundGroup(SIGWINCH, info);
		}else if(req.command() == TIOCSCTTY || req.command() == TIOCGPGRP
				|| req.command() == TIOCSPGRP || req.command() == TIOCGSID
				|| req.command() == TIOCNOTTY) {
			co_await self->_channel->commonIoctl(std::move(req), conversation);
		}else if(req.command() == TIOCINQ) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(self->_channel->slaveQueue.empty()) {
				resp.set_fionread_count(0);
			}else{
				auto packet = &self->_channel->slaveQueue.front();
				resp.set_fionread_count(packet->buffer.size() - packet->offset);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else if(req.command() == TIOCGPTN) {
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_pts_index(self->_channel->ptsIndex);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			std::cout << "\e[31m" "posix: Rejecting unknown PTS slave ioctl " << req.command()
					<< "\e[39m" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		co_return {};
	}
};

async::result<void> SlaveFile::ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest
	>(conversation, std::move(msg), HandleIoctl{}, this);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

async::result<frg::expected<Error, std::string>>
SlaveFile::ttyname() {
	smarter::shared_ptr<FsLink, LinkRc> me = associatedLink();
	std::string name;
	if(!isTerminal())
		co_return Error::notTerminal;

	name = me->getName();;

	//TODO: dynamically resolve absolute path?
	co_return std::string("/dev/pts/").append(name);
}

void SlaveFile::handleClose() {
	cancelServe_.cancel();
	_passthrough = {};

	if (--_channel->slaveCount == 0) {
		_channel->currentSeq++;
		_channel->statusBell.raise();
	}
}

//-----------------------------------------------------------------------------
// Link and RootLink implementation.
//-----------------------------------------------------------------------------

smarter::shared_ptr<FsLink, LinkRc> Link::getParent() {
	return _root;
}

std::string Link::getName() {
	return _name;
}

smarter::shared_ptr<FsNode> Link::getTarget() {
	return _device;
}

RootLink::RootLink()
: _root(makeFsShared<RootNode>()) { }

smarter::shared_ptr<FsNode> RootLink::getTarget() {
	return _root->sharedFromThis();
}

smarter::shared_ptr<RootLink, LinkRc> globalRootLink = makeFsShared<RootLink>();

} // anonymous namespace

std::shared_ptr<UnixDevice> createMasterDevice() {
	return std::make_shared<MasterDevice>();
}

smarter::shared_ptr<FsLink, LinkRc> getFsRoot() {
	return globalRootLink;
}

} // namespace pts
