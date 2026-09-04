#include <assert.h>
#include <async/result.hpp>
#include <core/clock.hpp>
#include <frg/scope_exit.hpp>
#include <print>
#include <protocols/fs/server.hpp>

#include "../common-ops.hpp"

#include "btrfs.hpp"
#include "pretty-print.hpp"
#include "spec.hpp"

namespace blockfs::btrfs {

namespace {

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
readEntries(void *object) {
	auto self = static_cast<btrfs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await inode->readyEvent.wait();

	if (inode->fileType != kTypeDirectory) {
		std::println(
		    "\e[33mlibblockfs: readEntries called on something that's not a directory\e[39m"
		);
		co_return std::unexpected(managarm::fs::Errors::NOT_DIRECTORY);
	}

	auto fs = static_cast<btrfs::FileSystem *>(&inode->fs);
	Key searchKey{inode->number, ItemType::DIR_INDEX, self->offset};
	BtreePtr ptr{};
	auto val = co_await fs->upperBound(fs->fsTreeRoot_, searchKey, ptr);
	if (!val || ptr.back().key.noOffset() != searchKey.noOffset())
		co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);

	auto item = reinterpret_cast<const struct DirItem *>(val->data());
	auto name_span = val->subspan(
		sizeof(struct DirItem),
		std::min(val->size() - sizeof(struct DirItem), size_t(item->name_len))
	);
	auto name = std::string{reinterpret_cast<const char *>(name_span.data()), name_span.size()};

	self->offset = ptr.back().key.offset;
	assert(self->offset <= LONG_MAX);

	co_return protocols::fs::ReadEntriesResult{
	    std::move(name), item->location.objectid, static_cast<long>(self->offset)
	};
}

async::result<int> getFileFlags(void *) {
	std::println("libblockfs: getFileFlags is stubbed");
	co_return 0;
}

async::result<void> setFileFlags(void *, int) {
	std::println("libblockfs: setFileFlags is stubbed");
	co_return;
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::GetLinkResult>>
getLink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<btrfs::Inode>(object);

	assert(!name.empty() && name != "." && name != "..");
	auto entry = FRG_CO_TRY(co_await self->findEntry(name));
	if (!entry)
		co_return protocols::fs::GetLinkResult{nullptr, -1, protocols::fs::FileType::unknown,
				self->dirSerial};

	protocols::fs::FileType type;
	switch (entry->fileType) {
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

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>>
link(std::shared_ptr<void> object, std::string name, int64_t ino) {
	(void)object;
	(void)name;
	(void)ino;
	std::println("libblockfs/btrfs: link unimplemented!");
	co_return std::unexpected{protocols::fs::Error::internalError};
}

async::result<std::expected<uint64_t, protocols::fs::Error>>
unlink(std::shared_ptr<void> object, std::string name) {
	(void)object;
	(void)name;
	std::println("libblockfs/btrfs: unlink unimplemented!");
	co_return std::unexpected{protocols::fs::Error::internalError};
}

async::result<protocols::fs::FileStats> getStats(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<btrfs::Inode>(object);
	co_await self->readyEvent.wait();

	auto fs = static_cast<btrfs::FileSystem *>(&self->fs);

	BtreePtr ptr{};
	auto val = co_await fs->find(fs->fsTreeRoot_, {self->number, ItemType::INODE_ITEM}, ptr);
	assert(val);

	auto disk_inode = reinterpret_cast<const InodeItem *>(val->data());

	protocols::fs::FileStats stats;
	stats.linkCount = disk_inode->nlink;
	stats.fileSize = disk_inode->size;
	stats.mode = disk_inode->mode & 0xFFF;
	stats.uid = self->uid;
	stats.gid = self->gid;
	stats.accessTime.tv_sec = disk_inode->atime.sec;
	stats.accessTime.tv_nsec = disk_inode->atime.sec;
	stats.dataModifyTime.tv_sec = disk_inode->mtime.sec;
	stats.dataModifyTime.tv_nsec = disk_inode->mtime.sec;
	stats.anyChangeTime.tv_sec = disk_inode->ctime.sec;
	stats.anyChangeTime.tv_nsec = disk_inode->ctime.sec;

	co_return stats;
}

async::result<std::expected<std::string, protocols::fs::Error>> readSymlink(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<btrfs::Inode>(object);
	co_await self->readyEvent.wait();

	// readlink() is only valid on symbolic links.
	if(self->fileType != kTypeSymlink)
		co_return std::unexpected{protocols::fs::Error::illegalArguments};

	auto fs = static_cast<btrfs::FileSystem *>(&self->fs);

	BtreePtr ptr{};
	auto val = co_await fs->find(fs->fsTreeRoot_, {self->number, ItemType::EXTENTDATA_ITEM}, ptr);
	assert(val);

	auto ed = reinterpret_cast<const ExtentData *>(val->data());

	if (ed->type == 0) {
		size_t extentDataSize = val->size_bytes() - sizeof(*ed);
		auto symlinkData = val->subspan(sizeof(*ed), extentDataSize);
		co_return std::string{reinterpret_cast<char *>(symlinkData.data()), symlinkData.size_bytes()};
	} else {
		std::println("libblockfs/btrfs: readSymlink from regular extents is not implemented!");
		co_return {};
	}
}

async::result<std::expected<protocols::fs::MkdirResult, protocols::fs::Error>>
mkdir(std::shared_ptr<void> object, std::string name, uid_t, gid_t, mode_t) {
	(void)object;
	(void)name;
	std::println("libblockfs/btrfs: mkdir unimplemented!");
	co_return std::unexpected{protocols::fs::Error::internalError};
}

async::result<std::expected<protocols::fs::SymlinkResult, protocols::fs::Error>>
symlink(std::shared_ptr<void> object, std::string name, std::string target) {
	(void)object;
	(void)name;
	(void)target;
	std::println("libblockfs/btrfs: symlink unimplemented!");
	co_return std::unexpected{protocols::fs::Error::internalError};
}

async::result<protocols::fs::Error> chmod(std::shared_ptr<void> object, int mode) {
	(void)object;
	(void)mode;
	std::println("libblockfs/btrfs: chmod unimplemented!");
	co_return protocols::fs::Error::internalError;
}

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>> getLinkOrCreate(
    std::shared_ptr<void> object,
    std::string name,
    mode_t mode,
    bool exclusive,
    uid_t uid,
    gid_t gid
) {
	auto self = std::static_pointer_cast<btrfs::Inode>(object);
	auto findResult = co_await self->findEntry(name);
	if (!findResult)
		co_return std::unexpected{findResult.error()};

	auto result = findResult.value();
	if (result) {
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

	(void)mode;
	(void)uid;
	(void)gid;
	std::println("libblockfs/btrfs: getLinkOrCreate unimplemented!");
	co_return std::unexpected{protocols::fs::Error::internalError};
}

} // namespace

constinit protocols::fs::FileOperations fileOperations{
    .seekAbs = &doSeekAbs<FileSystem>,
    .seekRel = &doSeekRel<FileSystem>,
    .seekEof = &doSeekEof<FileSystem>,
    .read = &doRead<FileSystem>,
    .pread = &doPread<FileSystem>,
    .write = &doWrite<FileSystem>,
    .pwrite = &doPwrite<FileSystem>,
    .readEntries = &readEntries,
    .accessMemory = &doAccessMemory<FileSystem>,
    .truncate = &doTruncate<FileSystem>,
    .flock = &doFlock<FileSystem>,
    .getFileFlags = &getFileFlags,
    .setFileFlags = &setFileFlags,
};

constinit protocols::fs::NodeOperations nodeOperations{
    .getStats = &getStats,
    .getLink = &getLink,
    .link = &link,
    .unlink = &unlink,
    .open = &doOpen<FileSystem>,
    .readSymlink = &readSymlink,
    .mkdir = &mkdir,
    .symlink = &symlink,
    .chmod = &chmod,
    .utimensat = &doUtimensat<FileSystem>,
    .obstructLink = &doObstructLink<FileSystem>,
    .traverseLinks = &doTraverseLinks<FileSystem>,
    .getLinkOrCreate = &getLinkOrCreate
};

} // namespace blockfs::btrfs
