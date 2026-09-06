
#include <async/cancellation.hpp>
#include <linux/magic.h>
#include <string.h>

#include <core/clock.hpp>
#include "common.hpp"
#include "sysfs.hpp"

#include <bitset>

namespace {

sysfs::SysfsSuperblock sysfsSuperblock{};

} // namespace

namespace sysfs {

FutureMaybe<smarter::shared_ptr<FsNode>> SysfsSuperblock::createRegular(Process *) {
	std::cout << "posix: createRegular on SysfsSuperblock unsupported" << std::endl;
	co_return nullptr;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
SysfsSuperblock::rename(FsLink *, FsLink *, std::string) {
	co_return Error::noSuchFile;
};

async::result<frg::expected<Error, FsStats>> SysfsSuperblock::getFsStats() {
	FsStats stats{};
	stats.fsType = SYSFS_MAGIC;
	co_return stats;
}

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
// Attribute implementation.
// ----------------------------------------------------------------------------

async::result<Error> Attribute::store(Object *, std::string) {
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, helix::UniqueDescriptor>> Attribute::accessMemory(Object *) {
	co_return Error::noBackingDevice;
}

// ----------------------------------------------------------------------------
// AttributeFile implementation.
// ----------------------------------------------------------------------------

void AttributeFile::serve(smarter::shared_ptr<AttributeFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

AttributeFile::AttributeFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("sysfs.attr"), std::move(mount), std::move(link)},
		_cached{false}, _offset{0} { }

void AttributeFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>> AttributeFile::seek(off_t offset, VfsSeek whence) {
	// TODO: it's unclear whether we should allow seeks past the end.
	// TODO: re-cache the file for seeks to zero.
	if(whence == VfsSeek::relative)
		_offset = _offset + offset;
	else if(whence == VfsSeek::absolute)
		_offset = offset;
	else if(whence == VfsSeek::eof)
		// TODO: Unimplemented!
		assert(!"unimplemented");
	co_return _offset;
}

async::result<std::expected<size_t, Error>>
AttributeFile::readSome(Process *process, void *data, size_t max_length, async::cancellation_token) {
	auto ret = co_await pread(process, _offset, data, max_length);

	if(ret.has_value())
		_offset += ret.value();

	co_return ret;
}

async::result<std::expected<size_t, Error>>
AttributeFile::pread(Process *, int64_t offset, void *buffer, size_t length) {
	assert(length > 0);

	if(!_cached) {
		auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());
		// TODO(geert): Don't assume this doesn't block.
		if(auto res = co_await node->_attr->show(node->_object); res) {
			_buffer = res.value();
			_cached = true;
		} else
			co_return std::unexpected{res.error() | protocols::fs::toFsProtoError | toPosixError};
	}

	if (_offset >= _buffer.size())
		co_return std::unexpected{Error::eof};
	size_t chunk = std::min(_buffer.size() - offset, length);
	memcpy(buffer, _buffer.data() + _offset, chunk);
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
AttributeFile::writeAll(Process *, const void *data, size_t length)  {
	assert(length > 0);

	auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());
	auto err = co_await node->_attr->store(node->_object,
			std::string{reinterpret_cast<const char *>(data), length});

	if(err != Error::success)
		co_return err;

	co_return length;
}

FutureMaybe<helix::UniqueDescriptor> AttributeFile::accessMemory() {
	auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());

	auto res = co_await node->_attr->accessMemory(node->_object);
	assert(res);

	co_return std::move(res.value());
}

helix::BorrowedDescriptor AttributeFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("sysfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

// TODO: Verify that this is correct
async::result<frg::expected<Error, off_t>> DirectoryFile::seek(off_t, VfsSeek) {
	co_return Error::illegalArguments;
}

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> DirectoryFile::readEntries() {
	if(auto entry = nextDotEntry(_dots, 0, 0); entry)
		co_return *entry;

	if(_iter != _node->_entries.end()) {
		auto name = (*_iter)->getName();
		_iter++;

		co_return protocols::fs::ReadEntriesResult{
			.name = name,
			.inode = 0,
			.offset = 2 + std::distance(_node->_entries.begin(), _iter)
		};
	}else{
		co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);
	}
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
// AttributeNode implementation.
// ----------------------------------------------------------------------------

AttributeNode::AttributeNode(Object *object, Attribute *attr)
: FsNode(&sysfsSuperblock), _object{object}, _attr{attr} {
	inode_ = static_cast<SysfsSuperblock *>(superblock())->inodeAllocator().allocate();
}

VfsType AttributeNode::getType() {
	return VfsType::regular;
}

async::result<frg::expected<Error, FileStats>> AttributeNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = inode_;
	stats.numLinks = 1;
	stats.fileSize = _attr->size();
	stats.mode = _attr->writable() ? 0666 : 0444; // TODO: Some files can be written.
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
AttributeNode::open(Process *, std::shared_ptr<MountView> mount,
		smarter::shared_ptr<FsLink, LinkRc> link, SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: sysfs AttributeNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<AttributeFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	AttributeFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

// ----------------------------------------------------------------------------
// SymlinkNode implementation.
// ----------------------------------------------------------------------------

SymlinkNode::SymlinkNode(std::weak_ptr<Object> target)
: FsNode(&sysfsSuperblock), _target{std::move(target)} {
	inode_ = static_cast<SysfsSuperblock *>(superblock())->inodeAllocator().allocate();
}

VfsType SymlinkNode::getType() {
	return VfsType::symlink;
}

async::result<frg::expected<Error, FileStats>> SymlinkNode::getStats() {
	auto fs = FileStats{};
	fs.inodeNumber = inode_;
	fs.numLinks = 1;
	fs.mode = 0777;
	co_return fs;
}

expected<std::string> SymlinkNode::readSymlink(FsLink *link, Process *) {
	auto object = _target.lock();
	assert(object);

	std::string path;

	// Walk from the target to the root to discover the path.
	for(smarter::shared_ptr<FsLink, LinkRc> ref = object->dirLink(); ref->getParent(); ref = ref->getParent())
		path = path.empty() ? ref->getName() : ref->getName() + "/" + path;

	// Walk from the symlink to the root to discover the number of ../ prefixes.
	for(auto ref = link->getParent(); ref->getParent(); ref = ref->getParent())
		path = "../" + path;

	co_return path;
}

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

smarter::shared_ptr<Link, LinkRc> DirectoryNode::createRootDirectory() {
	auto node = makeFsShared<DirectoryNode>();
	auto link = makeFsShared<Link>(std::move(node));
	return link;
}

DirectoryNode::DirectoryNode()
: FsNode(&sysfsSuperblock) {
	inode_ = static_cast<SysfsSuperblock *>(superblock())->inodeAllocator().allocate();
}

smarter::shared_ptr<Link, LinkRc> DirectoryNode::directMkattr(FsLink *parent, Object *object, Attribute *attr) {
	assert(_entries.find(attr->name()) == _entries.end());
	auto node = makeFsShared<AttributeNode>(object, attr);
	auto link = makeFsShared<Link>(parent->sharedFromThis(), attr->name(), std::move(node));
	_entries.insert(link);
	return link;
}

smarter::shared_ptr<Link, LinkRc> DirectoryNode::directMklink(FsLink *parent, std::string name, std::weak_ptr<Object> target) {
	assert(_entries.find(name) == _entries.end());
	auto node = makeFsShared<SymlinkNode>(std::move(target));
	auto link = makeFsShared<Link>(parent->sharedFromThis(), std::move(name), std::move(node));
	_entries.insert(link);
	return link;
}

smarter::shared_ptr<Link, LinkRc> DirectoryNode::directMkdir(FsLink *parent, std::string name) {
	auto preexisting = _entries.find(name);
	if(preexisting != _entries.end()) {
		return *preexisting;
	}
	auto node = makeFsShared<DirectoryNode>();
	auto link = makeFsShared<Link>(parent->sharedFromThis(), std::move(name), std::move(node));
	_entries.insert(link);
	return link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, FileStats>> DirectoryNode::getStats() {
	auto fs = FileStats{};
	fs.inodeNumber = inode_;
	fs.numLinks = 2;
	fs.fileSize = 0;
	fs.mode = 0755;
	fs.uid = 0;
	fs.gid = 0;
	co_return fs;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
DirectoryNode::open(Process *, std::shared_ptr<MountView> mount,
		smarter::shared_ptr<FsLink, LinkRc> link, SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: sysfs DirectoryNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	DirectoryFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> DirectoryNode::getLink(FsLink *, std::string name) {
	auto it = _entries.find(name);
	if(it != _entries.end())
		co_return *it;
	co_return Error::noSuchFile;
}

// ----------------------------------------------------------------------------
// Attribute implementation
// ----------------------------------------------------------------------------

Attribute::Attribute(std::string name, bool writable)
: _name{std::move(name)}, _writable{writable} { }

Attribute::Attribute(std::string name, bool writable, size_t size)
: _size{size}, _name{std::move(name)}, _writable{writable} { }

// ----------------------------------------------------------------------------
// Object implementation
// ----------------------------------------------------------------------------

Object::Object(std::shared_ptr<Object> parent, std::string name)
: _parent{std::move(parent)}, _name{std::move(name)} { }

smarter::shared_ptr<DirectoryNode> Object::directoryNode() {
	return smarter::static_pointer_cast<DirectoryNode>(_dirLink->getTarget());
}

void Object::realizeAttribute(Attribute *attr) {
	assert(_dirLink);
	auto dir = static_cast<DirectoryNode *>(_dirLink->getTarget().get());
	dir->directMkattr(_dirLink.get(), this, attr);
}

void Object::createSymlink(std::string name, std::shared_ptr<Object> target) {
	assert(_dirLink);
	auto dir = static_cast<DirectoryNode *>(_dirLink->getTarget().get());
	dir->directMklink(_dirLink.get(), std::move(name), std::move(target));
}

std::optional<std::string> Object::getClassPath() {
	return std::nullopt;
}

void Object::addObject() {
	if(_parent) {
		assert(_parent->_dirLink);
		auto parent_dir = static_cast<DirectoryNode *>(_parent->_dirLink->getTarget().get());
		_dirLink = parent_dir->directMkdir(_parent->_dirLink.get(), _name);
	}else{
		auto parent_dir = static_cast<DirectoryNode *>(getSysfs()->getTarget().get());
		_dirLink = parent_dir->directMkdir(getSysfs().get(), _name);
	}
}

} // namespace sysfs

smarter::shared_ptr<FsLink, LinkRc> getSysfs() {
	static smarter::shared_ptr<FsLink, LinkRc> sysfs = sysfs::DirectoryNode::createRootDirectory();
	return sysfs;
}

