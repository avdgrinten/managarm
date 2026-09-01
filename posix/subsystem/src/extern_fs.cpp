#include <async/cancellation.hpp>
#include <sys/epoll.h>
#include <map>
#include <vector>

#include <bragi/helpers-std.hpp>
#include <frg/list.hpp>
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
struct Superblock;

// Bounds the number of reclaimable (directory, name) entries that the name cache keeps alive.
constexpr size_t nameCacheCapacity = 8192;
// Number of hash buckets; a power of two, sized for a load factor of one at capacity.
constexpr size_t nameCacheBuckets = 8192;

size_t nameCacheHash(FsNode *dir, const std::string &name) {
	return std::hash<std::string>{}(name)
			^ (reinterpret_cast<uintptr_t>(dir) * size_t{0x9e3779b97f4a7c15});
}

// If a namecache is enabled for a superblock, links double as namecache entries.
struct Link final : FsLink {
public:
	smarter::shared_ptr<FsLink, LinkRc> getParent() override {
		return _owner;
	}

	async::result<frg::expected<Error>> obstruct() override;

	// Whether this link was obstructed by a mount; cached path walks must not descend past it.
	bool isObstructed() {
		return _obstructed;
	}

	smarter::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

	// A negative link records that a name is known to be absent, so it has no target.
	// Such links only ever exist as name cache entries.
	bool isNegative() {
		return !_target;
	}

	void renameTo(smarter::shared_ptr<FsLink, LinkRc> owner, std::string name) {
		assert(_owner); // The root link is never renamed.
		_owner = std::move(owner);
		_name = std::move(name);
	}

	std::string getName() override {
		assert(_owner);
		return _name;
	}

	// Same as getParentNode() but avoids refcount churn.
	FsNode *ownerNode() {
		return static_cast<Link *>(_owner.get())->_target.get();
	}

	// Constructs the root link of the mount, which has neither an owner nor a name.
	Link(Superblock *sb, smarter::shared_ptr<FsNode> target)
	: _sb{sb}, _target{std::move(target)} { }

	// Constructs a negative link.
	Link(Superblock *sb, smarter::shared_ptr<FsLink, LinkRc> owner, std::string name)
	: _sb{sb}, _owner{std::move(owner)}, _name{std::move(name)} {
		assert(_owner);
	}

	Link(Superblock *sb, smarter::shared_ptr<FsLink, LinkRc> owner, std::string name,
			smarter::shared_ptr<FsNode> target)
	: _sb{sb}, _owner{std::move(owner)}, _name{std::move(name)}, _target{std::move(target)} {
		assert(_owner);
	}

	~Link();

private:
	friend struct Superblock;
	friend struct DirectoryNode;

	Superblock *_sb;
	smarter::shared_ptr<FsLink, LinkRc> _owner;
	std::string _name;
	smarter::shared_ptr<FsNode> _target;
	bool _obstructed = false;

	// Name cache state.
	size_t _cacheHash = 0;
	// Hook for the name cache's hash table chain.
	frg::default_list_hook<Link> _cacheHashHook;
};

struct Superblock final : FsSuperblock, LinkReclaimer {
	Superblock(helix::UniqueLane lane, std::shared_ptr<UnixDevice> device, uint64_t mountCaps);

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *process) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
			rename(FsLink *source, FsLink *directory, std::string name) override;
	async::result<frg::expected<Error, FsStats>> getFsStats() override;

	std::string getFsType() override {
		return "ext2";
	}

	dev_t deviceNumber() override {
		auto id = device_->getId();
		return makedev(id.first, id.second);
	}

	smarter::shared_ptr<FsLink, LinkRc> internalizeRoot(uint64_t id, helix::UniqueLane lane);
	smarter::shared_ptr<FsLink, LinkRc> internalizeStructural(FsLink *parent, std::string name,
			uint64_t id, helix::UniqueLane lane);
	smarter::shared_ptr<Node> internalizeDirectory(uint64_t id, helix::UniqueLane lane);
	smarter::shared_ptr<Node> internalizePeripheralNode(int64_t type, int id, helix::UniqueLane lane);
	smarter::shared_ptr<FsLink, LinkRc> internalizeLink(FsLink *parent, std::string name,
			smarter::shared_ptr<Node> target);

	// Called from destructors. The entry may already belong to a newer object with the same key.
	void forgetStructural(uint64_t id);
	void forgetPeripheralNode(uint64_t id);
	void forgetLink(uint64_t ownerId, const std::string &name, uint64_t targetId);

	// Name caching is only sound if the server cannot change the namespace behind our back.
	bool nameCacheEnabled() {
		return _mountCaps & managarm::fs::MountCaps::MC_CLIENT_EXCLUSIVE_NAMESPACE;
	}

	// Reclaimer that keeps unreferenced links of this mount alive, or null without a name cache.
	LinkReclaimer *linkReclaimer() {
		return nameCacheEnabled() ? this : nullptr;
	}

	std::optional<smarter::shared_ptr<FsLink, LinkRc>> nameCacheLookup(FsNode *dir, const std::string &name);
	void nameCacheInsert(Link *link);
	void nameCacheInvalidate(FsNode *dir, const std::string &name);

private:
	using NameCacheBucket = frg::intrusive_list<Link,
			frg::locate_member<Link, frg::default_list_hook<Link>, &Link::_cacheHashHook>>;
	void onReclaim(LinkMetaObjectBase *meta) override;
	Link *nameCacheFind(FsNode *dir, const std::string &name, size_t hash);
	void nameCacheUnhash(Link *link);
	void nameCacheEvict(Link *link);

	helix::UniqueLane _lane;
	std::map<uint64_t, smarter::weak_ptr<DirectoryNode>> _activeStructural;
	std::map<uint64_t, smarter::weak_ptr<Node>> _activePeripheralNodes;
	std::map<std::tuple<uint64_t, std::string, uint64_t>, smarter::weak_ptr<FsLink, LinkRc>> _activeLinks;

	uint64_t _mountCaps;
	std::vector<NameCacheBucket> _nameCacheBuckets;

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
	: FsNode{sb}, _sb{sb}, _inode{inode}, _lane{std::move(lane)} { }

protected:
	~Node() = default;

	Superblock *_sb;

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
			std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
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
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
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

	~RegularNode() {
		_sb->forgetPeripheralNode(getInode());
	}
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *, Process *) override {
		if(_cachedTarget)
			co_return *_cachedTarget;

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

		std::string target{static_cast<char *>(recv_target.data()), recv_target.length()};
		// On client-exclusive mounts, symlink contents cannot change:
		// there is no way to rewrite a symlink in place, and replacing it creates a new inode.
		if(_sb->nameCacheEnabled())
			_cachedTarget = target;
		co_return target;
	}

public:
	SymlinkNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb} { }

	~SymlinkNode() {
		_sb->forgetPeripheralNode(getInode());
	}

private:
	std::optional<std::string> _cachedTarget;
};

async::result<frg::expected<Error>> Link::obstruct() {
	assert(_owner);
	managarm::fs::ObstructLinkRequest req;
	req.set_link_name(_name);

	auto lane = static_cast<Node *>(ownerNode())->getLane();

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
	_obstructed = true;
	co_return frg::success_tag{};
}

Link::~Link() {
	assert(!_cacheHashHook.in_list);
	// Neither the root link nor negative links are interned.
	if(_owner && _target)
		_sb->forgetLink(static_cast<Node *>(ownerNode())->getInode(),
				_name, static_cast<Node *>(_target.get())->getInode());
}

struct DirectoryNode final : Node {
private:
	VfsType getType() override {
		return VfsType::directory;
	}


	bool hasTraverseLinks() override {
		return true;
	}

	async::result<std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error>>
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

			smarter::shared_ptr<FsLink, LinkRc> result;
			if (resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				result = _sb->internalizeStructural(parent, name,
						resp.id(), pull_node.descriptor());
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				result = _sb->internalizeLink(parent, name, std::move(child));
			}
			recacheLink(parent, name, result.get());
			co_return result;
		} else {
			co_return std::unexpected{resp.error() | toPosixError};
		}
	}

	async::result<frg::expected<Error, std::pair<smarter::shared_ptr<FsLink, LinkRc>, size_t>>>
	traverseLinks(FsLink *parent, std::deque<std::string> path) override {
		// Resolve as many leading components as possible from the name cache.
		size_t consumed = 0;
		bool cacheMiss = false;
		smarter::shared_ptr<FsLink, LinkRc> lastLink;
		auto dirLink = parent->sharedFromThis();
		auto dir = smarter::static_pointer_cast<DirectoryNode>(sharedFromThis());
		while(!path.empty()) {
			const auto &name = path.front();
			if(name == "." || name == "..")
				break; // PathResolver handles these itself once we return.
			auto cached = dir->lookupCache(name);
			if(!cached) {
				cacheMiss = true;
				break;
			}
			// Negative entry: mirror the server-side walk, where any missing
			// component fails the entire traversal.
			if(!*cached)
				co_return Error::noSuchFile;
			lastLink = std::move(*cached);
			path.pop_front();
			++consumed;
			if(path.empty())
				break;
			// Mirror the server-side walk: stop at obstructions (mount points) and
			// at symlinks, fail on other non-directories.
			if(static_cast<Link *>(lastLink.get())->isObstructed())
				break;
			auto type = lastLink->getTarget()->getType();
			if(type == VfsType::symlink)
				break;
			if(type != VfsType::directory)
				co_return Error::notDirectory;
			dirLink = lastLink;
			dir = smarter::static_pointer_cast<DirectoryNode>(lastLink->getTarget());
		}

		if(consumed && !cacheMiss)
			co_return std::make_pair(std::move(lastLink), consumed);

		auto remote = FRG_CO_TRY(co_await dir->remoteTraverseLinks(dirLink.get(), std::move(path)));
		co_return std::make_pair(std::move(remote.first), consumed + remote.second);
	}

	async::result<frg::expected<Error, std::pair<smarter::shared_ptr<FsLink, LinkRc>, size_t>>>
	remoteTraverseLinks(FsLink *parent, std::deque<std::string> path) {
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

		if(resp.error() != managarm::fs::Errors::SUCCESS) {
			// Only a single-component traversal identifies which name is absent.
			if(resp.error() == managarm::fs::Errors::FILE_NOT_FOUND && path.size() == 1)
				cacheLink(parent, path.front(), nullptr);
			co_return resp.error() | toPosixError;
		}

		HEL_CHECK(pull_desc.error());
		helix::UniqueLane pull_lane = pull_desc.descriptor();

		assert(resp.links_traversed());
		assert(resp.links_traversed() <= path.size());

		// The reply only reports inodes, so we cannot immediately tell which link they correspond to.
		// We replay the resolution rules to associate each link with the proper parent.
		smarter::shared_ptr<FsLink, LinkRc> link = nullptr;
		std::vector<smarter::shared_ptr<FsLink, LinkRc>> dirStack{parent->sharedFromThis()};
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
			static_cast<DirectoryNode *>(dirStack.back()->getTarget().get())
					->cacheLink(dirStack.back().get(), path[i], link.get());

			if (!last)
				dirStack.push_back(link);
		}

		co_return std::make_pair(link, resp.links_traversed());
	}

	async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
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

			auto result = _sb->internalizeStructural(parent, name,
					resp.id(), pullNode.descriptor());
			recacheLink(parent, name, result.get());
			co_return result;
		} else {
			co_return resp.error() | toPosixError;
		}
	}

	async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
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
			auto result = _sb->internalizeLink(parent, name, std::move(child));
			recacheLink(parent, name, result.get());
			co_return result;
		} else {
			co_return resp.error() | toPosixError;
		}
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mkdev(FsLink *parent, std::string name, VfsType type, DeviceId id) override {
		(void)parent;
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
		__builtin_unreachable();
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
			getLink(FsLink *parent, std::string name) override {
		if(auto cached = lookupCache(name)) {
			if(!*cached)
				co_return Error::noSuchFile;
			co_return std::move(*cached);
		}

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

			smarter::shared_ptr<FsLink, LinkRc> result;
			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				result = _sb->internalizeStructural(parent, name,
						resp.id(), pull_node.descriptor());
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				result = _sb->internalizeLink(parent, name, std::move(child));
			}
			cacheLink(parent, name, result.get());
			co_return result;
		}else{
			if(resp.error() == managarm::fs::Errors::FILE_NOT_FOUND)
				cacheLink(parent, name, nullptr);
			co_return resp.error() | toPosixError;
		}
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> link(FsLink *parent, std::string name,
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

			smarter::shared_ptr<FsLink, LinkRc> result;
			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				result = _sb->internalizeStructural(parent, name,
						resp.id(), pull_node.descriptor());
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				result = _sb->internalizeLink(parent, name, std::move(child));
			}
			recacheLink(parent, name, result.get());
			co_return result;
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

		invalidateCache(name);
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

		invalidateCache(name);
		co_return {};
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
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
	std::optional<smarter::shared_ptr<FsLink, LinkRc>> lookupCache(const std::string &name) {
		return _sb->nameCacheLookup(this, name);
	}

	// Passing nullptr for link creates a negative cache entry.
	void cacheLink(FsLink *parent, const std::string &name, FsLink *link) {
		if(!_sb->nameCacheEnabled())
			return;
		// PathResolver never resolves these through the cache.
		if(name == "." || name == "..")
			return;
		smarter::shared_ptr<Link, LinkRc> negative;
		if(!link) {
			negative = makeReclaimableLink<Link>(_sb->linkReclaimer(), _sb,
					parent->sharedFromThis(), name);
			link = negative.get();
		}
		auto entry = static_cast<Link *>(link);
		// Links are interned under, and renamed in place to, the owner and name they are
		// resolved through, so the cache key always agrees with what the link carries.
		assert(entry->_owner.get() == parent && entry->_name == name);
		_sb->nameCacheInsert(entry);
	}

	void invalidateCache(const std::string &name) {
		_sb->nameCacheInvalidate(this, name);
	}

	// Replaces the cache entry for name by a link that we just obtained authoritatively.
	void recacheLink(FsLink *parent, const std::string &name, FsLink *link) {
		invalidateCache(name);
		cacheLink(parent, name, link);
	}

	DirectoryNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb} { }

	~DirectoryNode() {
		_sb->forgetStructural(getInode());
	}
};

Superblock::Superblock(helix::UniqueLane lane, std::shared_ptr<UnixDevice> device,
		uint64_t mountCaps)
: LinkReclaimer{nameCacheCapacity}, _lane{std::move(lane)}, _mountCaps{mountCaps},
		device_{device} {
	if(nameCacheEnabled())
		_nameCacheBuckets.resize(nameCacheBuckets);
}

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

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
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

	// Both the name cache and _activeLinks are keyed by the owner and name, so the entries
	// have to go before the link is renamed.
	nameCacheInvalidate(source_node, source->getName());
	nameCacheInvalidate(target_node, name);
	_activeLinks.erase({source_node->getInode(), source->getName(), shared_node->getInode()});
	slink->renameTo(directory->sharedFromThis(), name);
	_activeLinks[{target_node->getInode(), name, shared_node->getInode()}] = source->sharedFromThis();
	co_return source->sharedFromThis();
}

smarter::shared_ptr<FsLink, LinkRc> Superblock::internalizeRoot(uint64_t id, helix::UniqueLane lane) {
	return makeReclaimableLink<Link>(linkReclaimer(), this, internalizeDirectory(id, std::move(lane)));
}

smarter::shared_ptr<Node> Superblock::internalizeDirectory(uint64_t id, helix::UniqueLane lane) {
	auto entry = &_activeStructural[id];
	if(auto intern = entry->lock(); intern)
		return intern;

	auto node = makeFsShared<DirectoryNode>(this, id, std::move(lane));
	*entry = node;
	return node;
}

smarter::shared_ptr<FsLink, LinkRc> Superblock::internalizeStructural(FsLink *parent,
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

smarter::shared_ptr<FsLink, LinkRc> Superblock::internalizeLink(FsLink *parent,
		std::string name, smarter::shared_ptr<Node> target) {
	auto parentInode = static_cast<Node *>(parent->getTarget().get())->getInode();
	auto entry = &_activeLinks[{parentInode, name, target->getInode()}];
	if(auto intern = entry->lock(); intern)
		return intern;

	auto link = makeReclaimableLink<Link>(linkReclaimer(), this, parent->sharedFromThis(),
			std::move(name), std::move(target));
	*entry = link;
	return link;
}

void Superblock::forgetStructural(uint64_t id) {
	auto it = _activeStructural.find(id);
	if(it != _activeStructural.end() && !it->second.lock())
		_activeStructural.erase(it);
}

void Superblock::forgetPeripheralNode(uint64_t id) {
	auto it = _activePeripheralNodes.find(id);
	if(it != _activePeripheralNodes.end() && !it->second.lock())
		_activePeripheralNodes.erase(it);
}

void Superblock::forgetLink(uint64_t ownerId, const std::string &name, uint64_t targetId) {
	auto it = _activeLinks.find({ownerId, name, targetId});
	if(it != _activeLinks.end() && !it->second.lock())
		_activeLinks.erase(it);
}

Link *Superblock::nameCacheFind(FsNode *dir, const std::string &name, size_t hash) {
	if(_nameCacheBuckets.empty())
		return nullptr;
	auto &bucket = _nameCacheBuckets[hash & (nameCacheBuckets - 1)];
	for(auto it = bucket.begin(); it != bucket.end(); ++it) {
		Link *link = *it;
		if(link->_cacheHash == hash && link->ownerNode() == dir && link->_name == name)
			return link;
	}
	return nullptr;
}

std::optional<smarter::shared_ptr<FsLink, LinkRc>> Superblock::nameCacheLookup(FsNode *dir,
		const std::string &name) {
	auto link = nameCacheFind(dir, name, nameCacheHash(dir, name));
	if(!link)
		return std::nullopt;
	touch(link->selfPtr().policy().meta());
	if(link->isNegative())
		return smarter::shared_ptr<FsLink, LinkRc>{};
	return link->sharedFromThis();
}

void Superblock::nameCacheInsert(Link *link) {
	assert(!_nameCacheBuckets.empty());
	assert(link->_owner);
	auto hash = nameCacheHash(link->ownerNode(), link->_name);
	if(auto present = nameCacheFind(link->ownerNode(), link->_name, hash)) {
		if(present == link) {
			touch(link->selfPtr().policy().meta());
			return;
		}
		// The name was re-created and now refers to a different link.
		nameCacheEvict(present);
	}
	// The link stays alive on its own: the reclaimer holds a reference from construction on.
	link->_cacheHash = hash;
	_nameCacheBuckets[hash & (nameCacheBuckets - 1)].push_front(link);
}

void Superblock::nameCacheInvalidate(FsNode *dir, const std::string &name) {
	if(auto link = nameCacheFind(dir, name, nameCacheHash(dir, name)))
		nameCacheEvict(link);
}

void Superblock::nameCacheUnhash(Link *link) {
	auto &bucket = _nameCacheBuckets[link->_cacheHash & (nameCacheBuckets - 1)];
	bucket.erase(bucket.iterator_to(link));
}

// Drops an entry from the cache: it cannot be found by name anymore, hence it is only worth
// keeping around while something else still references it.
void Superblock::nameCacheEvict(Link *link) {
	nameCacheUnhash(link);
	reclaim(link->selfPtr().policy().meta());
}

void Superblock::onReclaim(LinkMetaObjectBase *meta) {
	auto link = static_cast<LinkMetaObject<Link> *>(meta)->get();
	if(link->_cacheHashHook.in_list)
		nameCacheUnhash(link);
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

smarter::shared_ptr<FsLink, LinkRc> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane,
		std::shared_ptr<UnixDevice> device, uint64_t mountCaps) {
	auto sb = new Superblock{std::move(sb_lane), device, mountCaps};
	// FIXME: 2 is the ext2fs root inode.
	return sb->internalizeRoot(2, std::move(lane));
}

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link) {
	auto file = smarter::make_shared<OpenFile>(helix::UniqueLane{},
			std::move(lane), std::move(mount), std::move(link));
	file->setupWeakFile(file);
	return File::constructHandle(std::move(file));
}

} // namespace extern_fs

