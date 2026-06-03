#include <fcntl.h>
#include <linux/magic.h>
#include <unistd.h>
#include <set>

#include <core/clock.hpp>
#include <core/mount.hpp>
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
#include <sys/stat.h>

#include <bitset>

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __raw_map(int fd);

namespace tmp_fs {

namespace {

struct Superblock;

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

public:
	async::result<frg::expected<Error, FileStats>> getStats() override {
		FileStats stats{};
		stats.inodeNumber = _inodeNumber;
		stats.fileSize = 0;
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
		co_return stats;
	}

	int64_t inodeNumber() {
		return _inodeNumber;
	}

	int numLinks() {
		return _numLinks;
	}

	mode_t mode() {
		return _mode;
	}

	uid_t uid() {
		return _uid;
	}

	gid_t gid() {
		return _gid;
	}

	timespec atime() {
		return _atime;
	}

	timespec mtime() {
		return _mtime;
	}

	timespec ctime() {
		return _ctime;
	}

	async::result<Error> chmod(int mode) override {
		_mode = (_mode & 0xFFFFF000) | mode;
		co_return Error::success;
	}

	async::result<Error> utimensat(std::optional<timespec> atime, std::optional<timespec> mtime,
			timespec ctime) override {
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
		if(uid)
			_uid = *uid;
		if(gid)
			_gid = *gid;
		co_return {};
	}

private:
	int64_t _inodeNumber;
	int _numLinks = 1;
	mode_t _mode = 0;
	uid_t _uid = 0;
	gid_t _gid = 0;
	timespec _atime = {0, 0};
	timespec _mtime = {0, 0};
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
		FileStats stats{};
		stats.inodeNumber = inodeNumber();
		stats.fileSize = _link.size();
		stats.numLinks = numLinks();
		stats.mode = mode();
		stats.uid = uid();
		stats.gid = gid();
		stats.atimeSecs = atime().tv_sec;
		stats.atimeNanos = atime().tv_nsec;
		stats.mtimeSecs = mtime().tv_sec;
		stats.mtimeNanos = mtime().tv_nsec;
		stats.ctimeSecs = ctime().tv_sec;
		stats.ctimeNanos = ctime().tv_nsec;
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
			std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override {
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
			std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override {
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
	explicit Link(std::shared_ptr<FsNode> target)
	: _target(std::move(target)) { }

	explicit Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
	: _owner(std::move(owner)), _name(std::move(name)), _target(std::move(target)) {
		assert(_owner);
		assert(!_name.empty());
	}

	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		// The root link does not have a name.
		assert(_owner);
		return _name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

private:
	std::shared_ptr<FsNode> _owner;
	std::string _name;
	std::shared_ptr<FsNode> _target;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
		return link->getName() < name;
	}

	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
		return name < link->getName();
	}

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
		return a->getName() < b->getName();
	}
};

struct DirectoryNode;

struct DirectoryFile final : File {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

	void handleClose() override;

	FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::set<std::shared_ptr<Link>, LinkCompare>::iterator _iter;
};

struct DirectoryNode final : Node, std::enable_shared_from_this<DirectoryNode> {
	friend struct Superblock;
	friend struct DirectoryFile;

	static std::shared_ptr<Link> createRootDirectory(Superblock *superblock, int mode, uid_t uid, gid_t gid);

private:
	VfsType getType() override {
		return VfsType::directory;
	}

	std::shared_ptr<FsLink> treeLink() override {
		assert(_treeLink);
		return _treeLink;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
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

	async::result<std::expected<std::shared_ptr<FsLink>, Error>>
	getLinkOrCreate(Process *process, std::string name, mode_t mode, bool exclusive) override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override {
		auto it = _entries.find(name);
		if(it != _entries.end())
			co_return *it;
		co_return Error::noSuchFile;
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> link(std::string name,
			std::shared_ptr<FsNode> target) override {
		if(!(_entries.find(name) == _entries.end()))
			co_return Error::alreadyExists;
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(target));
		_entries.insert(link);
		co_return link;
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(Process *p, std::string name, mode_t mode) override;

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	symlink(std::string name, std::string path) override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkdev(std::string name,
			VfsType type, DeviceId id) override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkfifo(std::string name, mode_t mode) override;

	async::result<frg::expected<Error>> unlink(std::string name) override {
		auto it = _entries.find(name);
		if(it == _entries.end())
			co_return Error::noSuchFile;

		auto target = it->get()->getTarget();
		if(target->getType() == VfsType::directory)
			co_return Error::directoryNotEmpty;

		_entries.erase(it);

		notifyObservers(FsObserver::deleteEvent, name, 0);
		co_return {};
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mksocket(std::string name, mode_t mode, uid_t uid, gid_t gid) override;

	async::result<frg::expected<Error>> rmdir(std::string name) override {
		auto it = _entries.find(name);
		if(it == _entries.end())
			co_return Error::noSuchFile;

		auto target = it->get()->getTarget();
		if(target->getType() == VfsType::directory) {
			auto dir_target = reinterpret_cast<DirectoryNode *>(target.get());

			if(dir_target->_entries.size()) {
				co_return Error::directoryNotEmpty;
			}
		} else {
			co_return Error::notDirectory;
		}

		_entries.erase(it);

		notifyObservers(FsObserver::deleteEvent, name, 0, true);
		co_return {};
	}

public:
	DirectoryNode(Superblock *superblock, int mode, uid_t uid, gid_t gid);

private:
	// TODO: This creates a circular reference -- fix this.
	std::shared_ptr<Link> _treeLink;
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

// TODO: Remove this class in favor of MemoryNode.
struct InheritedNode final : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
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

struct MemoryFile final : File {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags flags)
	: File{FileKind::unknown,  StructName::get("tmpfs.regular"), std::move(mount), std::move(link)},
	flags_{flags}, _offset{0} { }

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t delta, VfsSeek whence) override;

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

	uint64_t _offset;
};

struct MemoryNode final : Node {
	friend struct MemoryFile;

	MemoryNode(Superblock *superblock);
	~MemoryNode();

	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
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
		FileStats stats{};
		stats.inodeNumber = inodeNumber();
		stats.fileSize = _fileSize;
		stats.numLinks = numLinks();
		stats.mode = mode();
		stats.uid = uid();
		stats.gid = gid();
		stats.atimeSecs = atime().tv_sec;
		stats.atimeNanos = atime().tv_nsec;
		stats.mtimeSecs = mtime().tv_sec;
		stats.mtimeNanos = mtime().tv_nsec;
		stats.ctimeSecs = ctime().tv_sec;
		stats.ctimeNanos = ctime().tv_nsec;
		co_return stats;
	}

private:
	async::result<void> _resizeFile(size_t new_size) {
		_fileSize = new_size;

		size_t aligned_size = (new_size + 0xFFF) & ~size_t(0xFFF);
		if(aligned_size <= _areaSize)
			co_return;

		if(_memory) {
			auto result = co_await helix_ng::resizeMemory(_memory, aligned_size);
			HEL_CHECK(result.error());
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateMemory(rootHierarchy(), aligned_size, 0, nullptr, &handle));
			_memory = helix::UniqueDescriptor{handle};
		}

		_mapping = helix::Mapping{_memory, 0, aligned_size};
		_areaSize = aligned_size;
	}

	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
	size_t _areaSize;
	size_t _fileSize;
};

struct Superblock final : FsSuperblock {
	Superblock(std::string name = "tmpfs") : fsType_{name} {
		deviceMinor_ = getUnnamedDeviceIdAllocator().allocate();
	}

	FutureMaybe<std::shared_ptr<FsNode>> createRegular(Process *) override {
		auto node = std::make_shared<MemoryNode>(this);
		co_return std::move(node);
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> rename(FsLink *src_fs_link,
			FsNode *dest_fs_dir, std::string dest_name) override {
		auto src_link = static_cast<Link *>(src_fs_link);

		if(dest_fs_dir->getType() != VfsType::directory)
			co_return Error::notDirectory;
		auto dest_dir = static_cast<DirectoryNode *>(dest_fs_dir);

		auto src_dir = static_cast<DirectoryNode *>(src_link->getOwner().get());
		auto it = src_dir->_entries.find(src_link->getName());
		if(it == src_dir->_entries.end() || it->get() != src_link)
			co_return Error::alreadyExists;

		// Unlink an existing link if such a link exists.
		if(auto dest_it = dest_dir->_entries.find(dest_name);
				dest_it != dest_dir->_entries.end())
			dest_dir->_entries.erase(dest_it);

		auto new_link = std::make_shared<Link>(dest_dir->shared_from_this(),
				std::move(dest_name), src_link->getTarget());
		src_dir->_entries.erase(it);
		dest_dir->_entries.insert(new_link);
		co_return new_link;
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
		return _inodeCounter++;
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

private:
	int64_t _inodeCounter = 1;

	std::string fsType_;
	unsigned int deviceMinor_;
};

// ----------------------------------------------------------------------------
// MemoryNode and MemoryFile implementation.
// ----------------------------------------------------------------------------

MemoryNode::MemoryNode(Superblock *superblock)
: Node{superblock, FsNode::defaultSupportsObservers}, _areaSize{0}, _fileSize{0} { }

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
	if(whence == VfsSeek::absolute) {
		_offset = delta;
	}else if(whence == VfsSeek::relative){
		_offset += delta;
	}else if(whence == VfsSeek::eof) {
		auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
		_offset = delta + node->_fileSize;
	} else {
		co_return Error::illegalArguments;
	}
	co_return _offset;
}

async::result<std::expected<size_t, Error>>
MemoryFile::readSome(Process *, void *buffer, size_t max_length, async::cancellation_token) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(!(_offset <= node->_fileSize))
		co_return std::unexpected{Error::eof};
	auto chunk = std::min(node->_fileSize - _offset, max_length);

	memcpy(buffer, reinterpret_cast<char *>(node->_mapping.get()) + _offset, chunk);
	_offset += chunk;
	node->notifyObservers(FsObserver::accessEvent, associatedLink()->getName(), 0);
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::writeAll(Process *, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if (!(flags_ & semanticWrite))
		co_return Error::badFileDescriptor;

	if(flags_ & semanticAppend)
		_offset = node->_fileSize;

	if(_offset + length > node->_fileSize)
		co_await node->_resizeFile(_offset + length);

	memcpy(reinterpret_cast<char *>(node->_mapping.get()) + _offset, buffer, length);
	_offset += length;
	node->notifyObservers(FsObserver::modifyEvent, associatedLink()->getName(), 0);
	co_return length;
}

async::result<std::expected<size_t, Error>>
MemoryFile::pread(Process *, int64_t offset, void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(static_cast<size_t>(offset) >= node->_fileSize)
		co_return std::unexpected{Error::eof};
	auto chunk = std::min(node->_fileSize - offset, length);

	memcpy(buffer, reinterpret_cast<char *>(node->_mapping.get()) + offset, chunk);

	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::pwrite(Process *, int64_t offset, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(offset + length > node->_fileSize)
		co_await node->_resizeFile(offset + length);

	memcpy(reinterpret_cast<char *>(node->_mapping.get()) + offset, buffer, length);
	co_return length;
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::truncate(size_t size) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	co_await node->_resizeFile(size);
	co_return {};
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::allocate(int64_t offset, size_t size) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	// TODO: Careful about overflow.
	if(offset + size <= node->_fileSize)
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

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{FileKind::unknown,  StructName::get("tmpfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
DirectoryFile::readEntries() {
	if(_iter != _node->_entries.end()) {
		auto name = (*_iter)->getName();
		_iter++;

		co_return protocols::fs::ReadEntriesResult{
			.name = name,
			.inode = 0,
			.offset = std::distance(_node->_entries.begin(), _iter)
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

std::shared_ptr<Link> DirectoryNode::createRootDirectory(Superblock *superblock, int mode, uid_t uid, gid_t gid) {
	auto node = std::make_shared<DirectoryNode>(superblock, mode, uid, gid);
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link;
	return link;
}

DirectoryNode::DirectoryNode(Superblock *superblock, int mode, uid_t uid, gid_t gid)
: Node{superblock, FsNode::defaultSupportsObservers} {
	initializeMode(mode);
	initializeOwner(uid, gid);
}

async::result<std::expected<std::shared_ptr<FsLink>, Error>>
DirectoryNode::getLinkOrCreate(Process *, std::string name, mode_t mode, bool exclusive) {
	auto linkResult = co_await getLink(name);
	if (linkResult && !exclusive)
		co_return linkResult.value();
	else if (linkResult && exclusive)
		co_return std::unexpected{Error::alreadyExists};

	auto node = std::make_shared<MemoryNode>(static_cast<Superblock *>(superblock()));
	co_await node->chmod(mode);
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	co_return link;
}

async::result<std::variant<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkdir(Process *proc, std::string name, mode_t mode) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;

	auto umask = proc ? proc->fsContext()->getUmask() : 0;
	auto uid = proc ? proc->threadGroup()->uid() : 0;
	auto gid = proc ? proc->threadGroup()->gid() : 0;

	auto node = std::make_shared<DirectoryNode>(static_cast<Superblock *>(superblock()), mode & ~umask, uid, gid);
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	the_node->_treeLink = link;
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0, true);
	co_return link;
}

async::result<std::variant<Error, std::shared_ptr<FsLink>>>
DirectoryNode::symlink(std::string name, std::string path) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<SymlinkNode>(static_cast<Superblock *>(superblock()),
			std::move(path));
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	co_return link;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkdev(std::string name, VfsType type, DeviceId id) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<DeviceNode>(static_cast<Superblock *>(superblock()),
			type, id);
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkfifo(std::string name, mode_t mode) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<FifoNode>(static_cast<Superblock *>(superblock()), mode);
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::mksocket(std::string name, mode_t mode, uid_t uid, gid_t gid) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<SocketNode>(static_cast<Superblock *>(superblock()), mode, uid, gid);
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}

} // anonymous namespace

// Ironically, this function does not create a MemoryNode.
std::shared_ptr<FsNode> createMemoryNode(std::string path) {
	return std::make_shared<InheritedNode>(new Superblock{}, std::move(path));
}

std::expected<std::shared_ptr<FsLink>, Error> createRoot(Process *p, std::string options) {
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

std::shared_ptr<FsLink> createDevTmpFsRoot() {
	return DirectoryNode::createRootDirectory(new Superblock{"devtmpfs"}, 01777, 0, 0);
}

} // namespace tmp_fs
