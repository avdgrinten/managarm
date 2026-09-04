#include <core/clock.hpp>
#include <async/result.hpp>
#include <fcntl.h>
#include <protocols/fs/server.hpp>
#include <frg/scope_exit.hpp>

#include "../trace.hpp"
#include "../common-ops.hpp"

#include "ext2fs.hpp"

namespace blockfs::ext2fs {

namespace {

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
readEntries(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);

	co_await self->mutex.async_lock();
	frg::unique_lock fileLock{frg::adopt_lock, self->mutex};

	co_await self->inode->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inode->inodeMutex};

	co_return co_await self->readEntries();
}

async::result<int> getFileFlags(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	int flags = 0;

	if(self->read && self->write)
		flags |= O_RDWR;
	else if(self->read)
		flags |= O_RDONLY;
	else if(self->write)
		flags |= O_WRONLY;

	co_return flags;
}

async::result<void> setFileFlags(void *, int) {
	std::cout << "libblockfs: setFileFlags is stubbed" << std::endl;
	co_return;
}


async::result<frg::expected<protocols::fs::Error, protocols::fs::GetLinkResult>>
getLink(std::shared_ptr<void> object,
		std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t timeLock = 0;
	uint64_t timeFind = 0;
	bool found = false;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtGetLink,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeLock(timeLock),
			ostAttrTimeFind(timeFind),
			ostAttrFound(found)
		);
	}};


	assert(!name.empty() && name != "." && name != "..");

	co_await self->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	timeLock = timer.split();

	auto entry = FRG_CO_TRY(co_await self->findEntry(name));
	timeFind = timer.split();
	found = entry.has_value();
	if(!entry)
		co_return protocols::fs::GetLinkResult{nullptr, -1,
				protocols::fs::FileType::unknown, self->dirSerial};

	protocols::fs::FileType type;
	switch(entry->fileType) {
	case kTypeDirectory:
		type = protocols::fs::FileType::directory;
		break;
	case kTypeRegular:
		type = protocols::fs::FileType::regular;
		break;
	case kTypeSymlink:
		type = protocols::fs::FileType::symlink;
		break;
	default:
		throw std::runtime_error("Unexpected file type");
	}

	assert(entry->inode);
	co_return protocols::fs::GetLinkResult{self->fs.accessInode(entry->inode), entry->inode, type,
			self->dirSerial};
}

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>> link(std::shared_ptr<void> object,
		std::string name, int64_t ino) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	// Reject hard links to directories to guarantee an acyclic directory tree.
	auto target = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(ino));
	co_await target->readyEvent.wait();
	if(target->fileType == kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::insufficientPermissions};

	// link() requires the target to be locked.
	co_await target->inodeMutex.async_lock();
	frg::unique_lock targetLock{frg::adopt_lock, target->inodeMutex};

	auto entry = co_await self->link(std::move(name), ino, kTypeRegular);
	if(!entry)
		co_return std::unexpected{entry.error()};

	protocols::fs::FileType type;
	switch(entry->fileType) {
	case kTypeDirectory:
		type = protocols::fs::FileType::directory;
		break;
	case kTypeRegular:
		type = protocols::fs::FileType::regular;
		break;
	case kTypeSymlink:
		type = protocols::fs::FileType::symlink;
		break;
	default:
		throw std::runtime_error("Unexpected file type");
	}

	assert(entry->inode);
	co_return protocols::fs::GetLinkResult{self->fs.accessInode(entry->inode), entry->inode, type,
			self->dirSerial};
}

async::result<std::expected<uint64_t, protocols::fs::Error>> unlink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t lockDone = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2Unlink,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeLock(lockDone)
		);
	}};

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	lockDone = timer.elapsed();

	auto entry = co_await self->findEntry(name);
	if(!entry)
		co_return std::unexpected{entry.error()};
	if(!entry.value())
		co_return std::unexpected{protocols::fs::Error::fileNotFound};
	if(entry.value()->fileType == kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::isDirectory};

	// removeEntry() requires the target to be locked.
	auto target = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(entry.value()->inode));
	co_await target->inodeMutex.async_lock();
	frg::unique_lock targetLock{frg::adopt_lock, target->inodeMutex};

	auto result = co_await self->removeEntry(std::move(name));
	if (!result)
		co_return std::unexpected{result.error()};
	co_return self->dirSerial;
}

async::result<std::expected<uint64_t, protocols::fs::Error>> rmdir(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t lockDone = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2Rmdir,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeLock(lockDone)
		);
	}};

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	lockDone = timer.elapsed();

	auto entry = co_await self->findEntry(name);
	if(!entry)
		co_return std::unexpected{entry.error()};
	if(!entry.value())
		co_return std::unexpected{protocols::fs::Error::fileNotFound};
	if(entry.value()->fileType != kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::notDirectory};

	// isDirectoryEmpty() and removeEntry() require the target to be locked.
	auto target = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(entry.value()->inode));
	co_await target->inodeMutex.async_lock();
	frg::unique_lock targetLock{frg::adopt_lock, target->inodeMutex};

	auto isEmpty = FRG_CO_TRY(co_await target->isDirectoryEmpty());
	if(!isEmpty)
		co_return std::unexpected{protocols::fs::Error::directoryNotEmpty};

	auto result = co_await self->removeEntry(std::move(name));
	if (!result)
		co_return std::unexpected{result.error()};
	co_return self->dirSerial;
}

async::result<protocols::fs::FileStats>
getStats(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timeLock = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2GetStats,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeReady(timeReady),
			ostAttrTimeLock(timeLock)
		);
	}};

	co_await self->readyEvent.wait();
	timeReady = timer.split();

	co_await self->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	timeLock = timer.split();

	protocols::fs::FileStats stats;
	stats.linkCount = self->diskInode()->linksCount;
	stats.fileSize = self->fileSize();
	stats.mode = self->diskInode()->mode & 0xFFF;
	stats.uid = self->diskInode()->uid;
	stats.gid = self->diskInode()->gid;
	stats.accessTime.tv_sec = self->diskInode()->atime;
	stats.dataModifyTime.tv_sec = self->diskInode()->mtime;;
	stats.anyChangeTime.tv_sec = self->diskInode()->ctime;

	co_return stats;
}

async::result<std::expected<std::string, protocols::fs::Error>> readSymlink(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyEvent.wait();

	// readlink() is only valid on symbolic links.
	if(self->fileType != kTypeSymlink)
		co_return std::unexpected{protocols::fs::Error::illegalArguments};

	co_await self->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	if(self->fileSize() <= 60) {
		co_return std::string{self->diskInode()->data.embedded,
			self->diskInode()->data.embedded + self->fileSize()};
	} else {
		std::string result;
		result.resize(self->fileSize());
		co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(self->frontalMemory),
			0, self->fileSize(), result.data());
		co_return result;
	}
}

async::result<std::expected<protocols::fs::MkdirResult, protocols::fs::Error>>
mkdir(std::shared_ptr<void> object, std::string name, uid_t uid, gid_t gid, mode_t mode) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t lockDone = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2Mkdir,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeLock(lockDone)
		);
	}};

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	lockDone = timer.elapsed();

	auto entry = co_await self->mkdir(std::move(name), uid, gid, mode);

	if(!entry)
		co_return std::unexpected{entry.error()};

	assert(entry->inode);
	co_return protocols::fs::MkdirResult{self->fs.accessInode(entry->inode), entry->inode,
			self->dirSerial};
}

async::result<std::expected<protocols::fs::SymlinkResult, protocols::fs::Error>>
symlink(std::shared_ptr<void> object, std::string name, std::string target) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t lockDone = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2Symlink,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeLock(lockDone)
		);
	}};

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	lockDone = timer.elapsed();

	auto entry = co_await self->symlink(std::move(name), std::move(target));

	if(!entry)
		co_return std::unexpected{entry.error()};

	assert(entry->inode);
	co_return protocols::fs::SymlinkResult{self->fs.accessInode(entry->inode), entry->inode,
			self->dirSerial};
}

async::result<protocols::fs::Error> chmod(std::shared_ptr<void> object, int mode) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto result = co_await self->chmod(mode);

	co_return result;
}

async::result<protocols::fs::Error> chown(std::shared_ptr<void> object, std::optional<uid_t> uid, std::optional<gid_t> gid) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto result = co_await self->chown(uid, gid);

	co_return result;
}

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>>
getLinkOrCreate(std::shared_ptr<void> object, std::string name, mode_t mode, bool exclusive,
		uid_t uid, gid_t gid) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t timeLock = 0;
	uint64_t timeFind = 0;
	uint64_t timeCreate = 0;
	uint64_t timeLink = 0;
	bool existed = false;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2GetLinkOrCreate,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeLock(timeLock),
			ostAttrTimeFind(timeFind),
			ostAttrTimeCreate(timeCreate),
			ostAttrTimeLink(timeLink),
			ostAttrFound(existed)
		);
	}};

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};
	timeLock = timer.split();

	auto findResult = co_await self->findEntry(name);
	timeFind = timer.split();

	if (!findResult)
		co_return std::unexpected{findResult.error()};

	auto result = findResult.value();

	if (result) {
		existed = true;
		if (exclusive)
			co_return std::unexpected{protocols::fs::Error::alreadyExists};

		auto e = *result;
		protocols::fs::FileType type;
		switch(e.fileType) {
		case kTypeDirectory:
			type = protocols::fs::FileType::directory;
			break;
		case kTypeRegular:
			type = protocols::fs::FileType::regular;
			break;
		case kTypeSymlink:
			type = protocols::fs::FileType::symlink;
			break;
		default:
			throw std::runtime_error("Unexpected file type");
		}
		co_return protocols::fs::GetLinkResult{self->fs.accessInode(e.inode), e.inode, type,
				self->dirSerial};
	}

	auto baseInode = co_await self->fs.createRegular(uid, gid, self->number);
	auto inode = std::static_pointer_cast<ext2fs::Inode>(baseInode);

	// Lock the new inode immediately as it is published by link() below.
	co_await inode->inodeMutex.async_lock();
	frg::unique_lock newInodeLock{frg::adopt_lock, inode->inodeMutex};

	auto chmodResult = co_await inode->chmod(mode);
	if (chmodResult != protocols::fs::Error::none)
		co_return std::unexpected{chmodResult};
	timeCreate = timer.split();

	auto linkResult = co_await self->link(name, inode->number, FileType::kTypeRegular);
	timeLink = timer.split();
	if (!linkResult)
		co_return std::unexpected{protocols::fs::Error::internalError};

	co_return protocols::fs::GetLinkResult{inode, inode->number, protocols::fs::FileType::regular,
			self->dirSerial};
}

} // namespace anonymous

constinit protocols::fs::FileOperations fileOperations {
	.seekAbs      = &doSeekAbs<FileSystem>,
	.seekRel      = &doSeekRel<FileSystem>,
	.seekEof      = &doSeekEof<FileSystem>,
	.read         = &doRead<FileSystem>,
	.pread        = &doPread<FileSystem>,
	.write        = &doWrite<FileSystem>,
	.pwrite       = &doPwrite<FileSystem>,
	.readEntries  = &readEntries,
	.accessMemory = &doAccessMemory<FileSystem>,
	.truncate     = &doTruncate<FileSystem>,
	.flock        = &doFlock<FileSystem>,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

constinit protocols::fs::NodeOperations nodeOperations{
	.getStats = &getStats,
	.getLink = &getLink,
	.link = &link,
	.unlink = &unlink,
	.rmdir = &rmdir,
	.open = &doOpen<FileSystem>,
	.readSymlink = &readSymlink,
	.mkdir = &mkdir,
	.symlink = &symlink,
	.chmod = &chmod,
	.chown = &chown,
	.utimensat = &doUtimensat<FileSystem>,
	.obstructLink = &doObstructLink<FileSystem>,
	.traverseLinks = &doTraverseLinks<FileSystem>,
	.getLinkOrCreate = &getLinkOrCreate
};

} // namespace blockfs::ext2fs
