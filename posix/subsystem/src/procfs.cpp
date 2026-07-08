#include <async/cancellation.hpp>
#include <functional>
#include <linux/magic.h>
#include <print>
#include <string.h>
#include <sstream>
#include <iomanip>

#include <core/clock.hpp>
#include "common.hpp"
#include "procfs.hpp"
#include "process.hpp"
#include "protocols/fs/common.hpp"

#include <bitset>
#include <sys/epoll.h>

namespace procfs {

SuperBlock procfsSuperblock;

// ----------------------------------------------------------------------------
// LinkCompare implementation.
// ----------------------------------------------------------------------------

bool LinkCompare::operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
	return a->getName() < b->getName();
}

bool LinkCompare::operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
	return link->getName() < name;
}

bool LinkCompare::operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
	return name < link->getName();
}

// ----------------------------------------------------------------------------
// RegularFile implementation.
// ----------------------------------------------------------------------------

void RegularFile::serve(smarter::shared_ptr<RegularFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

RegularFile::RegularFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("procfs.attr"), std::move(mount), std::move(link)},
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

	// rewinding all the way invalidates caching; this is necessary for propagating errors like ESRCH
	if (_offset == 0)
		_cached = false;

	co_return _offset;
}

async::result<std::expected<size_t, Error>>
RegularFile::readSome(Process *process, void *data, size_t max_length, async::cancellation_token) {
	assert(max_length > 0);

	if(!_cached) {
		assert(!_offset);
		auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
		// TODO(geert): We assume this can't block (probably wrong).
		auto result = co_await node->show(process);
		if (!result)
			co_return std::unexpected{result.error()};

		_buffer = result.value();
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

async::result<frg::expected<Error, PollStatusResult>>
RegularFile::pollStatus(Process *) {
	co_return PollStatusResult{1, EPOLLIN};
}

async::result<frg::expected<Error, PollWaitResult>>
RegularFile::pollWait(Process *, uint64_t sequence, int mask,
		async::cancellation_token cancellation) {
	(void)mask;

	if(sequence > 1)
		co_return Error::illegalArguments;

	if(sequence)
		co_await async::suspend_indefinitely(cancellation);
	co_return PollWaitResult{1, EPOLLIN};
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
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: FileWithDefaults{FileKind::unknown,  StructName::get("procfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> DirectoryFile::readEntries() {
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
// Link implementation.
// ----------------------------------------------------------------------------

Link::Link(std::shared_ptr<FsNode> target)
: _target{std::move(target)} { }

Link::Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
: _owner{std::move(owner)}, _name{std::move(name)}, _target{std::move(target)} {
	assert(_owner);
	assert(!_name.empty());
}

std::shared_ptr<FsNode> Link::getOwner() {
	return _owner;
}

std::string Link::getName() {
	// The root link does not have a name.
	assert(_owner);
	return _name;
}

std::shared_ptr<FsNode> Link::getTarget() {
	return _target;
}

void Link::unlinkSelf() {
	assert(_target->getType() == VfsType::directory);

	auto node = std::static_pointer_cast<DirectoryNode>(_owner);
	auto err = node->directUnlink(_name);
	assert(err == Error::success);
}

// ----------------------------------------------------------------------------
// RegularNode implementation.
// ----------------------------------------------------------------------------

RegularNode::RegularNode() : FsNode(&procfsSuperblock, 0) {}

VfsType RegularNode::getType() {
	return VfsType::regular;
}

async::result<frg::expected<Error, FileStats>> RegularNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

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

async::result<frg::expected<Error, FileStats>> RegularNode::getStatsInternal(ThreadGroup *tg) {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 4096; // Same as in Linux.
	stats.mode = 0666; // TODO: Some files can be written.
	stats.uid = tg->uid();
	stats.gid = tg->gid();
	stats.atimeSecs = now.tv_sec;
	stats.atimeNanos = now.tv_nsec;
	stats.mtimeSecs = now.tv_sec;
	stats.mtimeNanos = now.tv_nsec;
	stats.ctimeSecs = now.tv_sec;
	stats.ctimeNanos = now.tv_nsec;
	co_return stats;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
RegularNode::open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: procfs RegularNode open() received illegal arguments:"
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

FutureMaybe<std::shared_ptr<FsNode>> SuperBlock::createRegular(Process *) {
	std::cout << "posix: createRegular on procfs Superblock unsupported" << std::endl;
	co_return nullptr;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
SuperBlock::rename(FsLink *, FsNode *, std::string) {
	co_return Error::noSuchFile;
};

async::result<frg::expected<Error, FsStats>> SuperBlock::getFsStats() {
	FsStats stats{};
	stats.fsType = PROC_SUPER_MAGIC;
	co_return stats;
}

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::createRootDirectory() {
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link.get();

	auto self_link = std::make_shared<Link>(the_node->shared_from_this(), "self", std::make_shared<SelfLink>());
	the_node->_entries.insert(std::move(self_link));

	auto self_thread_link = std::make_shared<Link>(the_node->shared_from_this(), "thread-self", std::make_shared<SelfThreadLink>());
	the_node->_entries.insert(std::move(self_thread_link));

	the_node->directMkregular("uptime", std::make_shared<UptimeNode>());
	the_node->directMknode("mounts", std::make_shared<MountsLink>());

	auto sysLink = the_node->directMkdir("sys");
	auto sys = std::static_pointer_cast<DirectoryNode>(sysLink->getTarget());
	auto kernelLink = sys->directMkdir("kernel");
	auto kernel = std::static_pointer_cast<DirectoryNode>(kernelLink->getTarget());
	auto randomLink = kernel->directMkdir("random");
	auto random = std::static_pointer_cast<DirectoryNode>(randomLink->getTarget());

	kernel->directMkregular("ostype", std::make_shared<OstypeNode>());
	kernel->directMkregular("osrelease", std::make_shared<OsreleaseNode>());
	kernel->directMkregular("arch", std::make_shared<ArchNode>());
	kernel->directMkregular("hostname", std::make_shared<HostnameNode>());

	random->directMkregular("boot_id", std::make_shared<BootIdNode>());

	return link;
}

DirectoryNode::DirectoryNode()
: FsNode{&procfsSuperblock}, _treeLink{nullptr} { }

std::shared_ptr<Link> DirectoryNode::directMkregular(std::string name,
		std::shared_ptr<RegularNode> regular) {
	assert(_entries.find(name) == _entries.end());
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(regular));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMkdir(std::string name) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	the_node->_treeLink = link.get();
	return link;
}

template<typename T>
requires requires (T t) { {t._treeLink} -> std::same_as<Link *&>; }
std::shared_ptr<Link> DirectoryNode::directMknodeDir(std::string name, std::shared_ptr<T> dirnode) {
	assert(_entries.find(name) == _entries.end());
	auto dirnodePtr = dirnode.get();
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(dirnode));
	dirnodePtr->_treeLink = link.get();
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMknode(std::string name, std::shared_ptr<FsNode> node) {
	assert(_entries.find(name) == _entries.end());
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::createProcDirectory(Process *process) {
	auto link = directMkdir(std::to_string(process->pid()));
	auto proc_dir = static_cast<DirectoryNode*>(link->getTarget().get());

	proc_dir->directMknode("exe", std::make_shared<ExeLink>(process));
	proc_dir->directMknode("root", std::make_shared<RootLink>(process));
	proc_dir->directMknode("cwd", std::make_shared<CwdLink>(process));
	proc_dir->directMknodeDir("fd", std::make_shared<FdDirectoryNode>(process));
	proc_dir->directMknodeDir("fdinfo", std::make_shared<FdInfoDirectoryNode>(process));
	proc_dir->directMkregular("maps", std::make_shared<MapNode>(process));
	proc_dir->directMkregular("comm", std::make_shared<CommNode>(process));
	proc_dir->directMkregular("stat", std::make_shared<StatNode>(process));
	proc_dir->directMkregular("statm", std::make_shared<StatmNode>(process));
	proc_dir->directMkregular("status", std::make_shared<ProcessStatusNode>(process->threadGroup()->weak_from_this()));
	proc_dir->directMkregular("cgroup", std::make_shared<CgroupNode>(process));
	proc_dir->directMkregular("mounts", std::make_shared<MountsNode>(process));
	proc_dir->directMkregular("mountinfo", std::make_shared<MountInfoNode>(process));

	proc_dir->directMkdir("task");

	return link;
}

std::shared_ptr<Link> DirectoryNode::createProcTaskDirectory(Process *process) {
	auto pidProcfsLink = process->threadGroup()->procfsLink();
	// Create /proc/[pid] on demand to minimize observable states of /proc/[pid] being visible
	// without any /proc/[pid]/task/[tid] attached.
	if (!pidProcfsLink) {
		pidProcfsLink = createProcDirectory(process);
		process->threadGroup()->setProcfsLink(pidProcfsLink);
	}

	auto proc_dir = static_cast<DirectoryNode *>(pidProcfsLink->getTarget().get());

	auto taskLinkIt = std::ranges::find_if(proc_dir->_entries, [](auto e) {
		return e->getName() == "task";
	});
	assert(taskLinkIt != proc_dir->_entries.end());
	auto task_dir = static_cast<DirectoryNode *>(((*taskLinkIt)->getTarget()).get());

	auto tid_link = task_dir->directMkdir(std::to_string(process->tid()));
	auto tid_dir = static_cast<DirectoryNode *>(tid_link->getTarget().get());

	tid_dir->directMkregular("comm", std::make_shared<CommNode>(process));
	tid_dir->directMkregular("status", std::make_shared<StatusNode>(process->weak_from_this()));

	return tid_link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::link(std::string,
		std::shared_ptr<FsNode>) {
	co_return Error::noSuchFile;
}

async::result<frg::expected<Error, FileStats>> DirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix procfs Directory::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

std::shared_ptr<FsLink> DirectoryNode::treeLink() {
	assert(_treeLink);
	auto s = _treeLink->shared_from_this();
	assert(s);
	return s;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
DirectoryNode::open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: procfs DirectoryNode open() received illegal arguments:"
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

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::getLink(std::string name) {
	auto it = _entries.find(name);
	if(it != _entries.end())
		co_return *it;
	co_return Error::noSuchFile;
}

async::result<frg::expected<Error>> DirectoryNode::unlink(std::string name) {
	auto it = _entries.find(name);
	if (it == _entries.end())
		co_return Error::noSuchFile;
	_entries.erase(it);
	co_return frg::expected<Error>{};
}

Error DirectoryNode::directUnlink(std::string name) {
	auto it = _entries.find(name);
	if (it == _entries.end())
		return Error::noSuchFile;
	_entries.erase(it);
	return Error::success;
}

LinkNode::LinkNode()
: FsNode{&procfsSuperblock} { }

async::result<frg::expected<Error, FileStats>> LinkNode::getStatsInternal(Process *proc) {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 4096; // Same as in Linux.
	stats.mode = 0666; // TODO: Some files can be written.
	stats.uid = proc->threadGroup()->uid();
	stats.gid = proc->threadGroup()->gid();
	stats.atimeSecs = now.tv_sec;
	stats.atimeNanos = now.tv_nsec;
	stats.mtimeSecs = now.tv_sec;
	stats.mtimeNanos = now.tv_nsec;
	stats.ctimeSecs = now.tv_sec;
	stats.ctimeNanos = now.tv_nsec;
	co_return stats;
}

async::result<std::expected<std::string, Error>> UptimeNode::show(Process *) {
	auto uptime = clk::getTimeSinceBoot();
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	// TODO: Add time spent in the idle thread here.
	std::stringstream stream;
	stream << uptime.tv_sec << "." << std::setw(2) << std::setfill('0') << (uptime.tv_nsec / 10000000) << std::setw(1) << "0.00" << "\n";
	co_return stream.str();
}

async::result<void> UptimeNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/uptime file" << std::endl;
	co_return;
}

async::result<std::expected<std::string, Error>> OstypeNode::show(Process *) {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "Managarm\n";
	co_return stream.str();
}

async::result<void> OstypeNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/sys/kernel/ostype file" << std::endl;
	co_return;
}

async::result<std::expected<std::string, Error>> OsreleaseNode::show(Process *) {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	// TODO: The version is a placeholder!
	std::stringstream stream;
	stream << "0.0.1\n";
	co_return stream.str();
}

async::result<void> OsreleaseNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/sys/kernel/osrelease file" << std::endl;
	co_return;
}

async::result<std::expected<std::string, Error>> ArchNode::show(Process *) {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
#if defined(__x86_64__)
	stream << "x86_64\n";
#elif defined(__aarch64__)
	stream << "AArch64\n";
#elif defined(__riscv) && __riscv_xlen == 64
	stream << "riscv64";
#else
	#error "Unknown architecture"
#endif
	co_return stream.str();
}

async::result<void> ArchNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/sys/kernel/arch file" << std::endl;
	co_return;
}

BootIdNode::BootIdNode() {
	uint8_t uuid[16];
	size_t n = 0;
	while(n < 16) {
		size_t chunk;
		HEL_CHECK(helGetRandomBytes(uuid + n, 16 - n, &chunk));
		n += chunk;
	}

	uint32_t a = 0;
	uint16_t b = 0;
	uint16_t c = 0;
	uint8_t d[8] = { 0 };
	memcpy(&a, &uuid[0], sizeof(a));
	memcpy(&b, &uuid[4], sizeof(b));
	memcpy(&c, &uuid[6], sizeof(c));
	memcpy(&d, &uuid[8], sizeof(d));

	bootId_ = std::format(
		"{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
		a, b, c, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

async::result<std::expected<std::string, Error>> BootIdNode::show(Process *) {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	co_return bootId_ + "\n";
}

async::result<void> BootIdNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/sys/kernel/random/boot_id file" << std::endl;
	co_return;
}

async::result<std::expected<std::string, Error>> HostnameNode::show(Process *) {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "managarm\n";
	co_return stream.str();
}

async::result<void> HostnameNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/sys/kernel/hostname file" << std::endl;
	co_return;
}

expected<std::string> SelfLink::readSymlink(FsLink *, Process *process) {
	co_return "/proc/" + std::to_string(process->pid());
}

async::result<frg::expected<Error, FileStats>> SelfLink::getStats() {
	FileStats stats = {};
	stats.numLinks = 1;
	stats.mode = 0777;
	co_return stats;
}

expected<std::string> SelfThreadLink::readSymlink(FsLink *, Process *process) {
	co_return "/proc/" + std::to_string(process->pid()) + "/task/" + std::to_string(process->tid());
}

async::result<frg::expected<Error, FileStats>> SelfThreadLink::getStats() {
	FileStats stats = {};
	stats.numLinks = 1;
	stats.mode = 0777;
	co_return stats;
}

ExeLink::ExeLink(Process *process)
	: _process(process->weak_from_this())
	{ }

expected<std::string> ExeLink::readSymlink(FsLink *, Process *) {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return p->path();
}

async::result<frg::expected<Error, FileStats>> ExeLink::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p.get());
}

MapNode::MapNode(Process* process)
	: _process(process->weak_from_this())
	{ }

async::result<std::expected<std::string, Error>> MapNode::show(Process *) {
	auto p = _process.lock();
	if (!p)
		co_return std::unexpected(Error::noSuchProcess);

	auto vmContext = p->vmContext();
	std::stringstream stream;
	for (auto area : *vmContext) {
		stream << std::hex << area.baseAddress();
		stream << "-";
		stream << std::hex << area.baseAddress() + area.size();
		stream << " ";
		stream << (area.isReadable() ? "r" : "-");
		stream << (area.isWritable() ? "w" : "-");
		stream << (area.isExecutable() ? "x" : "-");
		stream << (area.isPrivate() ? "p" : "-");
		stream << " ";
		auto backingFile = area.backingFile();
		if(backingFile && backingFile->associatedLink() && backingFile->associatedMount()) {
			stream << std::setfill('0') << std::setw(8) << area.backingFileOffset();
			stream << " ";
			auto fsNode = backingFile->associatedLink()->getTarget();
			ViewPath viewPath = {backingFile->associatedMount(), backingFile->associatedLink()};
			auto fileStats = co_await fsNode->getStats();
			DeviceId deviceId{};
			if (fsNode->getType() == VfsType::charDevice || fsNode->getType() == VfsType::blockDevice)
				deviceId = fsNode->readDevice();
			assert(fileStats);

			stream << std::dec << std::setfill('0') << std::setw(2) << deviceId.first << ":" << deviceId.second;
			stream << " ";
			stream << std::setw(0) << fileStats.value().inodeNumber;
			stream << "    ";
			stream << viewPath.getPath(p->fsContext()->getRoot());
		} else {
			// TODO: In the case of memfd files, show the name here.
			stream << "00000000 00:00 0";
		}
		stream << "\n";
	}
	co_return stream.str();
}

async::result<void> MapNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/maps file" << std::endl;
	co_return;
}

async::result<frg::expected<Error, FileStats>> MapNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

CommNode::CommNode(Process* process)
	: _process(process->weak_from_this())
	{ }

async::result<std::expected<std::string, Error>> CommNode::show(Process *) {
	auto p = _process.lock();
	if (!p)
		co_return std::unexpected(Error::noSuchProcess);

	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << p->name() << "\n";
	co_return stream.str();
}

async::result<void> CommNode::store(std::string name) {
	auto p = _process.lock();
	if (!p)
		co_return; // TODO: Proper error reporting

	// silently truncate to TASK_COMM_LEN (16), including the null terminator
	p->setName(name.substr(0, 15));
	co_return;
}

async::result<frg::expected<Error, FileStats>> CommNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

RootLink::RootLink(Process* process)
	: _process(process->weak_from_this())
	{ }

expected<std::string> RootLink::readSymlink(FsLink *, Process *) {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return p->fsContext()->getRoot().getPath(p->fsContext()->getRoot());
}

async::result<frg::expected<Error, FileStats>> RootLink::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p.get());
}

StatNode::StatNode(Process *process) : _process(process->weak_from_this()) {}

async::result<std::expected<std::string, Error>> StatNode::show(Process *) {
	auto p = _process.lock();
	if (!p)
		co_return std::unexpected(Error::noSuchProcess);

	// Everything that has a value of 0 is likely not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << p->pid(); // Pid
	stream << " (" << p->name() << ") "; // Name
	stream << "R "; // State
	// This avoids a crash when asking for the parent of init.
	if(p->getParent()) {
		stream << p->getParent()->pid() << " ";
	} else {
		stream << "0 ";
	}
	stream << p->pgPointer()->getHull()->getPid() << " "; // Pgrp
	stream << p->pgPointer()->getSession()->getSessionId() << " "; // SID
	stream << "0 "; // tty_nr
	stream << "0 "; // tpgid
	stream << "0 "; // flags
	stream << "0 "; // minflt
	stream << "0 "; // cminflt
	stream << "0 "; // majflt
	stream << "0 "; // cmajflt
	stream << p->threadGroup()->accumulatedUsage().userTime << " "; // utime
	stream << "0 "; // stime
	stream << "0 "; // cutime
	stream << "0 "; // cstime
	stream << "0 "; // priority
	stream << "0 "; // nice
	stream << "1 "; // num_threads
	stream << "0 "; // itrealvalue
	stream << "0 "; // starttime
	stream << "0 "; // vsize
	stream << "0 "; // rss
	stream << "0 "; // rsslim
	stream << "0 "; // startcode
	stream << "0 "; // endcode
	stream << "0 "; // startstack
	stream << "0 "; // kstkesp
	stream << "0 "; // kstkeip
	stream << "0 "; // signal
	stream << "0 "; // blocked
	stream << "0 "; // sigignore
	stream << "0 "; // sigcatch
	stream << "0 "; // wchan
	stream << "0 "; // nswap
	stream << "0 "; // cnswap
	stream << "0 "; // exit_signal
	stream << "0 "; // processor
	stream << "0 "; // rt_priority
	stream << "0 "; // policy
	stream << "0 "; // delayacct_blkio_ticks
	stream << "0 "; // guest_time
	stream << "0 "; // cguest_time
	stream << "0 "; // start_data
	stream << "0 "; // end_data
	stream << "0 "; // start_brk
	stream << "0 "; // arg_start
	stream << "0 "; // arg_end
	stream << "0 "; // env_start
	stream << "0 "; // env_end
	stream << "0\n"; // exitcode
	co_return stream.str();
}

async::result<void> StatNode::store(std::string) {
	// TODO: proper error reporting.
	std::println("Can't store to a /proc/stat file!");
	co_return;
}

async::result<frg::expected<Error, FileStats>> StatNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

StatmNode::StatmNode(Process *process) : _process(process->weak_from_this()) {}

async::result<std::expected<std::string, Error>> StatmNode::show(Process *) {
	(void)_process;
	// All hardcoded to 0.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "0 "; // size
	stream << "0 "; // resident
	stream << "0 "; // shared
	stream << "0 "; // text
	stream << "0 "; // lib
	stream << "0 "; // data
	stream << "0\n"; // dt
	co_return stream.str();
}

async::result<void> StatmNode::store(std::string) {
	// TODO: proper error reporting.
	std::println("Can't store to a /proc/statm file!");
	co_return;
}

async::result<frg::expected<Error, FileStats>> StatmNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

async::result<std::expected<std::string, Error>> ProcessStatusNode::show(Process *) {
	auto tg = _tg.lock();
	if (!tg)
		co_return std::unexpected{Error::noSuchProcess};

	std::string state = "R (running)";
	if(tg->notifyType() == NotifyType::terminated)
		state = "Z (zombie)";

	// Everything that has a value of N/A is not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << std::format("State:\t{}\n", state);
	stream << "Tgid: " << tg->pid() << "\n"; // Thread group id, same as gid for now
	stream << "NGid: 0\n"; // NUMA Group ID, 0 if none.
	stream << "Pid: " << tg->pid() << "\n";
	// This avoids a crash when asking for the parent of init.
	if(tg->getParent()) {
		stream << "PPid: " << tg->getParent()->pid() << "\n";
	} else {
		stream << "PPid: 0\n";
	}
	stream << "TracerPid: 0\n"; // We're not being traced, so 0 is fine.
	stream << "Uid: " << tg->uid() << "\n";
	stream << "Gid: " << tg->gid() << "\n";
	stream << "FDSize: " << fileTableSize << "\n";
	stream << "Groups: 0\n"; // We don't implement groups yet, so 0 is fine.
	// Namespace information, unimplemented.
	stream << "NStgid: N/A\n";
	stream << "NSpid: N/A\n";
	stream << "NSpgid: N/A\n";
	stream << "NSsid: N/A\n";
	// End namespace information.
	// VM information, not exposed yet.
	stream << "VmPeak: N/A kB\n";
	stream << "VmSize: N/A kB\n";
	stream << "VmLck: 0 kB\n"; // We don't lock memory.
	stream << "VmPin: 0 kB\n"; // We don't pin memory.
	stream << "VmHWM: N/A kB\n";
	stream << "VmRSS: N/A kB\n";
	stream << "RssAnon: N/A kB\n";
	stream << "RssFile: N/A kB\n";
	stream << "RssShmem: N/A kB\n";
	stream << "VmData: N/A kB\n";
	stream << "VmStk: N/A kB\n";
	stream << "VmExe: N/A kB\n";
	stream << "VmLib: N/A kB\n";
	stream << "VmPTE: N/A kB\n";
	stream << "VmSwap: 0 kB\n"; // We don't have swap yet.
	stream << "HugetlbPages: N/A kB\n";
	// End of VM information.
	stream << "CoreDumping: 0\n"; // We don't implement coredumps, so 0 is correct here.
	// Documentation doesn't mention THP_enabled.
	stream << "THP_enabled: N/A\n";
	stream << "Threads: 1\n"; // Number of threads in this process, hardcode to 1 for now.
	// Signal related information, we should fill this out properly eventually.
	stream << "SigQ: N/A\n";
	// Masks of pending, blocked, ignored and caught signals, zero them all.
	stream << "SigPnd: 0000000000000000\n";
	stream << "ShdPnd: 0000000000000000\n";
	stream << "SigBlk: 0000000000000000\n";
	stream << "SigIgn: 0000000000000000\n";
	stream << "SigCgt: 0000000000000000\n";
	// End of signal related information.
	// We don't implement capabilities, so 0 is good for all of them.
	stream << "CapInh: 0000000000000000\n";
	stream << "CapPrm: 0000000000000000\n";
	stream << "CapEff: 0000000000000000\n";
	stream << "CapBnd: 0000000000000000\n";
	stream << "CapAmb: 0000000000000000\n";
	// We don't implement this bit, nor seccomp, nor spectre/meltdown mitigations.
	stream << "NoNewPrivs: 0\n";
	stream << "Seccomp: 0\n";
	stream << "Seccomp_filters: 0\n";
	stream << "Speculation_Store_Bypass: thread vulnerable\n";
	stream << "SpeculationIndirectBranch: thread vulnerable\n";
	// Other stuff we don't implement yet.
	stream << "Cpus_allowed: N/A\n";
	stream << "Cpus_allowed_list: N/A\n";
	stream << "Mems_allowed: N/A\n";
	stream << "Mems_allowed_list: N/A\n";
	stream << "voluntary_ctxt_switches: N/A\n";
	stream << "nonvoluntary_ctxt_switches: N/A\n";
	co_return stream.str();
}

async::result<void> ProcessStatusNode::store(std::string) {
	// TODO: proper error reporting.
	std::println("Can't store to a /proc/status file!");
	co_return;
}

async::result<frg::expected<Error, FileStats>> ProcessStatusNode::getStats() {
	auto tg = _tg.lock();
	if (!tg)
		co_return Error::noSuchFile;

	co_return co_await getStatsInternal(tg.get());
}

async::result<std::expected<std::string, Error>> StatusNode::show(Process *) {
	auto p = _process.lock();
	if (!p)
		co_return std::unexpected{Error::noSuchProcess};

	std::string state = "R (running)";
	if(p->threadGroup()->notifyType() == NotifyType::terminated)
		state = "Z (zombie)";

	// Everything that has a value of N/A is not implemented yet.
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "Name: " << p->name() << "\n"; // Name is hardcoded to be the last part of the path
	if (p->fsContext())
		stream << std::format("Umask: 0{:03o}\n", p->fsContext()->getUmask());
	stream << std::format("State:\t{}\n", state);
	stream << "Tgid: " << p->pid() << "\n"; // Thread group id, same as gid for now
	stream << "NGid: 0\n"; // NUMA Group ID, 0 if none.
	stream << "Pid: " << p->tid() << "\n";
	// This avoids a crash when asking for the parent of init.
	if(p->getParent()) {
		stream << "PPid: " << p->getParent()->pid() << "\n";
	} else {
		stream << "PPid: 0\n";
	}
	stream << "TracerPid: 0\n"; // We're not being traced, so 0 is fine.
	stream << "Uid: " << p->threadGroup()->uid() << "\n";
	stream << "Gid: " << p->threadGroup()->gid() << "\n";
	stream << "FDSize: " << fileTableSize << "\n";
	stream << "Groups: 0\n"; // We don't implement groups yet, so 0 is fine.
	// Namespace information, unimplemented.
	stream << "NStgid: N/A\n";
	stream << "NSpid: N/A\n";
	stream << "NSpgid: N/A\n";
	stream << "NSsid: N/A\n";
	// End namespace information.
	// VM information, not exposed yet.
	stream << "VmPeak: N/A kB\n";
	stream << "VmSize: N/A kB\n";
	stream << "VmLck: 0 kB\n"; // We don't lock memory.
	stream << "VmPin: 0 kB\n"; // We don't pin memory.
	stream << "VmHWM: N/A kB\n";
	stream << "VmRSS: N/A kB\n";
	stream << "RssAnon: N/A kB\n";
	stream << "RssFile: N/A kB\n";
	stream << "RssShmem: N/A kB\n";
	stream << "VmData: N/A kB\n";
	stream << "VmStk: N/A kB\n";
	stream << "VmExe: N/A kB\n";
	stream << "VmLib: N/A kB\n";
	stream << "VmPTE: N/A kB\n";
	stream << "VmSwap: 0 kB\n"; // We don't have swap yet.
	stream << "HugetlbPages: N/A kB\n";
	// End of VM information.
	stream << "CoreDumping: 0\n"; // We don't implement coredumps, so 0 is correct here.
	// Documentation doesn't mention THP_enabled.
	stream << "THP_enabled: N/A\n";
	stream << "Threads: 1\n"; // Number of threads in this process, hardcode to 1 for now.
	// Signal related information, we should fill this out properly eventually.
	stream << "SigQ: N/A\n";
	// Masks of pending, blocked, ignored and caught signals, zero them all.
	stream << "SigPnd: 0000000000000000\n";
	stream << "ShdPnd: 0000000000000000\n";
	stream << "SigBlk: 0000000000000000\n";
	stream << "SigIgn: 0000000000000000\n";
	stream << "SigCgt: 0000000000000000\n";
	// End of signal related information.
	// We don't implement capabilities, so 0 is good for all of them.
	stream << "CapInh: 0000000000000000\n";
	stream << "CapPrm: 0000000000000000\n";
	stream << "CapEff: 0000000000000000\n";
	stream << "CapBnd: 0000000000000000\n";
	stream << "CapAmb: 0000000000000000\n";
	// We don't implement this bit, nor seccomp, nor spectre/meltdown mitigations.
	stream << "NoNewPrivs: 0\n";
	stream << "Seccomp: 0\n";
	stream << "Seccomp_filters: 0\n";
	stream << "Speculation_Store_Bypass: thread vulnerable\n";
	stream << "SpeculationIndirectBranch: thread vulnerable\n";
	// Other stuff we don't implement yet.
	stream << "Cpus_allowed: N/A\n";
	stream << "Cpus_allowed_list: N/A\n";
	stream << "Mems_allowed: N/A\n";
	stream << "Mems_allowed_list: N/A\n";
	stream << "voluntary_ctxt_switches: N/A\n";
	stream << "nonvoluntary_ctxt_switches: N/A\n";
	co_return stream.str();
}

async::result<void> StatusNode::store(std::string) {
	// TODO: proper error reporting.
	std::println("Can't store to a /proc/status file!");
	co_return;
}

async::result<frg::expected<Error, FileStats>> StatusNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchFile;

	co_return co_await getStatsInternal(p->threadGroup());
}

CwdLink::CwdLink(Process *process)
	: _process(process->weak_from_this())
	{ }

expected<std::string> CwdLink::readSymlink(FsLink *, Process *) {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return p->fsContext()->getWorkingDirectory().getPath(p->fsContext()->getWorkingDirectory());
}

async::result<frg::expected<Error, FileStats>> CwdLink::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p.get());
}

expected<std::string> MountsLink::readSymlink(FsLink *, Process *) {
	co_return "self/mounts";
}

async::result<frg::expected<Error, FileStats>> MountsLink::getStats() {
	FileStats stats = {};

	stats.fileSize = sizeof("self/mounts") - 1;
	stats.mode = 0777;

	co_return stats;
}

CgroupNode::CgroupNode(Process *process)
	: _process(process->weak_from_this())
	{ }

// MASSIVE STUBS
async::result<std::expected<std::string, Error>> CgroupNode::show(Process *) {
	// See man 7 cgroups for more details, I'm emulating cgroups2 here.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << "0::/init.scope\n";
	co_return stream.str();
}

async::result<void> CgroupNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/cgroup file" << std::endl;
	co_return;
}

async::result<frg::expected<Error, FileStats>> CgroupNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

void FdDirectoryFile::serve(smarter::shared_ptr<FdDirectoryFile> file) {
	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

FdDirectoryFile::FdDirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, Process *process)
: FileWithDefaults{FileKind::unknown,  StructName::get("procfs.fddir"), std::move(mount), std::move(link)},
		_process{process->weak_from_this()}, _fileTable{process->fileContext()->fileTable()}, _iter{_fileTable.begin()} {}

void FdDirectoryFile::handleClose() {
	_cancelServe.cancel();
}

FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> FdDirectoryFile::readEntries() {
	if(_iter != _fileTable.end()) {
		auto name = std::to_string((_iter++)->first);

		co_return protocols::fs::ReadEntriesResult{
			.name = name,
			.inode = 0,
			.offset = std::distance(_fileTable.cbegin(), _iter)
		};
	}else{
		co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);
	}
}

helix::BorrowedDescriptor FdDirectoryFile::getPassthroughLane() {
	return _passthrough;
}

FdDirectoryNode::FdDirectoryNode(Process *process)
: FsNode(&procfsSuperblock), _process{process->weak_from_this()} {}

VfsType FdDirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, FileStats>> FdDirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix procfs FdDirectoryNode::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

std::shared_ptr<FsLink> FdDirectoryNode::treeLink() {
	assert(_treeLink);
	auto s = _treeLink->shared_from_this();
	assert(s);
	return s;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
FdDirectoryNode::open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: procfs FdDirectoryNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	auto file = smarter::make_shared<FdDirectoryFile>(std::move(mount), std::move(link), p.get());
	file->setupWeakFile(file);
	FdDirectoryFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> FdDirectoryNode::getLink(std::string name) {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	for(const auto &[fdnum, fd] : p->fileContext()->fileTable()) {
		if(name != std::to_string(fdnum))
			continue;
		auto pointee = std::make_shared<SymlinkNode>(p.get(), fd.file->associatedMount(), fd.file->associatedLink());
		co_return std::make_shared<Link>(shared_from_this(), name, pointee);
	}
	co_return Error::noSuchFile;
}

SymlinkNode::SymlinkNode(Process* proc, std::shared_ptr<MountView> mount, std::weak_ptr<FsLink> link)
: _process{proc->weak_from_this()}, _mount{std::move(mount)}, _link{std::move(link)} { }

expected<std::string> SymlinkNode::readSymlink(FsLink *, Process *process) {
	auto link = _link.lock();
	if(!link)
		co_return Error::ioError;

	auto desc = link->getProcFsDescription();
	if(desc)
		co_return desc.value();

	ViewPath viewPath = {_mount, link};
	auto path = viewPath.getPath(process->fsContext()->getRoot());

	co_return path;
}

async::result<frg::expected<Error, FileStats>> SymlinkNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	FileStats stats = {};

	stats.fileSize = 64; // Same as Linux.
	stats.mode = 0777;
	stats.uid = p->threadGroup()->uid();
	stats.gid = p->threadGroup()->gid();

	co_return stats;
}

MountsNode::MountsNode(Process* process)
	: _process(process->weak_from_this())
	{ }

async::result<std::expected<std::string, Error>> MountsNode::show(Process *proc) {
	auto root = proc->fsContext()->getRoot();

	auto processMount = [&proc](std::shared_ptr<MountView> mount, bool root = false) {
		auto dev = mount->getDevice();
		auto fsType = mount->getOrigin()->getTarget()->superblock()->getFsType();

		std::string devName = [&]() {
			if(dev.second) {
				return dev.getPath(proc->fsContext()->getRoot());
			} else {
				return fsType;
			}
		}();

		auto mountPath = root ? "/" :
			ViewPath{mount->getParent(), mount->getAnchor()}.getPath(proc->fsContext()->getRoot());

		return std::format("{} {} {} rw 0 0\n",
			devName, mountPath, fsType);
	};

	std::function<std::string(const std::set<std::shared_ptr<MountView>, MountView::Compare>&)> processChildren =
		[&](auto &mounts) -> std::string {
		std::string ret;

		for(auto mount : mounts) {
			ret.append(processMount(mount));
			ret.append(processChildren(mount->mounts()));
		}

		return ret;
	};

	std::string ret = processMount(root.first, true);
	ret.append(processChildren(root.first->mounts()));

	co_return ret;
}

async::result<void> MountsNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/mounts file" << std::endl;
	co_return;
}

async::result<frg::expected<Error, FileStats>> MountsNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

MountInfoNode::MountInfoNode(Process *process)
	: _process(process->shared_from_this())
	{ }

async::result<std::expected<std::string, Error>> MountInfoNode::show(Process *proc) {
	auto root = proc->fsContext()->getRoot();

	auto processMount = [&proc](std::shared_ptr<MountView> mount, bool root = false) {
		auto mountId = mount->mountId();
		auto devno = mount->getOrigin()->getTarget()->superblock()->deviceNumber();
		auto parentId = mount->getParent() ? mount->getParent()->mountId() : mountId;
		auto dev = mount->getDevice();
		auto fsType = mount->getOrigin()->getTarget()->superblock()->getFsType();

		auto devName = [&]() -> std::string {
			if(dev.second) {
				return dev.getPath(proc->fsContext()->getRoot());
			} else {
				return "none";
			}
		}();

		auto mountPath = root ? "/" :
			ViewPath{mount->getParent(), mount->getAnchor()}.getPath(proc->fsContext()->getRoot());

		return std::format("{} {} {}:{} {} {} rw - {} {} rw\n",
			mountId, parentId, major(devno), minor(devno), "/", mountPath, fsType, devName);
	};

	std::function<std::string(const std::set<std::shared_ptr<MountView>, MountView::Compare> &)> processChildren =
		[&](auto &mounts) -> std::string {
		std::string ret;

		for(auto mount : mounts) {
			ret.append(processMount(mount));
			ret.append(processChildren(mount->mounts()));
		}

		return ret;
	};

	std::string ret = processMount(root.first, true);
	ret.append(processChildren(root.first->mounts()));

	co_return ret;
}

async::result<void> MountInfoNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/mountinfo file" << std::endl;
	co_return;
}

async::result<frg::expected<Error, FileStats>> MountInfoNode::getStats() {
	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	co_return co_await getStatsInternal(p->threadGroup());
}

FdInfoDirectoryNode::FdInfoDirectoryNode(Process* process)
: FsNode(&procfsSuperblock), _process{process->weak_from_this()} {}

VfsType FdInfoDirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, FileStats>> FdInfoDirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix procfs FdInfoDirectoryNode::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

std::shared_ptr<FsLink> FdInfoDirectoryNode::treeLink() {
	assert(_treeLink);
	auto s = _treeLink->shared_from_this();
	assert(s);
	return s;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
FdInfoDirectoryNode::open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: procfs FdInfoDirectoryNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	auto file = smarter::make_shared<FdInfoDirectoryFile>(std::move(mount), std::move(link), p.get());
	file->setupWeakFile(file);
	FdInfoDirectoryFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> FdInfoDirectoryNode::getLink(std::string name) {
	if(!std::all_of(name.begin(), name.end(), isdigit))
		co_return Error::noSuchFile;

	auto p = _process.lock();
	if (!p)
		co_return Error::noSuchProcess;

	auto nameNum = std::stoi(name);
	if(p->fileContext()->fileTable().contains(nameNum)) {
		auto file = p->fileContext()->fileTable().at(nameNum).file;
		auto pointee = std::make_shared<FdInfoNode>(file->associatedMount(), file);
		co_return std::make_shared<Link>(shared_from_this(), name, pointee);
	}

	co_return Error::noSuchFile;
}

void FdInfoDirectoryFile::serve(smarter::shared_ptr<FdInfoDirectoryFile> file) {
	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

FdInfoDirectoryFile::FdInfoDirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, Process* process)
: FileWithDefaults{FileKind::unknown,  StructName::get("procfs.fdinfodir"), std::move(mount), std::move(link)},
		_process{process->weak_from_this()}, _fileTable{process->fileContext()->fileTable()}, _iter{_fileTable.begin()} {}

void FdInfoDirectoryFile::handleClose() {
	_cancelServe.cancel();
}

FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> FdInfoDirectoryFile::readEntries() {
	if(_iter != _fileTable.end()) {
		auto name = std::to_string((_iter++)->first);

		co_return protocols::fs::ReadEntriesResult{
			.name = name,
			.inode = 0,
			.offset = std::distance(_fileTable.cbegin(), _iter)
		};
	}else{
		co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);
	}
}

helix::BorrowedDescriptor FdInfoDirectoryFile::getPassthroughLane() {
	return _passthrough;
}

async::result<std::expected<std::string, Error>> FdInfoNode::show(Process *) {
	auto seekResult = co_await file_->seek(0, VfsSeek::relative);
	auto pos = seekResult ? seekResult.value() : 0;
	auto mountId = mountView_ ? mountView_->mountId() : 0;
	auto extraInfo = co_await file_->getFdInfo();

	co_return std::format("pos:\t{}\nmnt_id:\t{}\n{}", pos, mountId, extraInfo);
}

async::result<void> FdInfoNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/[pid]/fdinfo/N file" << std::endl;
	co_return;
}

} // namespace procfs

std::shared_ptr<FsLink> getProcfs() {
	static std::shared_ptr<FsLink> procfs = procfs::DirectoryNode::createRootDirectory();
	return procfs;
}
