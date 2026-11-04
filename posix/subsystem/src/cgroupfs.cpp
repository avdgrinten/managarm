#include <linux/magic.h>
#include <string.h>
#include <shared_mutex>
#include <sstream>

#include <core/clock.hpp>
#include <helix/dispatcher-pool.hpp>
#include "common.hpp"
#include "cgroupfs.hpp"
#include "process.hpp"

#include <bitset>

namespace cgroupfs {

SuperBlock cgroupfsSuperblock;

namespace {

// Leaf lock; unlinked entries are destroyed after releasing it.
std::shared_mutex treeMutex;

} // anonymous namespace

// ----------------------------------------------------------------------------
// LinkCompare implementation.
// ----------------------------------------------------------------------------

bool LinkCompare::operator() (const smarter::shared_ptr<Link, LinkRc> &a, const smarter::shared_ptr<Link, LinkRc> &b) const {
	return a->getName() < b->getName();
}

bool LinkCompare::operator() (const smarter::shared_ptr<Link, LinkRc> &link, const std::string &name) const {
	return link->getName() < name;
}

bool LinkCompare::operator() (const std::string &name, const smarter::shared_ptr<Link, LinkRc> &link) const {
	return name < link->getName();
}

// ----------------------------------------------------------------------------
// RegularFile implementation.
// ----------------------------------------------------------------------------

void RegularFile::serve(smarter::shared_ptr<RegularFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

RegularFile::RegularFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("cgroupfs.file"), std::move(mount), std::move(link)},
		_cached{false}, _offset{0} { }

void RegularFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>> RegularFile::seek(off_t offset, VfsSeek whence) {
	if(whence == VfsSeek::relative)
		_offset = _offset + offset;
	else if(whence == VfsSeek::absolute)
		_offset = offset;
	else if(whence == VfsSeek::eof)
		// TODO: Unimplemented!
		assert(whence == VfsSeek::eof);
	co_return _offset;
}

async::result<std::expected<size_t, Error>>
RegularFile::readSome(Process *, void *data, size_t max_length, async::cancellation_token) {
	assert(max_length > 0);

	if(!_cached) {
		assert(!_offset);
		auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
		_buffer = co_await node->show();
		_cached = true;
	}

	assert(_offset <= _buffer.size());
	size_t chunk = std::min(_buffer.size() - _offset, max_length);
	memcpy(data, _buffer.data() + _offset, chunk);
	_offset += chunk;
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
RegularFile::writeAll(Process *, const void *data, size_t length) {
	assert(length > 0);

	auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
	co_await node->store(std::string{reinterpret_cast<const char *>(data), length});
	co_return length;
}

helix::BorrowedDescriptor RegularFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("cgroupfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())} { }

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> DirectoryFile::readEntries() {
	if(auto entry = nextDotEntry(_dots, 0, 0); entry)
		co_return *entry;

	std::string name;
	{
		std::shared_lock lock{treeMutex};
		// Continue after the last name such that concurrent insertions and removals are tolerated.
		auto it = _lastName ? _node->_entries.upper_bound(*_lastName) : _node->_entries.begin();
		if(it == _node->_entries.end())
			co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);
		name = (*it)->getName();
	}
	_lastName = name;
	_numEntries++;

	co_return protocols::fs::ReadEntriesResult{
		.name = name,
		.inode = 0,
		.offset = static_cast<long>(2 + _numEntries)
	};
}

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// Link implementation.
// ----------------------------------------------------------------------------

Link::Link(smarter::shared_ptr<FsNode> target)
: _target{std::move(target)} { }

Link::Link(smarter::shared_ptr<FsLink, LinkRc> owner, std::string name, smarter::shared_ptr<FsNode> target)
: _owner{std::move(owner)}, _name{std::move(name)}, _target{std::move(target)} {
	assert(_owner);
	assert(!_name.empty());
}

smarter::shared_ptr<FsLink, LinkRc> Link::getParent() {
	return _owner;
}

std::string Link::getName() {
	// The root link does not have a name.
	assert(_owner);
	return _name;
}

smarter::shared_ptr<FsNode> Link::getTarget() {
	return _target;
}

// ----------------------------------------------------------------------------
// RegularNode implementation.
// ----------------------------------------------------------------------------

RegularNode::RegularNode() : FsNode(&cgroupfsSuperblock, 0) {}

VfsType RegularNode::getType() {
	return VfsType::regular;
}

async::result<frg::expected<Error, FileStats>> RegularNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

    // TODO CGROUPFS: FIX!
	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 4096; // Same as in Linux.
	stats.mode = 0666; // TODO: Some files can be written.
	stats.uid = 0;
	stats.gid = 0;
	stats.atimeSecs = now.tv_sec;
	stats.atimeNanos = now.tv_nsec;
	stats.mtimeSecs = now.tv_sec;
	stats.mtimeNanos = now.tv_nsec;
	stats.ctimeSecs = now.tv_sec;
	stats.ctimeNanos = now.tv_nsec;
	co_return stats;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
RegularNode::open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: cgroupfs RegularNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<RegularFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	RegularFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

FutureMaybe<smarter::shared_ptr<FsNode>> SuperBlock::createRegular(Process *) {
	std::cout << "posix: createRegular on cgroupfs Superblock unsupported" << std::endl;
	co_return nullptr;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
SuperBlock::rename(FsLink *, FsLink *, std::string) {
	co_return Error::noSuchFile;
};

async::result<frg::expected<Error, FsStats>> SuperBlock::getFsStats() {
	FsStats stats{};
	stats.fsType = CGROUP2_SUPER_MAGIC;
	co_return stats;
}

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

smarter::shared_ptr<Link, LinkRc> DirectoryNode::createRootDirectory() {
	auto node = makeFsShared<DirectoryNode>();
	auto the_node = node.get();
	auto link = makeFsShared<Link>(std::move(node));

	the_node->createCgroupFiles(link.get());

	return link;
}

DirectoryNode::DirectoryNode()
: FsNode{&cgroupfsSuperblock} { }

smarter::shared_ptr<Link, LinkRc> DirectoryNode::directMkregular(FsLink *parent, std::string name,
		smarter::shared_ptr<RegularNode> regular) {
	auto link = makeFsShared<Link>(parent->sharedFromThis(), name, std::move(regular));
	std::unique_lock lock{treeMutex};
	assert(_entries.find(name) == _entries.end());
	_entries.insert(link);
	return link;
}

async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>> DirectoryNode::mkdir(FsLink *parent, Process *, std::string name, mode_t) {
	auto link = createCgroupDirectory(parent, std::move(name));
	if(!link)
		co_return Error::alreadyExists;
	co_return link;
}

smarter::shared_ptr<Link, LinkRc> DirectoryNode::directMknode(FsLink *parent, std::string name, smarter::shared_ptr<FsNode> node) {
	auto link = makeFsShared<Link>(parent->sharedFromThis(), name, std::move(node));
	std::unique_lock lock{treeMutex};
	assert(_entries.find(name) == _entries.end());
	_entries.insert(link);
	return link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> DirectoryNode::link(FsLink *, std::string,
		smarter::shared_ptr<FsNode>) {
	co_return Error::noSuchFile;
}

async::result<frg::expected<Error, FileStats>> DirectoryNode::getStats() {
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 0; // Same as in Linux.
	stats.mode = 0644; // TODO: Some files can be written.
	stats.uid = 0;
	stats.gid = 0;
	// TODO: Verify the times are correct and fix them
	stats.atimeSecs = now.tv_sec;
	stats.atimeNanos = now.tv_nsec;
	stats.mtimeSecs = now.tv_sec;
	stats.mtimeNanos = now.tv_nsec;
	stats.ctimeSecs = now.tv_sec;
	stats.ctimeNanos = now.tv_nsec;
	co_return stats;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
DirectoryNode::open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: cgroup DirectoryNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	DirectoryFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> DirectoryNode::getLink(FsLink *, std::string name) {
	smarter::shared_ptr<Link, LinkRc> link;
	{
		std::shared_lock lock{treeMutex};
		auto it = _entries.find(name);
		if(it == _entries.end())
			co_return Error::noSuchFile;
		link = *it;
	}
	co_return link;
}

async::result<frg::expected<Error>> DirectoryNode::unlink(std::string name) {
	decltype(_entries)::node_type entry;
	{
		std::unique_lock lock{treeMutex};
		auto it = _entries.find(name);
		if (it == _entries.end())
			co_return Error::noSuchFile;
		entry = _entries.extract(it);
	}
	co_return frg::expected<Error>{};
}

smarter::shared_ptr<Link, LinkRc> DirectoryNode::createCgroupDirectory(FsLink *parent, std::string name) {
	auto node = makeFsShared<DirectoryNode>();
	auto cgroup_dir = node.get();
	auto link = makeFsShared<Link>(parent->sharedFromThis(), name, std::move(node));

	// Populate the directory before it becomes visible, such that concurrent mkdir()s cannot
	// take the names of its files.
	cgroup_dir->createCgroupFiles(link.get());

	// The files keep the link alive, hence a directory that loses the race is torn down.
	// They are dropped after unlocking.
	decltype(_entries) orphans;
	std::unique_lock lock{treeMutex};
	if(_entries.find(name) != _entries.end()) {
		orphans = std::move(cgroup_dir->_entries);
		return nullptr;
	}
	_entries.insert(link);
	return link;
}

void DirectoryNode::createCgroupFiles(FsLink *parent) {
	this->directMkregular(parent, "cgroup.procs", makeFsShared<ProcsNode>());
	this->directMkregular(parent, "cgroup.controllers", makeFsShared<ControllersNode>());
}

async::result<std::string> ProcsNode::show() {
	// Everything that has a value of N/A is not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "";
	co_return stream.str();
}

async::result<void> ProcsNode::store(std::string string) {
	// TODO: proper error reporting.
	std::cout << "posix: writing to cgroup.procs with: " << string << std::endl;
	co_return;
}

async::result<std::string> ControllersNode::show() {
	// Everything that has a value of N/A is not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "";
	co_return stream.str();
}

async::result<void> ControllersNode::store(std::string string) {
	// TODO: proper error reporting.
	std::cout << "posix: writing to cgroup.procs with: " << string << std::endl;
	co_return;
}

LinkNode::LinkNode()
: FsNode{&cgroupfsSuperblock} { }

} // namespace cgroupfs

smarter::shared_ptr<FsLink, LinkRc> getCgroupfs() {
	static smarter::shared_ptr<FsLink, LinkRc> cgroupfs = cgroupfs::DirectoryNode::createRootDirectory();
	return cgroupfs;
}
