#include <async/cancellation.hpp>
#include <sys/epoll.h>
#include <map>

#include <bragi/helpers-std.hpp>
#include <frg/std_compat.hpp>
#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "process.hpp"
#include "fs.bragi.hpp"

#include <bitset>

namespace extern_fs {

namespace {

struct Node;
struct DirectoryNode;

struct Superblock final : FsSuperblock {
	Superblock(helix::UniqueLane lane, std::shared_ptr<UnixDevice> device);

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *process) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink>>>
			rename(FsLink *source, FsLink *directory, std::string name) override;
	async::result<frg::expected<Error, FsStats>> getFsStats() override;

	std::string getFsType() override {
		return "ext2";
	}

	dev_t deviceNumber() override {
		auto id = device_->getId();
		return makedev(id.first, id.second);
	}

	smarter::shared_ptr<FsLink> internalizeRoot(uint64_t id, helix::UniqueLane lane);
	smarter::shared_ptr<FsLink> internalizeStructural(FsLink *parent, std::string name,
			uint64_t id, helix::UniqueLane lane);
	smarter::shared_ptr<Node> internalizeDirectory(uint64_t id, helix::UniqueLane lane);
	smarter::shared_ptr<Node> internalizePeripheralNode(int64_t type, int id, helix::UniqueLane lane);
	smarter::shared_ptr<FsLink> internalizeLink(FsLink *parent, std::string name,
			smarter::shared_ptr<Node> target);

private:
	helix::UniqueLane _lane;
	std::map<uint64_t, smarter::weak_ptr<DirectoryNode>> _activeStructural;
	std::map<uint64_t, smarter::weak_ptr<Node>> _activePeripheralNodes;
	std::map<std::tuple<uint64_t, std::string, uint64_t>, smarter::weak_ptr<FsLink>> _activeLinks;

	std::shared_ptr<UnixDevice> device_;
};

struct Node : FsNode {
	async::result<frg::expected<Error, FileStats>> getStats() override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_STATS);

		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		FileStats stats{};
		stats.inodeNumber = getInode(); // TODO: Move this out of FileStats.
		stats.fileSize = resp.file_size();
		stats.numLinks = resp.num_links();
		stats.mode = resp.mode();
		stats.uid = resp.uid();
		stats.gid = resp.gid();
		stats.atimeSecs = resp.atime_secs();
		stats.atimeNanos = resp.atime_nanos();
		stats.mtimeSecs = resp.mtime_secs();
		stats.mtimeNanos = resp.mtime_nanos();
		stats.ctimeSecs = resp.ctime_secs();
		stats.ctimeNanos = resp.ctime_nanos();

		co_return stats;
	}

	async::result<Error> chmod(int mode) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_CHMOD);
		req.set_mode(mode);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		co_return Error::success;
	}

	async::result<std::expected<void, Error>> chown(std::optional<uid_t> uid, std::optional<gid_t> gid) override {
		managarm::fs::ChownRequest req;
		req.set_uid(uid.value_or(~0U));
		req.set_gid(gid.value_or(~0U));

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::ChownResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return std::unexpected{resp.error() | toPosixError};

		co_return {};
	}

	async::result<Error> utimensat(std::optional<timespec> atime, std::optional<timespec> mtime,
			timespec ctime) override {
		managarm::fs::UtimensatRequest req;
		if(atime) {
			req.set_atime_sec(atime->tv_sec);
			req.set_atime_nsec(atime->tv_nsec);
			req.set_atime_update(true);
		}

		if(mtime) {
			req.set_mtime_sec(mtime->tv_sec);
			req.set_mtime_nsec(mtime->tv_nsec);
			req.set_mtime_update(true);
		}

		req.set_ctime_sec(ctime.tv_sec);
		req.set_ctime_nsec(ctime.tv_nsec);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		co_return Error::success;
	}


public:
	Node(uint64_t inode, helix::UniqueLane lane, Superblock *sb)
	: FsNode{sb}, _inode{inode}, _lane{std::move(lane)} { }

protected:
	~Node() = default;

public:
	uint64_t getInode() {
		return _inode;
	}

	helix::BorrowedLane getLane() {
		return _lane;
	}

private:
	uint64_t _inode;
	helix::UniqueLane _lane;
};

struct OpenFile final : File {
private:
	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override {
		if(whence == VfsSeek::absolute) {
			co_await _file.seekAbsolute(offset);
			co_return offset;
		} else if(whence == VfsSeek::relative) {
			co_return co_await _file.seekRelative(offset);
		} else if(whence == VfsSeek::eof) {
			co_return co_await _file.seekEof(offset);
		}
		co_return Error::illegalArguments;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override {
		size_t res = co_await _file.writeSome(data, length);
		co_return res;
	}

	// TODO: Ensure that the process is null? Pass credentials of the thread in the request?
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) override {
		auto res = co_await _file.readSome(data, max_length, ce);
		co_return res.transform_error(toPosixError);
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		(void)mask;

		if(sequence > 1)
			co_return Error::illegalArguments;

		if(sequence)
			co_await async::suspend_indefinitely(cancellation);
		co_return PollWaitResult{1, EPOLLIN | EPOLLOUT};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{1, EPOLLIN | EPOLLOUT};
	}

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		auto memory = co_await _file.accessMemory();
		co_return std::move(memory);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

public:
	OpenFile(helix::UniqueLane control, helix::UniqueLane lane,
			std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink> link)
	: File{FileKind::unknown, StructName::get("externfs.file"), std::move(mount), std::move(link)},
			_control{std::move(control)}, _file{std::move(lane)} { }

	~OpenFile() override {
		// It's not necessary to do any cleanup here.
	}

	void handleClose() override {
		// Close the control lane to inform the server that we closed the file.
		_control = helix::UniqueLane{};
	}

	async::result<frg::expected<protocols::fs::Error>> truncate(size_t size) override {
		managarm::fs::TruncateRequest req;
		req.set_size(size);

		auto [offer, send_req, recv_resp]
				= co_await helix_ng::exchangeMsgs(getPassthroughLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | protocols::fs::toFsProtoError;
		co_return {};
	}

private:
	helix::UniqueLane _control;
	protocols::fs::File _file;
};

struct RegularNode final : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		// Regular files do not support O_NONBLOCK.
		semantic_flags &= ~semanticNonBlock;

		if(semantic_flags & ~(semanticRead | semanticWrite | semanticAppend)){
			std::cout << "\e[31mposix: extern_fs OpenFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2), semanticWrite (0x4) and semanticAppend (0x8) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		managarm::fs::NodeOpenRequest req;
		req.set_append((semantic_flags & semanticAppend) != 0);
		req.set_write((semantic_flags & semanticWrite) != 0);
		req.set_read((semantic_flags & semanticRead) != 0);

		auto [offer, send_req, recv_resp, pull_ctrl, pull_passthrough] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::NodeOpenResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		// The lanes are only pushed if the open succeeded.
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(mount), std::move(link));
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}

public:
	RegularNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb} { }
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *, Process *) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_READ_SYMLINK);

		auto [offer, send_req, recv_resp, recv_target] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_target.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		co_return std::string{static_cast<char *>(recv_target.data()), recv_target.length()};
	}

public:
	SymlinkNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb} { }
};

struct Link final : FsLink {
public:
	smarter::shared_ptr<FsLink> getParent() override {
		return _owner;
	}

	async::result<frg::expected<Error>> obstruct() override {
		assert(_owner);
		managarm::fs::ObstructLinkRequest req;
		req.set_link_name(_name);

		auto lane = static_cast<Node *>(_owner->getTarget().get())->getLane();

		auto [offer, send_req, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;
		co_return frg::success_tag{};
	}

	smarter::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

	void renameTo(smarter::shared_ptr<FsLink> owner, std::string name) {
		assert(_owner); // The root link is never renamed.
		_owner = std::move(owner);
		_name = std::move(name);
	}

private:
	std::string getName() override {
		assert(_owner);
		return _name;
	}

public:
	// Constructs the root link of the mount, which has neither an owner nor a name.
	explicit Link(smarter::shared_ptr<FsNode> target)
	: _target{std::move(target)} { }

	Link(smarter::shared_ptr<FsLink> owner, std::string name, smarter::shared_ptr<FsNode> target)
	: _owner{std::move(owner)}, _name{std::move(name)}, _target{std::move(target)} {
		assert(_owner);
	}

private:
	smarter::shared_ptr<FsLink> _owner;
	std::string _name;
	smarter::shared_ptr<FsNode> _target;
};

struct DirectoryNode final : Node {
private:
	VfsType getType() override {
		return VfsType::directory;
	}


	bool hasTraverseLinks() override {
		return true;
	}

	async::result<std::expected<smarter::shared_ptr<FsLink>, Error>>
	getLinkOrCreate(FsLink *parent, Process *process, std::string name, mode_t mode,
			bool exclusive) override {
		assert(this->getType() == VfsType::directory);

		managarm::fs::GetLinkOrCreateRequest req;
		req.set_mode(mode);
		req.set_exclusive(exclusive);
		req.set_name(name);
		req.set_uid(process->threadGroup()->uid());
		req.set_gid(process->threadGroup()->gid());

		auto [offer, send_head, send_tail, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_head.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GetLinkOrCreateResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			if (resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _sb->internalizeStructural(parent, name,
						resp.id(), pull_node.descriptor());
				co_return child;
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				co_return _sb->internalizeLink(parent, name, std::move(child));
			}
		} else {
			co_return std::unexpected{resp.error() | toPosixError};
		}
	}

	async::result<frg::expected<Error, std::pair<smarter::shared_ptr<FsLink>, size_t>>>
	traverseLinks(FsLink *parent, std::deque<std::string> path) override {
		managarm::fs::NodeTraverseLinksRequest req;
		for (auto &i : path)
			req.add_path_segments(i);

		auto [offer, send_head, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		auto conversation = offer.descriptor();
		HEL_CHECK(send_head.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		auto preamble = bragi::read_preamble(recv_resp);
		if (preamble.error()) {
			recv_resp.reset();
			std::cout << "posix: error decoding preamble" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
			co_return Error::ioError;
		}

		auto resp = *bragi::parse_head_only<managarm::fs::NodeTraverseLinksResponse>(recv_resp);
		recv_resp.reset();

		std::vector<uint8_t> tail(preamble.tail_size());
		auto [recv_tail, pull_desc] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size()),
			// TODO: We can just use the conversation lane instead.
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
		);
		HEL_CHECK(recv_tail.error());

		bragi::limited_reader reader{tail.data(), tail.size()};
		if(!resp.decode_tail(reader)) {
			std::cout << "posix: error decoding tail" << std::endl;
			co_return Error::ioError;
		}

		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		HEL_CHECK(pull_desc.error());
		helix::UniqueLane pull_lane = pull_desc.descriptor();

		assert(resp.links_traversed());
		assert(resp.links_traversed() <= path.size());

		// The reply only reports inodes, so we cannot immediately tell which link they correspond to.
		// We replay the resolution rules to associate each link with the proper parent.
		smarter::shared_ptr<FsLink> link = nullptr;
		std::vector<smarter::shared_ptr<FsLink>> dirStack{parent->sharedFromThis()};
		for (size_t i = 0; i < resp.ids().size(); i++) {
			auto [pull_node] = co_await helix_ng::exchangeMsgs(
				pull_lane,
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			);

			HEL_CHECK(pull_node.error());

			bool last = i == resp.ids().size() - 1;

			if (path[i] == ".") {
				link = dirStack.back();
				continue;
			}else if (path[i] == "..") {
				assert(dirStack.size() > 1); // The server does not ascend past parent.
				dirStack.pop_back();
				link = dirStack.back();
				continue;
			}

			if (!last || resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				link = _sb->internalizeStructural(dirStack.back().get(), path[i],
						resp.ids()[i], pull_node.descriptor());
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.ids()[i],
						pull_node.descriptor());
				link = _sb->internalizeLink(dirStack.back().get(), path[i], std::move(child));
			}

			if (!last)
				dirStack.push_back(link);
		}

		co_return std::make_pair(link, resp.links_traversed());
	}

	async::result<std::variant<Error, smarter::shared_ptr<FsLink>>>
	mkdir(FsLink *parent, Process *proc, std::string name, mode_t mode) override {
		auto umask = proc ? proc->fsContext()->getUmask() : 0;

		managarm::fs::MkdirRequest req;
		req.set_path(name);
		req.set_mode(mode & ~umask);
		req.set_uid(proc ? proc->threadGroup()->uid() : 0);
		req.set_gid(proc ? proc->threadGroup()->gid() : 0);

		auto [offer, sendReq, sendTail, recvResp, pullNode] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendTail.error());
		HEL_CHECK(recvResp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());
		recvResp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pullNode.error());

			auto child = _sb->internalizeStructural(parent, name,
					resp.id(), pullNode.descriptor());
			co_return child;
		} else {
			co_return resp.error() | toPosixError;
		}
	}

	async::result<std::variant<Error, smarter::shared_ptr<FsLink>>>
	symlink(FsLink *parent, std::string name, std::string path) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_SYMLINK);
		req.set_name_length(name.size());
		req.set_target_length(path.size());

		auto ser = req.SerializeAsString();
		auto [offer, sendReq, sendName, sendTarget, recvResp, pullNode]
			= co_await helix_ng::exchangeMsgs(getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(name.data(), name.size()),
				helix_ng::sendBuffer(path.data(), path.size()),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendName.error());
		HEL_CHECK(sendTarget.error());
		HEL_CHECK(recvResp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());
		recvResp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pullNode.error());

			auto child = _sb->internalizePeripheralNode(managarm::fs::FileType::SYMLINK,
					resp.id(), pullNode.descriptor());
			co_return _sb->internalizeLink(parent, name, std::move(child));
		} else {
			co_return resp.error() | toPosixError;
		}
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink>>> mkdev(FsLink *parent, std::string name, VfsType type, DeviceId id) override {
		(void)parent;
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
		__builtin_unreachable();
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink>>>
			getLink(FsLink *parent, std::string name) override {
		managarm::fs::GetLinkRequest req;
		req.set_path(name);

		auto [offer, send_req, send_tail, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _sb->internalizeStructural(parent, name,
						resp.id(), pull_node.descriptor());
				co_return child;
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				co_return _sb->internalizeLink(parent, name, std::move(child));
			}
		}else{
			co_return resp.error() | toPosixError;
		}
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink>>> link(FsLink *parent, std::string name,
			smarter::shared_ptr<FsNode> target) override {
		managarm::fs::LinkRequest req;
		req.set_path(name);
		req.set_fd(static_cast<Node *>(target.get())->getInode());

		auto [offer, send_req, send_tail, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _sb->internalizeStructural(parent, name,
						resp.id(), pull_node.descriptor());
				co_return child;
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				co_return _sb->internalizeLink(parent, name, std::move(child));
			}
		}else{
			co_return resp.error() | toPosixError;
		}
	}

	async::result<frg::expected<Error>> unlink(std::string name) override {
		managarm::fs::UnlinkRequest req;
		req.set_path(name);

		auto [offer, send_req, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		co_return {};
	}

	async::result<frg::expected<Error>> rmdir(std::string name) override {
		managarm::fs::RmdirRequest req;
		req.set_path(name);

		auto [offer, send_req, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();

		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		co_return {};
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		// Regular files do not support O_NONBLOCK.
		semantic_flags &= ~semanticNonBlock;

		if(semantic_flags & ~(semanticRead | semanticWrite | semanticAppend)){
			std::cout << "\e[31mposix: extern_fs DirectoryNode open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2), semanticWrite (0x4) and semanticAppend (0x8) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		managarm::fs::NodeOpenRequest req;
		req.set_append((semantic_flags & semanticAppend) != 0);
		req.set_write((semantic_flags & semanticWrite) != 0);
		req.set_read((semantic_flags & semanticRead) != 0);

		auto [offer, send_req, recv_resp, pull_ctrl, pull_passthrough] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::NodeOpenResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		// The lanes are only pushed if the open succeeded.
		if(resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;

		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(mount), std::move(link));
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}

public:
	DirectoryNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb}, _sb{sb} { }

private:
	Superblock *_sb;
};

Superblock::Superblock(helix::UniqueLane lane, std::shared_ptr<UnixDevice> device)
: _lane{std::move(lane)}, device_{device} { }

FutureMaybe<smarter::shared_ptr<FsNode>> Superblock::createRegular(Process *process) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SB_CREATE_REGULAR);
	req.set_uid(process->threadGroup()->uid());
	req.set_gid(process->threadGroup()->gid());

	auto [offer, send_req, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
	if(resp.error() == managarm::fs::Errors::SUCCESS) {
		HEL_CHECK(pull_node.error());

		co_return internalizePeripheralNode(resp.file_type(), resp.id(),
				pull_node.descriptor());
	}else{
		co_return nullptr;
	}
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink>>>
		Superblock::rename(FsLink *source, FsLink *directory, std::string name) {

	managarm::fs::RenameRequest req;
	Link *slink = static_cast<Link *>(source);
	Node *source_node = static_cast<Node *>(slink->getParentNode().get());
	Node *target_node = static_cast<Node *>(directory->getTarget().get());
	smarter::shared_ptr<Node> shared_node = smarter::static_pointer_cast<Node>(source->getTarget());
	req.set_inode_source(source_node->getInode());
	req.set_inode_target(target_node->getInode());
	req.set_old_name(source->getName());
	req.set_new_name(name);

	auto [offer, send_head, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_head.error());
	HEL_CHECK(send_tail.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return resp.error() | toPosixError;

	// _activeLinks is keyed by the owner and name, so the link has to be re-keyed.
	_activeLinks.erase({source_node->getInode(), source->getName(), shared_node->getInode()});
	slink->renameTo(directory->sharedFromThis(), name);
	_activeLinks[{target_node->getInode(), name, shared_node->getInode()}] = source->sharedFromThis();
	co_return source->sharedFromThis();
}

smarter::shared_ptr<FsLink> Superblock::internalizeRoot(uint64_t id, helix::UniqueLane lane) {
	return makeFsShared<Link>(internalizeDirectory(id, std::move(lane)));
}

smarter::shared_ptr<Node> Superblock::internalizeDirectory(uint64_t id, helix::UniqueLane lane) {
	auto entry = &_activeStructural[id];
	if(auto intern = entry->lock(); intern)
		return intern;

	auto node = makeFsShared<DirectoryNode>(this, id, std::move(lane));
	*entry = node;
	return node;
}

smarter::shared_ptr<FsLink> Superblock::internalizeStructural(FsLink *parent,
		std::string name, uint64_t id, helix::UniqueLane lane) {
	return internalizeLink(parent, std::move(name),
			internalizeDirectory(id, std::move(lane)));
}

smarter::shared_ptr<Node> Superblock::internalizePeripheralNode(int64_t type,
		int id, helix::UniqueLane lane) {
	auto entry = &_activePeripheralNodes[id];
	auto intern = entry->lock();
	if(intern)
		return intern;

	smarter::shared_ptr<Node> node;
	switch(type) {
	case managarm::fs::FileType::REGULAR:
		node = makeFsShared<RegularNode>(this, id, std::move(lane));
		break;
	case managarm::fs::FileType::SYMLINK:
		node = makeFsShared<SymlinkNode>(this, id, std::move(lane));
		break;
	default:
		throw std::runtime_error("extern_fs: Unexpected file type");
	}
	*entry = node;
	return node;
}

smarter::shared_ptr<FsLink> Superblock::internalizeLink(FsLink *parent,
		std::string name, smarter::shared_ptr<Node> target) {
	auto parentInode = static_cast<Node *>(parent->getTarget().get())->getInode();
	auto entry = &_activeLinks[{parentInode, name, target->getInode()}];
	if(auto intern = entry->lock(); intern)
		return intern;

	auto link = makeFsShared<Link>(parent->sharedFromThis(), std::move(name), std::move(target));
	*entry = link;
	return link;
}

async::result<frg::expected<Error, FsStats>> Superblock::getFsStats() {
	managarm::fs::GetFsStatsRequest req;

	auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::GetFsStatsResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();

	if(resp.error() != managarm::fs::Errors::SUCCESS) {
		co_return Error::illegalOperationTarget;
	}

	FsStats stats{};
	stats.fsType = resp.fs_type();
	stats.blockSize = resp.block_size();
	stats.fragmentSize = resp.fragment_size();
	stats.numBlocks = resp.num_blocks();
	stats.blocksFree = resp.blocks_free();
	stats.blocksFreeUser = resp.blocks_free_user();
	stats.numInodes = resp.num_inodes();
	stats.inodesFree = resp.inodes_free();
	stats.inodesFreeUser = resp.inodes_free_user();
	stats.maxNameLength = resp.max_name_length();
	stats.fsid[0] = resp.fsid0();
	stats.fsid[1] = resp.fsid1();
	stats.flags = resp.flags();

	co_return stats;
}

} // anonymous namespace

smarter::shared_ptr<FsLink> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane, std::shared_ptr<UnixDevice> device) {
	auto sb = new Superblock{std::move(sb_lane), device};
	// FIXME: 2 is the ext2fs root inode.
	return sb->internalizeRoot(2, std::move(lane));
}

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink> link) {
	auto file = smarter::make_shared<OpenFile>(helix::UniqueLane{},
			std::move(lane), std::move(mount), std::move(link));
	file->setupWeakFile(file);
	return File::constructHandle(std::move(file));
}

} // namespace extern_fs

