#include <fcntl.h>
#include <linux/magic.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <set>
#include <shared_mutex>

#include <async/mutex.hpp>
#include <core/clock.hpp>
#include <core/mount.hpp>
#include <frg/mutex.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <helix/passthrough-fd.hpp>
#include <protocols/fs/client.hpp>
#include <protocols/fs/server.hpp>
#include "common.hpp"
#include "device.hpp"
#include "protocols/fs/common.hpp"
#include "tmp_fs.hpp"
#include "fifo.hpp"
#include "process.hpp"
#include "swap.hpp"
#include <sys/stat.h>

#include <bitset>

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __raw_map(int fd);

namespace tmp_fs {

// Locking. tmpfs only suspends to access file data and to open FIFOs, hence all locks except for
// the ones that protect file data and offsets are std:: locks. Locks are taken in the following
// order:
// 1. MemoryFile::_offsetMutex (async::mutex): the file offset; not taken by pread/pwrite.
// 2. Superblock::_topologyMutex (std::shared_mutex): taken shared by every operation that
//    mutates directory entries and exclusively by rename(). Also protects Link::_owner and
//    Link::_name.
// 3. DirectoryNode::_dirMutex (std::shared_mutex): _entries and _dead. When two are held,
//    the parent's is taken first; only rename() (under exclusive topology) holds more.
// 4. MemoryNode::_dataMutex (async::shared_mutex): the file's memory; never held together with 3.
// 5. Node::_metaMutex (std::mutex): mode, owner, timestamps and link count. Leaf lock.
// Links that are removed from a directory are only dropped after releasing all locks, since
// destructing them can destruct directories (and their locks) or FIFO channels.
// Observers are notified after releasing all locks.

namespace {

struct Superblock;

std::shared_mutex &topologyMutexOf(FsSuperblock *superblock);

struct Node : FsNode {
	Node(Superblock *superblock, FsNode::DefaultOps default_ops = 0);

protected:
	~Node() = default;

	void initializeMode(mode_t mode) {
		_mode = mode;
	}

	void initializeOwner(uid_t uid, gid_t gid) {
		_uid = uid;
		_gid = gid;
	}

	// Returns the stats of this node except for the file size.
	FileStats baseStats() {
		std::lock_guard lock{_metaMutex};
		FileStats stats{};
		stats.inodeNumber = _inodeNumber;
		stats.numLinks = _numLinks;
		stats.mode = _mode;
		stats.uid = _uid;
		stats.gid = _gid;
		stats.atimeSecs = _atime.tv_sec;
		stats.atimeNanos = _atime.tv_nsec;
		stats.mtimeSecs = _mtime.tv_sec;
		stats.mtimeNanos = _mtime.tv_nsec;
		stats.ctimeSecs = _ctime.tv_sec;
		stats.ctimeNanos = _ctime.tv_nsec;
		return stats;
	}

public:
	async::result<frg::expected<Error, FileStats>> getStats() override {
		co_return baseStats();
	}

	int64_t inodeNumber() {
		return _inodeNumber;
	}

	void adjustLinkCount(int delta) {
		std::lock_guard lock{_metaMutex};
		_numLinks += delta;
	}

	async::result<Error> chmod(int mode) override {
		std::lock_guard lock{_metaMutex};
		_mode = (_mode & 0xFFFFF000) | mode;
		co_return Error::success;
	}

	async::result<Error> utimensat(std::optional<timespec> atime, std::optional<timespec> mtime,
			timespec ctime) override {
		std::lock_guard lock{_metaMutex};
		if(atime) {
			_atime.tv_sec = atime->tv_sec;
			_atime.tv_nsec = atime->tv_nsec;
		}

		if(mtime) {
			_mtime.tv_sec = mtime->tv_sec;
			_mtime.tv_nsec = mtime->tv_nsec;
		}

		_ctime.tv_sec = ctime.tv_sec;
		_ctime.tv_nsec = ctime.tv_nsec;
		co_return Error::success;
	}

	async::result<std::expected<void, Error>> chown(std::optional<uid_t> uid, std::optional<gid_t> gid) override {
		std::lock_guard lock{_metaMutex};
		if(uid)
			_uid = *uid;
		if(gid)
			_gid = *gid;
		co_return {};
	}

private:
	int64_t _inodeNumber;

	// Leaf lock.
	std::mutex _metaMutex;
	// Protected by _metaMutex.
	int _numLinks = 1;
	// Protected by _metaMutex.
	mode_t _mode = 0;
	// Protected by _metaMutex.
	uid_t _uid = 0;
	// Protected by _metaMutex.
	gid_t _gid = 0;
	// Protected by _metaMutex.
	timespec _atime = {0, 0};
	// Protected by _metaMutex.
	timespec _mtime = {0, 0};
	// Protected by _metaMutex.
	timespec _ctime = {0, 0};
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *, Process *) override {
		co_return _link;
	}

public:
	SymlinkNode(Superblock *superblock, std::string link);

	async::result<frg::expected<Error, FileStats>> getStats() override {
		auto stats = baseStats();
		stats.fileSize = _link.size();
		co_return stats;
	}

private:
	std::string _link;
};

struct DeviceNode final : Node {
private:
	VfsType getType() override {
		return _type;
	}

	DeviceId readDevice() override {
		return _id;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(Process *process, std::shared_ptr<MountView> mount,
			smarter::shared_ptr<FsLink, LinkRc> link, SemanticFlags semantic_flags) override {
		return openDevice(process, _type, _id, std::move(mount), std::move(link), semantic_flags);
	}

public:
	DeviceNode(Superblock *superblock, VfsType type, DeviceId id);

private:
	VfsType _type;
	DeviceId _id;
};

struct SocketNode final : Node {
	SocketNode(Superblock *superblock, mode_t mode, uid_t uid, gid_t gid);

	VfsType getType() override {
		return VfsType::socket;
	}
};

struct FifoNode final : Node {
private:
	VfsType getType() override {
		return VfsType::fifo;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(Process *, std::shared_ptr<MountView> mount,
			smarter::shared_ptr<FsLink, LinkRc> link, SemanticFlags semantic_flags) override {
		co_return co_await fifo::openNamedChannel(mount, link, this, semantic_flags);
	}

public:
	FifoNode(Superblock *superblock, mode_t mode)
	:Node{superblock} {
		fifo::createNamedChannel(this);
		initializeMode(mode);
	}

	~FifoNode() {
		fifo::unlinkNamedChannel(this);
	}
};

struct Link final : FsLink {
public:
	explicit Link(smarter::shared_ptr<FsNode> target)
	: _sb(target->superblock()), _target(std::move(target)) { }

	// The superblock is the one of the directory: populateRootView() links nodes of other
	// superblocks into tmpfs directories.
	explicit Link(smarter::shared_ptr<FsLink, LinkRc> owner, std::string name, smarter::shared_ptr<FsNode> target)
	: _sb(owner->getTarget()->superblock()), _owner(std::move(owner)), _name(std::move(name)),
			_target(std::move(target)) {
		assert(_owner);
		assert(!_name.empty());
	}

	smarter::shared_ptr<FsLink, LinkRc> getParent() override {
		std::shared_lock lock{topologyMutexOf(_sb)};
		return _owner;
	}

	std::string getName() override {
		std::shared_lock lock{topologyMutexOf(_sb)};
		// The root link does not have a name.
		assert(_owner);
		return _name;
	}

	std::pair<smarter::shared_ptr<FsLink, LinkRc>, std::string> getParentAndName() override {
		std::shared_lock lock{topologyMutexOf(_sb)};
		return {_owner, _name};
	}

	smarter::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

	// Callers must hold the topology lock or the lock of the directory that contains the link:
	// rename() only renames links while they are not part of any directory.
	const std::string &nameLocked() {
		return _name;
	}

	// Callers must hold the topology lock.
	Link *ownerLocked() {
		return static_cast<Link *>(_owner.get());
	}

	// Callers must hold the topology lock exclusively.
	// Returns the previous owner, which callers drop after releasing their locks.
	smarter::shared_ptr<FsLink, LinkRc> renameTo(smarter::shared_ptr<FsLink, LinkRc> owner, std::string name) {
		assert(_owner); // The root link is never renamed.
		_name = std::move(name);
		return std::exchange(_owner, std::move(owner));
	}

private:
	FsSuperblock *_sb;
	// Protected by the topology lock.
	smarter::shared_ptr<FsLink, LinkRc> _owner;
	std::string _name;
	smarter::shared_ptr<FsNode> _target;
};

// Callers must hold the lock of the directory whose entries are compared.
struct LinkCompare {
	struct is_transparent { };

	bool operator() (const smarter::shared_ptr<Link, LinkRc> &link, const std::string &name) const {
		return link->nameLocked() < name;
	}

	bool operator() (const std::string &name, const smarter::shared_ptr<Link, LinkRc> &link) const {
		return name < link->nameLocked();
	}

	bool operator() (const smarter::shared_ptr<Link, LinkRc> &a, const smarter::shared_ptr<Link, LinkRc> &b) const {
		return a->nameLocked() < b->nameLocked();
	}
};

struct DirectoryNode;

struct DirectoryFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link);

	void handleClose() override;

	async::result<Error> synchronize(protocols::fs::SynchronizeFlags) override {
		co_return Error::success;
	}

	FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// The '.' and '..' entries are synthesized before iterating _entries.
	DotEntriesPhase _dots = DotEntriesPhase::dot;

	// Name of the last entry that readEntries() returned.
	std::optional<std::string> _lastName;
	size_t _numEntries = 0;
};

struct DirectoryNode final : Node {
	friend struct Superblock;
	friend struct DirectoryFile;

	static smarter::shared_ptr<Link, LinkRc> createRootDirectory(Superblock *superblock, int mode, uid_t uid, gid_t gid);

private:
	VfsType getType() override {
		return VfsType::directory;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		// we silently ignore semanticAppend for directories
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite | semanticAppend)){
			std::println("\e[31mposix: DirectoryNode open() received illegal arguments: {:#x}", semantic_flags);
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		DirectoryFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}

	async::result<std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error>>
	getLinkOrCreate(FsLink *parent, Process *process, std::string name, mode_t mode,
			bool exclusive) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *, std::string name) override {
		if(auto link = lookup(name); link)
			co_return link;
		co_return Error::noSuchFile;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> link(FsLink *parent, std::string name,
			smarter::shared_ptr<FsNode> target) override;

	async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	mkdir(FsLink *parent, Process *p, std::string name, mode_t mode) override;

	async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	symlink(FsLink *parent, std::string name, std::string path) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mkdev(FsLink *parent, std::string name,
			VfsType type, DeviceId id) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mkfifo(FsLink *parent, std::string name, mode_t mode) override;

	async::result<frg::expected<Error>> unlink(std::string name) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mksocket(FsLink *parent, std::string name, mode_t mode, uid_t uid, gid_t gid) override;

	async::result<frg::expected<Error>> rmdir(std::string name) override;

public:
	DirectoryNode(Superblock *superblock, int mode, uid_t uid, gid_t gid);

	// Synchronous operations for posix-internal users (e.g., devtmpfs).
	// Returns nullptr if the entry does not exist.
	smarter::shared_ptr<Link, LinkRc> lookup(const std::string &name);

	std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
	createDirectory(FsLink *parent, std::string name, int mode, uid_t uid, gid_t gid);

	// Returns the existing entry (of any type) or creates a new directory.
	std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
	getOrCreateDirectory(FsLink *parent, std::string name, int mode, uid_t uid, gid_t gid);

	std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
	createDevice(FsLink *parent, std::string name, VfsType type, DeviceId id);

private:
	using EntrySet = std::set<smarter::shared_ptr<Link, LinkRc>, LinkCompare>;

	// Returns the entry called name, or inserts a link to the node that makeNode() returns.
	// The bool is true if the link was inserted.
	template<typename MakeNode>
	std::expected<std::pair<smarter::shared_ptr<Link, LinkRc>, bool>, Error>
	getOrInsert(FsLink *parent, const std::string &name, bool isDirectory, MakeNode makeNode);

	// Like getOrInsert(), but fails if the entry exists already.
	template<typename MakeNode>
	std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
	insert(FsLink *parent, const std::string &name, bool isDirectory, MakeNode makeNode);

	std::shared_mutex _dirMutex;
	// Protected by _dirMutex.
	// TODO: This creates a circular reference -- fix this.
	EntrySet _entries;
	// Protected by _dirMutex. Set once the directory was removed; no entries can be added anymore.
	bool _dead = false;
};

// TODO: Remove this class in favor of MemoryNode.
struct InheritedNode final : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: tmp_fs open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}
		auto fd = ::open(_path.c_str(), O_RDONLY);
		assert(fd != -1);

		helix::UniqueDescriptor passthrough(helix::handleForFd(fd));
		co_return extern_fs::createFile(std::move(passthrough), std::move(mount), std::move(link));
	}

public:
	InheritedNode(Superblock *superblock, std::string path);

private:
	std::string _path;
};

struct MemoryFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, SemanticFlags flags)
	: FileWithDefaults{FileKind::unknown,  StructName::get("tmpfs.regular"), std::move(mount), std::move(link)},
	flags_{flags}, _offset{0} { }

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t delta, VfsSeek whence) override;

	async::result<Error> synchronize(protocols::fs::SynchronizeFlags) override {
		co_return Error::success;
	}

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *buffer, size_t max_length, async::cancellation_token ce) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *buffer, size_t length) override;

	async::result<std::expected<size_t, Error>>
	pread(Process *, int64_t offset, void *buffer, size_t length) override;

	async::result<frg::expected<Error, size_t>>
	pwrite(Process *, int64_t offset, const void *buffer, size_t length) override;

	async::result<frg::expected<protocols::fs::Error>> truncate(size_t size) override;

	async::result<frg::expected<protocols::fs::Error>> allocate(int64_t offset, size_t size) override;

	async::result<int> getFileFlags() override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;
	SemanticFlags flags_;

	// Protects _offset. Not taken by pread() and pwrite().
	async::mutex _offsetMutex;
	uint64_t _offset;
};

struct MemoryNode final : Node {
	friend struct MemoryFile;

	MemoryNode(Superblock *superblock, mode_t mode = 0);
	~MemoryNode();

	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite | semanticAppend)){
			std::cout << "\e[31mposix: MemoryNode open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2), semanticWrite(0x4) and semanticAppend(0x8) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}
		auto file = smarter::make_shared<MemoryFile>(std::move(mount), std::move(link), semantic_flags);
		file->setupWeakFile(file);
		MemoryFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		auto stats = baseStats();
		stats.fileSize = _fileSize.load(std::memory_order_relaxed);
		co_return stats;
	}

private:
	// Must be called with _dataMutex held exclusively.
	async::result<void> _resizeFile(size_t new_size) {
		// TODO: Growing the file again exposes the truncated contents. Zero them on shrink once memory
		//       objects can discard a range; writing zeros would allocate all of it.
		_fileSize = new_size;

		size_t aligned_size = (new_size + 0xFFF) & ~size_t(0xFFF);
		if(aligned_size <= _areaSize)
			co_return;

		if(_memory) {
			auto result = co_await helix_ng::resizeMemory(_memory, aligned_size);
			HEL_CHECK(result.error());
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateSwappableMemory(sharedHierarchy(),
					getSwapSpaceHandle().getHandle(), aligned_size, 0, &handle));
			_memory = helix::UniqueDescriptor{handle};
		}

		_areaSize = aligned_size;
	}

	async::result<frg::expected<Error>> _readData(size_t offset, void *buffer, size_t length) {
		if(!length)
			co_return {};
		auto load = co_await helix_ng::readMemory(_memory, offset, length, buffer);
		if(load.error() == kHelErrNoMemory)
			co_return Error::noMemory;
		HEL_CHECK(load.error());
		co_return {};
	}

	async::result<frg::expected<Error>> _writeData(size_t offset, const void *buffer, size_t length) {
		if(!length)
			co_return {};
		auto store = co_await helix_ng::writeMemory(_memory, offset, length, buffer);
		if(store.error() == kHelErrNoMemory)
			co_return Error::noMemory;
		HEL_CHECK(store.error());
		co_return {};
	}

	// Never held together with a directory lock.
	async::shared_mutex _dataMutex;
	// Protected by _dataMutex.
	helix::UniqueDescriptor _memory;
	// Protected by _dataMutex.
	size_t _areaSize;
	// Only written with _dataMutex held exclusively; atomic such that getStats() can read it.
	std::atomic<size_t> _fileSize;
};

struct Superblock final : FsSuperblock {
	Superblock(std::string name = "tmpfs") : fsType_{name} {
		deviceMinor_ = allocateUnnamedDeviceId();
	}

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *) override {
		auto node = makeFsShared<MemoryNode>(this);
		co_return std::move(node);
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> rename(FsLink *src_fs_link,
			FsLink *dest_fs_link, std::string dest_name) override {
		auto src_link = static_cast<Link *>(src_fs_link);
		auto dest_link = static_cast<Link *>(dest_fs_link);

		auto dest_fs_dir = dest_fs_link->getTarget();
		if(dest_fs_dir->getType() != VfsType::directory)
			co_return Error::notDirectory;
		auto dest_dir = static_cast<DirectoryNode *>(dest_fs_dir.get());

		auto target = src_link->getTarget();

		// Dropped after releasing the locks.
		DirectoryNode::EntrySet::node_type overwritten_entry;
		smarter::shared_ptr<FsLink, LinkRc> old_owner;
		smarter::shared_ptr<FsNode> src_fs_dir;
		smarter::shared_ptr<FsNode> overwritten;
		smarter::shared_ptr<Link, LinkRc> result;

		std::unique_lock topology_lock{_topologyMutex};

		auto src_owner = src_link->ownerLocked();
		if(!src_owner)
			co_return Error::illegalArguments;
		src_fs_dir = src_owner->getTarget();
		auto src_dir = static_cast<DirectoryNode *>(src_fs_dir.get());

		// Moving a directory into its own subtree would detach it into a cycle.
		if(target->getType() == VfsType::directory) {
			for(auto ancestor = dest_link; ancestor; ancestor = ancestor->ownerLocked()) {
				if(ancestor == src_link)
					co_return Error::illegalArguments;
			}
		}

		// Nobody else holds more than one directory lock under exclusive topology,
		// hence the order does not matter here.
		std::unique_lock src_lock{src_dir->_dirMutex};
		std::unique_lock<std::shared_mutex> dest_lock;
		if(dest_dir != src_dir)
			dest_lock = std::unique_lock{dest_dir->_dirMutex};

		if(dest_dir->_dead)
			co_return Error::noSuchFile;

		auto it = src_dir->_entries.find(src_link->nameLocked());
		if(it == src_dir->_entries.end() || it->get() != src_link)
			co_return Error::noSuchFile;

		// Unlink an existing link if such a link exists.
		if(auto dest_it = dest_dir->_entries.find(dest_name);
				dest_it != dest_dir->_entries.end()) {
			overwritten = (*dest_it)->getTarget();
			// Renaming a file onto itself is a no-op.
			if(overwritten.get() == target.get()) {
				result = *dest_it;
				co_return result;
			}

			if(overwritten->getType() == VfsType::directory) {
				// A directory may only be replaced by another empty directory.
				if(target->getType() != VfsType::directory)
					co_return Error::isDirectory;
				auto victim = static_cast<DirectoryNode *>(overwritten.get());
				// The source directory contains the source link.
				if(victim == src_dir)
					co_return Error::directoryNotEmpty;
				std::unique_lock victim_lock{victim->_dirMutex};
				if(!victim->_entries.empty())
					co_return Error::directoryNotEmpty;
				victim->_dead = true;
				dest_dir->adjustLinkCount(-1); // Drop its '..' backlink.
			}else{
				if(target->getType() == VfsType::directory)
					co_return Error::notDirectory;
				static_cast<Node *>(overwritten.get())->adjustLinkCount(-1);
			}
			overwritten_entry = dest_dir->_entries.extract(dest_it);
		}

		// _entries is ordered by name, so the link has to leave the set before it is renamed.
		auto moved = src_dir->_entries.extract(it);
		result = moved.value();
		old_owner = result->renameTo(dest_fs_link->sharedFromThis(), std::move(dest_name));
		dest_dir->_entries.insert(std::move(moved));

		if(target->getType() == VfsType::directory) {
			// Account for the moved directory's '..' backlink.
			if(src_dir != dest_dir) {
				src_dir->adjustLinkCount(-1);
				dest_dir->adjustLinkCount(1);
			}
		}

		if(dest_lock)
			dest_lock.unlock();
		src_lock.unlock();
		topology_lock.unlock();
		co_return result;
	}

	async::result<frg::expected<Error, FsStats>> getFsStats() override {
		FsStats stats{
			.fsType = TMPFS_MAGIC,
		};
		co_return stats;
	}

	std::string getFsType() override {
		return fsType_;
	}

	int64_t allocateInode() {
		return _inodeCounter.fetch_add(1, std::memory_order_relaxed);
	}

	std::shared_mutex &topologyMutex() {
		return _topologyMutex;
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

private:
	std::atomic<int64_t> _inodeCounter = 1;

	std::shared_mutex _topologyMutex;

	std::string fsType_;
	unsigned int deviceMinor_;
};

std::shared_mutex &topologyMutexOf(FsSuperblock *superblock) {
	return static_cast<Superblock *>(superblock)->topologyMutex();
}

// ----------------------------------------------------------------------------
// MemoryNode and MemoryFile implementation.
// ----------------------------------------------------------------------------

MemoryNode::MemoryNode(Superblock *superblock, mode_t mode)
: Node{superblock, FsNode::defaultSupportsObservers}, _areaSize{0}, _fileSize{0} {
	initializeMode(mode);
}

MemoryNode::~MemoryNode() {
	notifyObservers(FsObserver::deleteSelfEvent, {}, 0);
}

void MemoryFile::handleClose() {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
	if(flags_ & semanticWrite)
		node->notifyObservers(FsObserver::closeWriteEvent, {}, 0);
	else
		node->notifyObservers(FsObserver::closeNoWriteEvent, {}, 0);
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>>
MemoryFile::seek(off_t delta, VfsSeek whence) {
	co_await _offsetMutex.async_lock();
	frg::unique_lock offset_lock{frg::adopt_lock, _offsetMutex};

	if(whence == VfsSeek::absolute) {
		_offset = delta;
	}else if(whence == VfsSeek::relative){
		_offset += delta;
	}else if(whence == VfsSeek::eof) {
		auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
		_offset = delta + node->_fileSize.load(std::memory_order_relaxed);
	} else {
		co_return Error::illegalArguments;
	}
	co_return _offset;
}

async::result<std::expected<size_t, Error>>
MemoryFile::readSome(Process *, void *buffer, size_t max_length, async::cancellation_token) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	size_t chunk;
	{
		co_await _offsetMutex.async_lock();
		frg::unique_lock offset_lock{frg::adopt_lock, _offsetMutex};
		co_await node->_dataMutex.async_lock_shared();
		frg::shared_lock data_lock{frg::adopt_lock, node->_dataMutex};

		size_t file_size = node->_fileSize.load(std::memory_order_relaxed);
		if(!(_offset <= file_size))
			co_return std::unexpected{Error::eof};
		chunk = std::min(file_size - _offset, max_length);

		if(auto result = co_await node->_readData(_offset, buffer, chunk); !result)
			co_return std::unexpected{result.error()};
		_offset += chunk;
	}
	node->notifyObservers(FsObserver::accessEvent, associatedLink()->getName(), 0);
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::writeAll(Process *, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if (!(flags_ & semanticWrite))
		co_return Error::badFileDescriptor;

	{
		co_await _offsetMutex.async_lock();
		frg::unique_lock offset_lock{frg::adopt_lock, _offsetMutex};
		co_await node->_dataMutex.async_lock();
		frg::unique_lock data_lock{frg::adopt_lock, node->_dataMutex};

		if(flags_ & semanticAppend)
			_offset = node->_fileSize.load(std::memory_order_relaxed);

		if(_offset + length > node->_fileSize.load(std::memory_order_relaxed))
			co_await node->_resizeFile(_offset + length);

		FRG_CO_TRY(co_await node->_writeData(_offset, buffer, length));
		_offset += length;
	}
	node->notifyObservers(FsObserver::modifyEvent, associatedLink()->getName(), 0);
	co_return length;
}

async::result<std::expected<size_t, Error>>
MemoryFile::pread(Process *, int64_t offset, void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	co_await node->_dataMutex.async_lock_shared();
	frg::shared_lock data_lock{frg::adopt_lock, node->_dataMutex};

	size_t file_size = node->_fileSize.load(std::memory_order_relaxed);
	if(static_cast<size_t>(offset) >= file_size)
		co_return std::unexpected{Error::eof};
	auto chunk = std::min(file_size - offset, length);

	if(auto result = co_await node->_readData(offset, buffer, chunk); !result)
		co_return std::unexpected{result.error()};

	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::pwrite(Process *, int64_t offset, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	co_await node->_dataMutex.async_lock();
	frg::unique_lock data_lock{frg::adopt_lock, node->_dataMutex};

	if(offset + length > node->_fileSize.load(std::memory_order_relaxed))
		co_await node->_resizeFile(offset + length);

	FRG_CO_TRY(co_await node->_writeData(offset, buffer, length));
	co_return length;
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::truncate(size_t size) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	co_await node->_dataMutex.async_lock();
	frg::unique_lock data_lock{frg::adopt_lock, node->_dataMutex};

	co_await node->_resizeFile(size);
	co_return {};
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::allocate(int64_t offset, size_t size) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	co_await node->_dataMutex.async_lock();
	frg::unique_lock data_lock{frg::adopt_lock, node->_dataMutex};

	// TODO: Careful about overflow.
	if(offset + size <= node->_fileSize.load(std::memory_order_relaxed))
		co_return {};
	co_await node->_resizeFile(offset + size);
	co_return {};
}

async::result<int> MemoryFile::getFileFlags() {
	int ret = 0;
	if (flags_ & semanticNonBlock)
		ret |= O_NONBLOCK;

	if (flags_ & semanticWrite && flags_ & semanticRead)
		ret |= O_RDWR;
	else if (flags_ & semanticWrite)
		ret |= O_WRONLY;
	else if (flags_ & semanticRead)
		ret |= O_RDONLY;

	if (flags_ & semanticAppend)
		ret |= O_APPEND;

	co_return ret;
}

FutureMaybe<helix::UniqueDescriptor>
MemoryFile::accessMemory() {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	co_await node->_dataMutex.async_lock_shared();
	frg::shared_lock data_lock{frg::adopt_lock, node->_dataMutex};

	co_return node->_memory.dup();
}

// ----------------------------------------------------------------------------

InheritedNode::InheritedNode(Superblock *superblock, std::string path)
: Node{superblock}, _path{std::move(path)} { }

// ----------------------------------------------------------------------------
// DirectoryNode and DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("tmpfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())} { }

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
DirectoryFile::readEntries() {
	// '.' and '..' are not stored in _entries; synthesize them before iterating.
	if(_dots != DotEntriesPhase::done) {
		// The parent of the root directory is the root itself.
		auto owner = associatedLink()->getParentNode();
		auto parent = owner ? static_cast<Node *>(owner.get()) : static_cast<Node *>(_node);
		if(auto entry = nextDotEntry(_dots, _node->inodeNumber(), parent->inodeNumber()); entry)
			co_return *entry;
	}

	std::string name;
	smarter::shared_ptr<FsNode> targetPtr;
	{
		std::shared_lock lock{_node->_dirMutex};
		// Continue after the last name such that concurrent insertions and removals are tolerated.
		auto it = _lastName ? _node->_entries.upper_bound(*_lastName) : _node->_entries.begin();
		if(it != _node->_entries.end()) {
			name = (*it)->nameLocked();
			targetPtr = (*it)->getTarget();
		}
	}

	if(targetPtr) {
		auto target = static_cast<Node *>(targetPtr.get());
		auto type = target->getType();
		_lastName = name;
		_numEntries++;

		int64_t fileType = managarm::fs::FileType::REGULAR;

		switch(type) {
		case VfsType::null:
		case VfsType::regular:
			break;
		case VfsType::directory:
			fileType = managarm::fs::FileType::DIRECTORY;
			break;
		case VfsType::symlink:
			fileType = managarm::fs::FileType::SYMLINK;
			break;
		case VfsType::charDevice:
			fileType = managarm::fs::FileType::CHAR_DEVICE;
			break;
		case VfsType::blockDevice:
			fileType = managarm::fs::FileType::BLOCK_DEVICE;
			break;
		case VfsType::socket:
			fileType = managarm::fs::FileType::SOCKET;
			break;
		case VfsType::fifo:
			fileType = managarm::fs::FileType::FIFO;
			break;
		}

		co_return protocols::fs::ReadEntriesResult{
			.name = name,
			.inode = static_cast<ino_t>(target->inodeNumber()),
			.offset = static_cast<long>(2 + _numEntries),
			.fileType = fileType
		};
	}else{
		co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);
	}
}

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// Node implementation.
// ----------------------------------------------------------------------------

Node::Node(Superblock *superblock, FsNode::DefaultOps default_ops)
: FsNode{superblock, default_ops} {
	_inodeNumber = superblock->allocateInode();
	auto time = clk::getRealtime();
	_atime = time;
	_mtime = time;
	_ctime = time;
}

// ----------------------------------------------------------------------------
// SymlinkNode implementation.
// ----------------------------------------------------------------------------

SymlinkNode::SymlinkNode(Superblock *superblock, std::string link)
: Node{superblock}, _link(std::move(link)) { }

// ----------------------------------------------------------------------------
// DeviceNode implementation.
// ----------------------------------------------------------------------------

DeviceNode::DeviceNode(Superblock *superblock, VfsType type, DeviceId id)
: Node{superblock}, _type(type), _id(id) {
	assert(type == VfsType::charDevice || type == VfsType::blockDevice);
}

// ----------------------------------------------------------------------------
// SocketNode implementation.
// ----------------------------------------------------------------------------

SocketNode::SocketNode(Superblock *superblock, mode_t mode, uid_t uid, gid_t gid)
: Node{superblock} {
	initializeMode(mode);
	initializeOwner(uid, gid);
}

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

smarter::shared_ptr<Link, LinkRc> DirectoryNode::createRootDirectory(Superblock *superblock, int mode, uid_t uid, gid_t gid) {
	auto node = makeFsShared<DirectoryNode>(superblock, mode, uid, gid);
	auto link = makeFsShared<Link>(std::move(node));
	return link;
}

DirectoryNode::DirectoryNode(Superblock *superblock, int mode, uid_t uid, gid_t gid)
: Node{superblock, FsNode::defaultSupportsObservers} {
	initializeMode(mode);
	initializeOwner(uid, gid);
	// '.' and the entry in the parent give a fresh directory two links.
	adjustLinkCount(1);
}

template<typename MakeNode>
std::expected<std::pair<smarter::shared_ptr<Link, LinkRc>, bool>, Error>
DirectoryNode::getOrInsert(FsLink *parent, const std::string &name, bool isDirectory,
		MakeNode makeNode) {
	smarter::shared_ptr<Link, LinkRc> link;
	{
		std::shared_lock topology_lock{topologyMutexOf(superblock())};
		std::unique_lock dir_lock{_dirMutex};
		if(_dead)
			return std::unexpected{Error::noSuchFile};
		if(auto it = _entries.find(name); it != _entries.end())
			return std::pair{*it, false};
		link = makeFsShared<Link>(parent->sharedFromThis(), name, makeNode());
		_entries.insert(link);
		// Account for the new subdirectory's '..' backlink.
		if(isDirectory)
			adjustLinkCount(1);
	}
	return std::pair{std::move(link), true};
}

template<typename MakeNode>
std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
DirectoryNode::insert(FsLink *parent, const std::string &name, bool isDirectory,
		MakeNode makeNode) {
	auto result = getOrInsert(parent, name, isDirectory, std::move(makeNode));
	if(!result)
		return std::unexpected{result.error()};
	if(!result->second)
		return std::unexpected{Error::alreadyExists};
	return std::move(result->first);
}

async::result<std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error>>
DirectoryNode::getLinkOrCreate(FsLink *parent, Process *, std::string name, mode_t mode,
		bool exclusive) {
	auto result = getOrInsert(parent, name, false, [&] {
		return makeFsShared<MemoryNode>(static_cast<Superblock *>(superblock()), mode);
	});
	if(!result)
		co_return std::unexpected{result.error()};
	auto [link, inserted] = std::move(*result);
	if(!inserted) {
		if(exclusive)
			co_return std::unexpected{Error::alreadyExists};
		co_return link;
	}
	notifyObservers(FsObserver::createEvent, name, 0, false);
	co_return link;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
DirectoryNode::link(FsLink *parent, std::string name, smarter::shared_ptr<FsNode> target) {
	// Directories have a single parent, such that the directory tree stays acyclic.
	if(target->getType() == VfsType::directory)
		co_return Error::insufficientPermissions;
	auto result = insert(parent, name, false, [&] {
		static_cast<Node *>(target.get())->adjustLinkCount(1);
		return target;
	});
	if(!result)
		co_return result.error();
	co_return smarter::shared_ptr<FsLink, LinkRc>{std::move(*result)};
}

async::result<frg::expected<Error>> DirectoryNode::unlink(std::string name) {
	// Dropped after releasing the locks.
	EntrySet::node_type removed;
	{
		std::shared_lock topology_lock{topologyMutexOf(superblock())};
		std::unique_lock dir_lock{_dirMutex};
		auto it = _entries.find(name);
		if(it == _entries.end())
			co_return Error::noSuchFile;

		auto target = it->get()->getTarget();
		if(target->getType() == VfsType::directory)
			co_return Error::directoryNotEmpty;

		static_cast<Node *>(target.get())->adjustLinkCount(-1);
		removed = _entries.extract(it);
	}

	notifyObservers(FsObserver::deleteEvent, name, 0);
	co_return {};
}

async::result<frg::expected<Error>> DirectoryNode::rmdir(std::string name) {
	// Dropped after releasing the locks.
	EntrySet::node_type removed;
	{
		std::shared_lock topology_lock{topologyMutexOf(superblock())};
		std::unique_lock dir_lock{_dirMutex};
		auto it = _entries.find(name);
		if(it == _entries.end())
			co_return Error::noSuchFile;

		auto target = it->get()->getTarget();
		if(target->getType() != VfsType::directory)
			co_return Error::notDirectory;

		auto dir_target = static_cast<DirectoryNode *>(target.get());
		std::unique_lock child_lock{dir_target->_dirMutex};
		if(!dir_target->_entries.empty())
			co_return Error::directoryNotEmpty;
		dir_target->_dead = true;

		removed = _entries.extract(it);
		// Drop the removed subdirectory's '..' backlink.
		adjustLinkCount(-1);
	}

	notifyObservers(FsObserver::deleteEvent, name, 0, true);
	co_return {};
}

async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
DirectoryNode::mkdir(FsLink *parent, Process *proc, std::string name, mode_t mode) {
	auto umask = proc ? proc->fsContext()->getUmask() : 0;
	auto uid = proc ? proc->threadGroup()->uid() : 0;
	auto gid = proc ? proc->threadGroup()->gid() : 0;

	auto result = createDirectory(parent, std::move(name), mode & ~umask, uid, gid);
	if(!result)
		co_return result.error();
	co_return smarter::shared_ptr<FsLink, LinkRc>{std::move(*result)};
}

async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
DirectoryNode::symlink(FsLink *parent, std::string name, std::string path) {
	auto result = insert(parent, name, false, [&] {
		return makeFsShared<SymlinkNode>(static_cast<Superblock *>(superblock()),
				std::move(path));
	});
	if(!result)
		co_return result.error();
	co_return smarter::shared_ptr<FsLink, LinkRc>{std::move(*result)};
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
DirectoryNode::mkdev(FsLink *parent, std::string name, VfsType type, DeviceId id) {
	auto result = createDevice(parent, std::move(name), type, id);
	if(!result)
		co_return result.error();
	co_return smarter::shared_ptr<FsLink, LinkRc>{std::move(*result)};
}

smarter::shared_ptr<Link, LinkRc> DirectoryNode::lookup(const std::string &name) {
	std::shared_lock lock{_dirMutex};
	auto it = _entries.find(name);
	if(it == _entries.end())
		return nullptr;
	return *it;
}

std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
DirectoryNode::createDirectory(FsLink *parent, std::string name, int mode, uid_t uid, gid_t gid) {
	auto result = insert(parent, name, true, [&] {
		return makeFsShared<DirectoryNode>(static_cast<Superblock *>(superblock()), mode, uid, gid);
	});
	if(!result)
		return std::unexpected{result.error()};
	notifyObservers(FsObserver::createEvent, name, 0, true);
	return std::move(*result);
}

std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
DirectoryNode::getOrCreateDirectory(FsLink *parent, std::string name, int mode, uid_t uid, gid_t gid) {
	auto result = getOrInsert(parent, name, true, [&] {
		return makeFsShared<DirectoryNode>(static_cast<Superblock *>(superblock()), mode, uid, gid);
	});
	if(!result)
		return std::unexpected{result.error()};
	auto [link, inserted] = std::move(*result);
	if(inserted)
		notifyObservers(FsObserver::createEvent, name, 0, true);
	return std::move(link);
}

std::expected<smarter::shared_ptr<Link, LinkRc>, Error>
DirectoryNode::createDevice(FsLink *parent, std::string name, VfsType type, DeviceId id) {
	auto result = insert(parent, name, false, [&] {
		return makeFsShared<DeviceNode>(static_cast<Superblock *>(superblock()), type, id);
	});
	if(!result)
		return std::unexpected{result.error()};
	notifyObservers(FsObserver::createEvent, name, 0);
	return std::move(*result);
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
DirectoryNode::mkfifo(FsLink *parent, std::string name, mode_t mode) {
	auto result = insert(parent, name, false, [&] {
		return makeFsShared<FifoNode>(static_cast<Superblock *>(superblock()), mode);
	});
	if(!result)
		co_return result.error();
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return smarter::shared_ptr<FsLink, LinkRc>{std::move(*result)};
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> DirectoryNode::mksocket(FsLink *parent, std::string name, mode_t mode, uid_t uid, gid_t gid) {
	auto result = insert(parent, name, false, [&] {
		return makeFsShared<SocketNode>(static_cast<Superblock *>(superblock()), mode, uid, gid);
	});
	if(!result)
		co_return result.error();
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return smarter::shared_ptr<FsLink, LinkRc>{std::move(*result)};
}

} // anonymous namespace

std::expected<void, Error>
createDevtmpfsNode(FsLink *root, std::string_view path, VfsType type, DeviceId id) {
	auto dirLink = root->sharedFromThis();
	size_t k = 0;
	while(true) {
		auto dir = static_cast<DirectoryNode *>(dirLink->getTarget().get());
		size_t s = path.find('/', k);
		if(s == std::string_view::npos) {
			auto result = dir->createDevice(dirLink.get(), std::string{path.substr(k)}, type, id);
			if(!result)
				return std::unexpected{result.error()};
			return {};
		}

		assert(s > k);
		std::string name{path.substr(k, s - k)};
		auto result = dir->getOrCreateDirectory(dirLink.get(), name, 0755, 0, 0);
		if(!result)
			return std::unexpected{result.error()};
		smarter::shared_ptr<FsLink, LinkRc> link = std::move(*result);
		// devtmpfs is writable, hence userspace may have put non-directories (incl. symlinks) here.
		if(link->getTarget()->getType() != VfsType::directory)
			return std::unexpected{Error::notDirectory};
		dirLink = std::move(link);
		k = s + 1;
	}
}

// Ironically, this function does not create a MemoryNode.
smarter::shared_ptr<FsNode> createMemoryNode(std::string path) {
	return makeFsShared<InheritedNode>(new Superblock{}, std::move(path));
}

std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error> createRoot(Process *p, std::string options) {
	std::optional<uid_t> uid = std::nullopt;
	std::optional<gid_t> gid = std::nullopt;
	std::optional<mode_t> mode = std::nullopt;

	// These two options use custom suffixes (k for 1000, M for Megabyte, % for fractions of memory)
	// For now, we parse them but ignore their values.
	// TODO: respect these values.
	std::optional<std::string> size = std::nullopt;
	std::optional<std::string> nr_inodes = std::nullopt;

	auto opts = std::make_tuple(
	    mount_options::MountOption{
	        "uid",
	        mount_options::parse_numeric<uid_t, 10>,
			std::ref(uid)
	    },
	    mount_options::MountOption{
	        "gid",
	        mount_options::parse_numeric<gid_t, 10>,
			std::ref(gid)
	    },
	    mount_options::MountOption{
	        "mode",
	        mount_options::parse_numeric<mode_t, 8>,
			std::ref(mode)
	    },
	    mount_options::MountOption{
	        "size",
	        mount_options::parse_string,
			std::ref(size)
	    },
	    mount_options::MountOption{
	        "nr_inodes",
			mount_options::parse_string,
	        std::ref(nr_inodes)
	    }
	);

	auto result = mount_options::parse(options, opts);
	if (!result) {
		std::println("posix: error parsing mount options '{}': {}", options, result.error());
		return std::unexpected(Error::illegalArguments);
	}

	auto dir = DirectoryNode::createRootDirectory(
	    new Superblock{},
		// mounting tmpfs does not take umask into account
	    mode.value_or(01777),
		// On Linux, fsuid and fsgid are used instead.
	    uid.value_or(p ? p->threadGroup()->uid() : 0),
	    gid.value_or(p ? p->threadGroup()->gid() : 0)
	);

	return dir;
}

smarter::shared_ptr<FsLink, LinkRc> createDevTmpFsRoot() {
	return DirectoryNode::createRootDirectory(new Superblock{"devtmpfs"}, 01777, 0, 0);
}

} // namespace tmp_fs
