
#include <ranges>
#include <protocols/posix/procdata.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>
#include <sys/stat.h>
#include <print>
#include <linux/magic.h>

#include <async/result.hpp>
#include <core/align.hpp>
#include <core/clock.hpp>
#include <core/logging.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include <array>

#include "ext2fs.hpp"

namespace blockfs {
namespace ext2fs {

namespace {
	constexpr bool logSuperblock = true;

	constexpr int pageShift = 12;
	constexpr size_t pageSize = size_t{1} << pageShift;
}

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: BaseInode{fs, number}, fs{fs} { }

DiskInode *Inode::diskInode() {
	auto inodeAddress = (number - 1) * fs.inodeSize;
	return reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(fs.inodeTableMapping.get()) + inodeAddress);
}

void Inode::setFileSize(size_t size) {
	assert(!(size & ~uint64_t(0xFFFFFFFF)));
	diskInode()->size = size;
}

async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
Inode::findEntry(std::string name) {
	co_await readyEvent.wait();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;
	assert(fileMapping.size() == fileSize());

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Read the directory structure.
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(disk_entry->recordLength);

		if(disk_entry->inode
				&& name.length() == disk_entry->nameLength
				&& !memcmp(disk_entry->name, name.data(), name.length())) {
			DirEntry entry;
			entry.inode = disk_entry->inode;

			switch(disk_entry->fileType) {
			case EXT2_FT_REG_FILE:
				entry.fileType = kTypeRegular; break;
			case EXT2_FT_DIR:
				entry.fileType = kTypeDirectory; break;
			case EXT2_FT_SYMLINK:
				entry.fileType = kTypeSymlink; break;
			default:
				entry.fileType = kTypeNone;
			}

			co_return entry;
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == fileSize());

	co_return std::nullopt;
}

async::result<frg::expected<protocols::fs::Error, DirEntry>>
Inode::insertEntry(std::string name, int64_t ino, blockfs::FileType type) {
	assert(!name.empty() && name != "." && name != "..");
	assert(ino);

	co_await readyEvent.wait();

	assert(fileType == kTypeDirectory);
	assert(fileMapping.size() == fileSize());

	// Lock the mapping into memory before calling this function.
	auto appendDirEntry = [&](size_t offset, size_t length)
			-> async::result<DirEntry> {
		auto diskEntry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		memset(diskEntry, 0, sizeof(DiskDirEntry));
		diskEntry->inode = ino;
		diskEntry->recordLength = length;
		diskEntry->nameLength = name.length();
		switch (type) {
			case kTypeRegular:
				diskEntry->fileType = EXT2_FT_REG_FILE;
				break;
			case kTypeDirectory:
				diskEntry->fileType = EXT2_FT_DIR;
				break;
			case kTypeSymlink:
				diskEntry->fileType = EXT2_FT_SYMLINK;
				break;
			default:
				throw std::runtime_error("unexpected type");
		}
		memcpy(diskEntry->name, name.data(), name.length() + 1);

		// Flush the data to disk.
		// TODO: It would be enough to flush only one or two pages here.
		auto syncDir = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
		HEL_CHECK(syncDir.error());

		// Increment the target's link count.
		auto target = std::static_pointer_cast<Inode>(fs.accessInode(ino));
		co_await target->readyEvent.wait();
		target->diskInode()->linksCount++;

		// Flush the target inode to disk.
		auto syncInode = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle},
				target->diskInode(), fs.inodeSize);
		HEL_CHECK(syncInode.error());

		DirEntry entry;
		entry.inode = ino;
		entry.fileType = type;
		co_return entry;
	};

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	auto time = clk::getRealtime();
	diskInode()->mtime = time.tv_sec;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	// Space required for the new directory entry.
	// We use name.size() + 1 for the entry name length to account for the null terminator
	auto required = (sizeof(DiskDirEntry) + name.size() + 1 + 3) & ~size_t(3);

	// Walk the directory structure.
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto previous_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(previous_entry->recordLength);

		// Calculate available space after we contract previous_entry.
		auto contracted = (sizeof(DiskDirEntry) + previous_entry->nameLength + 3) & ~size_t(3);
		assert(previous_entry->recordLength >= contracted);
		auto available = previous_entry->recordLength - contracted;

		// Check whether we can shrink previous_entry and insert a new entry after it.
		if(available >= required) {
			// Update the existing dentry.
			previous_entry->recordLength = contracted;

			co_return co_await appendDirEntry(offset + contracted, available);
		}

		offset += previous_entry->recordLength;
	}
	assert(offset == fileSize());

	// If we made it this far, we ran out of space in the directory. Resize it.
	auto blockOffset = (offset & ~(fs.blockSize - 1)) >> fs.blockShift;
	auto newSize = (offset + fs.blockSize + 0xFFF) & ~size_t(0xFFF);
	setFileSize(newSize);
	co_await fs.assignDataBlocks(this, blockOffset, 1);
	auto resizeResult = co_await helix_ng::resizeMemory(
			helix::BorrowedDescriptor{backingMemory}, newSize);
	HEL_CHECK(resizeResult.error());
	fileMapping = helix::Mapping{helix::BorrowedDescriptor{frontalMemory},
			0, newSize,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// Now append the entry that we couldn't add before.
	{
		helix::LockMemoryView lock_memory;
		auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
				&lock_memory,
				0, newSize, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(lock_memory.error());

		co_return co_await appendDirEntry(offset, fileSize() - offset);
	}
}

async::result<frg::expected<protocols::fs::Error>> Inode::removeEntry(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyEvent.wait();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;
	assert(fileMapping.size() == fileSize());

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Read the directory structure.
	DiskDirEntry *previous_entry = nullptr;
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(disk_entry->recordLength);

		if(disk_entry->inode
				&& name.length() == disk_entry->nameLength
				&& !memcmp(disk_entry->name, name.data(), name.length())) {

			auto target = std::static_pointer_cast<Inode>(fs.accessInode(disk_entry->inode));
			co_await target->readyEvent.wait();

			// The directory should start with "." and "..". As those entries are never deleted,
			// we can assume that a previous entry exists.
			assert(previous_entry);
			previous_entry->recordLength += disk_entry->recordLength;

			// Flush the data to disk.
			// TODO: It would be enough to flush only one or two pages here.
			auto syncDir = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
			HEL_CHECK(syncDir.error());

			// Decrement the inode's link count
			if(--target->diskInode()->linksCount == 0) {
				// TODO: free the data blocks and set size to 0
				target->diskInode()->dtime = clk::getRealtime().tv_sec;
			}

			auto syncInode = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle},
					target->diskInode(), fs.inodeSize);
			HEL_CHECK(syncInode.error());

			co_return {};
		}

		offset += disk_entry->recordLength;
		previous_entry = disk_entry;
	}
	assert(offset == fileSize());

	co_return protocols::fs::Error::fileNotFound;
}

async::result<std::expected<bool, protocols::fs::Error>> Inode::isDirectoryEmpty() {
	co_await readyEvent.wait();

	if(fileType != kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::notDirectory};

	// Note: linksCount == 2 is necessary for empty directories
	//       (since they must not have subdirectories).
	//       However, this assumption can be broken if the FS is corrupt
	//       so it is more robust to not exploit it.

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Check the directory entries for anything other than "." and "..".
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
			reinterpret_cast<char*>(fileMapping.get()) + offset);
		assert(disk_entry);
		assert(disk_entry->recordLength);

		if(disk_entry->inode
			&& disk_entry->nameLength == 2
			&& disk_entry->name[0] == '.'
			&& disk_entry->name[1] == '.') {
			// ".."
		} else if(disk_entry->inode
			&& disk_entry->nameLength == 1
			&& disk_entry->name[0] == '.') {
			// "."
		} else if(disk_entry->inode) {
			// Directory has stuff in it.
			co_return false;
		}

		offset += disk_entry->recordLength;
	}

	co_return true;
}

async::result<std::expected<DirEntry, protocols::fs::Error>>
Inode::link(std::string name, int64_t ino, blockfs::FileType type) {
	// Check if an entry with this name already exists.
	auto existingResult = co_await findEntry(name);
	if(!existingResult)
		co_return std::unexpected{existingResult.error()};
	if(existingResult.value())
		co_return std::unexpected{protocols::fs::Error::alreadyExists};

	auto result = co_await insertEntry(name, ino, type);
	if(!result)
		co_return std::unexpected{result.error()};
	co_return result.value();
}

async::result<std::expected<DirEntry, protocols::fs::Error>> Inode::mkdir(std::string name, uid_t uid, gid_t gid, mode_t mode) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyEvent.wait();

	// Check if an entry with this name already exists.
	auto existing = co_await findEntry(name);
	if(!existing)
		co_return std::unexpected{existing.error()};
	if(existing.value())
		co_return std::unexpected{protocols::fs::Error::alreadyExists};

	auto dirNode = co_await fs.createDirectory();
	co_await dirNode->readyEvent.wait();

	co_await fs.assignDataBlocks(dirNode.get(), 0, 1);

	dirNode->setFileSize(fs.blockSize);
	auto resizeResult = co_await helix_ng::resizeMemory(
			helix::BorrowedDescriptor{dirNode->backingMemory},
			(fs.blockSize + 0xFFF) & ~size_t(0xFFF));
	HEL_CHECK(resizeResult.error());
	dirNode->fileMapping = helix::Mapping{helix::BorrowedDescriptor{dirNode->frontalMemory},
			0, fs.blockSize,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	helix::LockMemoryView lockMemory;
	auto mapSize = (dirNode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(dirNode->frontalMemory),
			&lockMemory,
			0, mapSize, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lockMemory.error());

	dirNode->diskInode()->uid = uid;
	dirNode->diskInode()->gid = gid;
	dirNode->diskInode()->mode = EXT2_S_IFDIR | mode;

	size_t offset = 0;
	auto dotEntry = reinterpret_cast<DiskDirEntry *>(dirNode->fileMapping.get());
	offset += (sizeof(DiskDirEntry) + 2 + 3) & ~size_t(3);

	dirNode->diskInode()->linksCount++;
	dotEntry->inode = dirNode->number;
	dotEntry->recordLength = offset;
	dotEntry->nameLength = 1;
	dotEntry->fileType = EXT2_FT_DIR;
	memcpy(dotEntry->name, ".", 2);

	auto dotDotEntry = reinterpret_cast<DiskDirEntry *>(
			reinterpret_cast<char *>(dirNode->fileMapping.get()) + offset);

	diskInode()->linksCount++;
	dotDotEntry->inode = number;
	dotDotEntry->recordLength = dirNode->fileSize() - offset;
	dotDotEntry->nameLength = 2;
	dotDotEntry->fileType = EXT2_FT_DIR;
	memcpy(dotDotEntry->name, "..", 3);

	// Synchronize this inode to update the linksCount
	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	// Synchronize the new directory's inode to update its linksCount
	auto syncNewInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			dirNode->diskInode(), fs.inodeSize);
	HEL_CHECK(syncNewInode.error());

	// Synchronize the data blocks
	auto syncNewDir = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			dirNode->fileMapping.get(), dirNode->fileSize());
	HEL_CHECK(syncNewDir.error());

	auto result = co_await insertEntry(name, dirNode->number, kTypeDirectory);
	if(!result)
		co_return std::unexpected{result.error()};
	co_return result.value();
}

async::result<std::expected<DirEntry, protocols::fs::Error>> Inode::symlink(std::string name, std::string target) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyEvent.wait();

	// Check if an entry with this name already exists.
	auto existing = co_await findEntry(name);
	if(!existing)
		co_return std::unexpected{existing.error()};
	if(existing.value())
		co_return std::unexpected{protocols::fs::Error::alreadyExists};

	auto newNode = co_await fs.createSymlink();
	co_await newNode->readyEvent.wait();

	newNode->setFileSize(target.size());

	if (target.size() <= 60) {
		// fast symlink: store target in the inode itself.
		memcpy(newNode->diskInode()->data.embedded, target.data(), target.size());
	} else {
		// slow symlink: store target in data blocks.
		auto numBlocks = (target.size() + fs.blockSize - 1) / fs.blockSize;
		co_await fs.assignDataBlocks(newNode.get(), 0, numBlocks);

		auto newSize = (target.size() + 0xFFF) & ~size_t(0xFFF);
		auto resizeResult = co_await helix_ng::resizeMemory(
		    helix::BorrowedDescriptor{newNode->backingMemory}, newSize
		);
		HEL_CHECK(resizeResult.error());

		newNode->fileMapping = helix::Mapping{
		    helix::BorrowedDescriptor{newNode->frontalMemory},
		    0,
		    newSize,
		    kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking
		};

		helix::LockMemoryView lockMemory;
		auto &&submit = helix::submitLockMemoryView(
		    helix::BorrowedDescriptor(newNode->frontalMemory),
		    &lockMemory,
		    0,
		    newSize,
		    helix::Dispatcher::global()
		);
		co_await submit.async_wait();
		HEL_CHECK(lockMemory.error());

		memcpy(newNode->fileMapping.get(), target.data(), target.size());

		auto syncData = co_await helix_ng::synchronizeSpace(
		    helix::BorrowedDescriptor{kHelNullHandle},
		    newNode->fileMapping.get(),
		    newNode->fileSize()
		);
		HEL_CHECK(syncData.error());
	}

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			newNode->diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	auto result = co_await insertEntry(name, newNode->number, kTypeSymlink);
	if(!result)
		co_return std::unexpected{result.error()};
	co_return result.value();
}

async::result<protocols::fs::Error> Inode::chmod(int mode) {
	co_await readyEvent.wait();

	diskInode()->mode = (diskInode()->mode & 0xFFFFF000) | mode;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Inode::chown(std::optional<uid_t> uid, std::optional<gid_t> gid) {
	co_await readyEvent.wait();

	if (uid)
		diskInode()->uid = *uid;
	if (gid)
		diskInode()->gid = *gid;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Inode::updateTimes(
		std::optional<timespec> atime,
		std::optional<timespec> mtime,
		std::optional<timespec> ctime) {
	if(atime)
		diskInode()->atime = atime->tv_sec;
	if(mtime)
		diskInode()->mtime = mtime->tv_sec;
	if(ctime)
		diskInode()->ctime = ctime->tv_sec;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return protocols::fs::Error::none;
}


async::result<frg::expected<protocols::fs::Error>>
Inode::ensureBackingBlocks(size_t offset, size_t length) {
	auto [alignedOffset, alignedSize] = core::alignExtend({offset, length}, fs.blockSize);
	size_t blockOffset = alignedOffset / fs.blockSize;
	size_t blockCount = alignedSize / fs.blockSize;
	co_await fs.assignDataBlocks(this, blockOffset, blockCount);

	co_return frg::success;
}

async::result<frg::expected<protocols::fs::Error>>
Inode::resizeFile(size_t newSize) {
	auto oldSize = fileSize();

	if (newSize > oldSize) {
		// TODO(qookie): Technically we only need to assign 0
		// blocks here, not allocate new ones. We also should
		// zero out the new blocks.
		FRG_CO_TRY(co_await ensureBackingBlocks(oldSize, newSize - oldSize));
	} else if (newSize < oldSize) {
		// TODO(qookie): Deallocate blocks if they're no longer within the file.
		std::println("libblockfs: Shrinking an Ext2 file does not free data blocks!");
	} else if (newSize == oldSize) {
		// Nothing to do.
		co_return frg::success;
	}

	auto resizeResult = co_await helix_ng::resizeMemory(
			helix::BorrowedDescriptor{backingMemory},
			(newSize + 0xFFF) & ~size_t(0xFFF));
	HEL_CHECK(resizeResult.error());
	setFileSize(newSize);
	auto syncInode = co_await helix_ng::synchronizeSpace(
		helix::BorrowedDescriptor{kHelNullHandle},
		diskInode(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return frg::success;
}

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

namespace {
	constexpr size_t superBlockOffset = 1024;
}

FileSystem::FileSystem(BlockDevice *device)
: device(device) {
}


extern protocols::fs::FileOperations fileOperations;
extern protocols::fs::NodeOperations nodeOperations;

const protocols::fs::FileOperations *FileSystem::fileOps() {
	return &fileOperations;
}

const protocols::fs::NodeOperations *FileSystem::nodeOps() {
	return &nodeOperations;
}

async::result<void> FileSystem::init() {
	size_t deviceSuperBlockSector = superBlockOffset / device->sectorSize;
	size_t deviceSuperBlockOffset = superBlockOffset % device->sectorSize;

	size_t deviceSuperBlockSectors = (1024 + deviceSuperBlockOffset + device->sectorSize - 1) / device->sectorSize;

	std::vector<uint8_t> buffer(deviceSuperBlockSectors * device->sectorSize);
	co_await device->readSectors(deviceSuperBlockSector, buffer.data(), deviceSuperBlockSectors);

	DiskSuperblock sb;
	memcpy(&sb, buffer.data() + deviceSuperBlockOffset, sizeof(DiskSuperblock));
	assert(sb.magic == 0xEF53);

	inodeSize = sb.inodeSize;
	blockShift = 10 + sb.logBlockSize;
	blockSize = 1024 << sb.logBlockSize;
	blockPagesShift = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock = blockSize / device->sectorSize;
	blocksPerGroup = sb.blocksPerGroup;
	inodesPerGroup = sb.inodesPerGroup;
	blocksCount = sb.blocksCount;
	inodesCount = sb.inodesCount;
	numBlockGroups = (sb.blocksCount + (sb.blocksPerGroup - 1)) / sb.blocksPerGroup;

	if(logSuperblock) {
		std::cout << "ext2fs: Revision is: " << sb.revLevel << std::endl;
		std::cout << "ext2fs: Block size is: " << blockSize << std::endl;
		std::cout << "ext2fs:     There are " << sb.blocksCount << " blocks" << std::endl;
		std::cout << "ext2fs: Inode size is: " << inodeSize << std::endl;
		std::cout << "ext2fs:     There are " << sb.inodesCount << " blocks" << std::endl;
		std::cout << "ext2fs:     First available inode is: " << sb.firstIno << std::endl;
		std::cout << "ext2fs: Optional features: " << sb.featureCompat
				<< ", w-required features: " << sb.featureRoCompat
				<< ", r/w-required features: " << sb.featureIncompat << std::endl;
		std::cout << "ext2fs: There are " << numBlockGroups << " block groups" << std::endl;
		std::cout << "ext2fs:     Blocks per group: " << blocksPerGroup << std::endl;
		std::cout << "ext2fs:     Inodes per group: " << inodesPerGroup << std::endl;
	}

	assert(blockSize >= device->sectorSize);
	assert(blockSize % device->sectorSize == 0);

	blockGroupDescriptorBuffer.resize(
			(numBlockGroups * sizeof(DiskGroupDesc) + device->sectorSize - 1) & ~(device->sectorSize - 1));
	bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer.data();

	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	co_await device->readSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer.data(), blockGroupDescriptorBuffer.size() / device->sectorSize);

	handleBgdtWriteback();

	// Create memory bundles to manage the block and inode bitmaps.
	HelHandle block_bitmap_frontal, inode_bitmap_frontal;
	HelHandle block_bitmap_backing, inode_bitmap_backing;
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), numBlockGroups << blockPagesShift,
			0, &block_bitmap_backing, &block_bitmap_frontal));
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), numBlockGroups << blockPagesShift,
			0, &inode_bitmap_backing, &inode_bitmap_frontal));
	blockBitmap = helix::UniqueDescriptor{block_bitmap_frontal};
	blockBitmapMapping = helix::Mapping{blockBitmap,
			0, numBlockGroups << blockPagesShift,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};
	inodeBitmap = helix::UniqueDescriptor{inode_bitmap_frontal};
	inodeBitmapMapping = helix::Mapping{inodeBitmap,
			0, numBlockGroups << blockPagesShift,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	manageBlockBitmap(helix::UniqueDescriptor{block_bitmap_backing});
	manageInodeBitmap(helix::UniqueDescriptor{inode_bitmap_backing});

	// Create a memory bundle to manage the inode table.
	assert(!((inodesPerGroup * inodeSize) & 0xFFF));
	HelHandle inode_table_frontal;
	HelHandle inode_table_backing;
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), inodesPerGroup * inodeSize * numBlockGroups,
			0, &inode_table_backing, &inode_table_frontal));
	inodeTable = helix::UniqueDescriptor{inode_table_frontal};
	inodeTableMapping = helix::Mapping{inodeTable,
			0, inodesPerGroup * inodeSize * numBlockGroups,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	manageInodeTable(helix::UniqueDescriptor{inode_table_backing});

	co_return;
}

async::detached FileSystem::handleBgdtWriteback() {
	while(true) {
		co_await bdgtWriteback.async_wait();

		auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
		co_await device->writeSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
				blockGroupDescriptorBuffer.data(), blockGroupDescriptorBuffer.size() / device->sectorSize);
	}
}

async::detached FileSystem::manageBlockBitmap(helix::UniqueDescriptor memory) {
	helix::Mapping bitmapMapping{memory,
			0, numBlockGroups << blockPagesShift,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		protocols::ostrace::Timer timer;

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: properly support multi-page blocks");

		for(size_t progress = 0; progress < manage.length(); progress += (1 << blockPagesShift)) {
			auto bg_idx = (manage.offset() + progress) >> blockPagesShift;
			auto block = bgdt[bg_idx].blockBitmap;
			assert(block);

			auto ptr = reinterpret_cast<std::byte *>(bitmapMapping.get())
					+ manage.offset() + progress;

			if(manage.type() == kHelManageInitialize) {
				co_await device->readSectors(block * sectorsPerBlock,
						ptr, sectorsPerBlock);
				HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
						manage.offset() + progress, 1 << blockPagesShift));
			}else{
				assert(manage.type() == kHelManageWriteback);

				co_await device->writeSectors(block * sectorsPerBlock,
						ptr, sectorsPerBlock);
				HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
						manage.offset() + progress, 1 << blockPagesShift));
			}
		}

		ostContext.emit(
			ostEvtExt2ManageBlockBitmap,
			ostAttrTime(timer.elapsed())
		);
	}
}

async::detached FileSystem::manageInodeBitmap(helix::UniqueDescriptor memory) {
	helix::Mapping bitmapMapping{memory,
			0, numBlockGroups << blockPagesShift,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		protocols::ostrace::Timer timer;

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: properly support multi-page blocks");

		for(size_t progress = 0; progress < manage.length(); progress += (1 << blockPagesShift)) {
			auto bg_idx = (manage.offset() + progress) >> blockPagesShift;
			auto block = bgdt[bg_idx].inodeBitmap;
			assert(block);

			auto ptr = reinterpret_cast<std::byte *>(bitmapMapping.get())
					+ manage.offset() + progress;

			if(manage.type() == kHelManageInitialize) {
				co_await device->readSectors(block * sectorsPerBlock,
						ptr, sectorsPerBlock);
				HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
						manage.offset() + progress, 1 << blockPagesShift));
			}else{
				assert(manage.type() == kHelManageWriteback);

				co_await device->writeSectors(block * sectorsPerBlock,
						ptr, sectorsPerBlock);
				HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
						manage.offset() + progress, 1 << blockPagesShift));
			}
		}

		ostContext.emit(
			ostEvtExt2ManageInodeBitmap,
			ostAttrTime(timer.elapsed())
		);
	}
}

async::detached FileSystem::manageInodeTable(helix::UniqueDescriptor memory) {
	helix::Mapping tableMapping{memory,
			0, inodesPerGroup * inodeSize * numBlockGroups,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		protocols::ostrace::Timer timer;

		// TODO: Make sure that we do not read/write past the end of the table.
		assert(!((inodesPerGroup * inodeSize) & (blockSize - 1)));

		auto sizePerGroup = inodesPerGroup * inodeSize;
		// TODO: It would be possible to support this by separating
		//       different block group inside the managed memory representing the inode table
		//       (or by having per-block-group managed memory objects for the inode table).
		if (sizePerGroup & (pageSize - 1))
			logPanic("Missing support for inode table sizes that are not multiples of the page size");

		size_t progress = 0;
		while (progress < manage.length()) {
			// TODO: Use shifts instead of division.
			auto bg_idx = (manage.offset() + progress) / sizePerGroup;
			auto bg_offset = (manage.offset() + progress) % sizePerGroup;
			auto block = bgdt[bg_idx].inodeTable;
			assert(block);

			// Do not cross block group boundaries.
			auto chunk = std::min(manage.length() - progress, sizePerGroup - bg_offset);
			assert(!(progress & (pageSize - 1))); // Guaranteed by the next assertion.
			assert(!(chunk & (pageSize - 1))); // Otherwise, the panic above would trigger.

			assert(bg_offset % device->sectorSize == 0);
			assert(chunk % device->sectorSize == 0);

			auto ptr = reinterpret_cast<std::byte *>(tableMapping.get())
					+ manage.offset() + progress;

			if(manage.type() == kHelManageInitialize) {
				co_await device->readSectors(block * sectorsPerBlock + bg_offset / device->sectorSize,
						ptr, chunk / device->sectorSize);
				HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
						manage.offset() + progress, chunk));
			}else{
				assert(manage.type() == kHelManageWriteback);

				co_await device->writeSectors(block * sectorsPerBlock + bg_offset / device->sectorSize,
						ptr, chunk / device->sectorSize);
				HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
						manage.offset() + progress, chunk));
			}

			progress += chunk;
		}

		ostContext.emit(
			ostEvtExt2ManageInode,
			ostAttrTime(timer.elapsed())
		);
	}
}

auto FileSystem::accessRoot() -> std::shared_ptr<BaseInode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<BaseInode> {
	assert(number > 0);
	std::weak_ptr<Inode> &inode_slot = activeInodes[number];
	std::shared_ptr<Inode> active_inode = inode_slot.lock();
	if(active_inode)
		return active_inode;

	auto new_inode = std::make_shared<Inode>(*this, number);
	inode_slot = std::weak_ptr<Inode>(new_inode);
	initiateInode(new_inode);

	return new_inode;
}

protocols::fs::FsStats FileSystem::getFsStats() {
	protocols::fs::FsStats stats{};
	stats.fsType = EXT2_SUPER_MAGIC;
	stats.blockSize = blockSize;
	stats.fragmentSize = blockSize;
	stats.numBlocks = blocksCount;
	stats.numInodes = inodesCount;
	stats.maxNameLength = 255; // Fixed for ext2.
	stats.flags = 0;

	// Sum over all block groups.
	for(uint32_t i = 0; i < numBlockGroups; i++) {
		stats.blocksFree += bgdt[i].freeBlocksCount;
		stats.inodesFree += bgdt[i].freeInodesCount;
	}
	stats.blocksFreeUser = stats.blocksFree;
	stats.inodesFreeUser = stats.inodesFree;

	return stats;
}

async::result<std::shared_ptr<BaseInode>> FileSystem::createRegular(int uid, int gid, uint32_t parentIno) {
	auto ino = co_await allocateInode(parentIno);
	assert(ino);

	// Lock the inode table.
	auto inodeAddress = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inodeAddress & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(inodeTableMapping.get()) + inodeAddress);
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFREG;
	disk_inode->generation = generation + 1;
	struct timespec time = clk::getRealtime();
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;
	disk_inode->uid = uid;
	disk_inode->gid = gid;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			disk_inode, inodeSize);
	HEL_CHECK(syncInode.error());

	co_return accessInode(ino);
}

async::result<std::shared_ptr<Inode>> FileSystem::createDirectory() {
	auto ino = co_await allocateInode(0, true);
	assert(ino);

	// Lock the inode table.
	auto inodeAddress = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inodeAddress & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(inodeTableMapping.get()) + inodeAddress);
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFDIR;
	disk_inode->generation = generation + 1;
	struct timespec time = clk::getRealtime();
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	// update usedDirsCount in the respective bgdt for this inode
	auto bg_idx = (ino - 1) / inodesPerGroup;
	bgdt[bg_idx].usedDirsCount++;
	bdgtWriteback.raise();

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			disk_inode, inodeSize);
	HEL_CHECK(syncInode.error());

	co_return std::static_pointer_cast<Inode>(accessInode(ino));
}

async::result<std::shared_ptr<Inode>> FileSystem::createSymlink() {
	auto ino = co_await allocateInode();
	assert(ino);

	// Lock the inode table.
	auto inodeAddress = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inodeAddress & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(inodeTableMapping.get()) + inodeAddress);
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFLNK;
	disk_inode->generation = generation + 1;
	struct timespec time = clk::getRealtime();
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			disk_inode, inodeSize);
	HEL_CHECK(syncInode.error());

	co_return std::static_pointer_cast<Inode>(accessInode(ino));
}

async::detached FileSystem::initiateInode(std::shared_ptr<Inode> inode) {
	// TODO: Use a shift instead of a division.
	auto inodeAddress = (inode->number - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inodeAddress & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());
	inode->diskLock = lock_inode.descriptor();

	auto disk_inode = inode->diskInode();
//	printf("Inode %u: file size: %u\n", inode->number, disk_inode.size);

	if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
		inode->fileType = kTypeRegular;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
		inode->fileType = kTypeSymlink;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
		inode->fileType = kTypeDirectory;
	}else{
		std::cerr << "ext2fs: Unexpected inode type " << (disk_inode->mode & EXT2_S_IFMT)
				<< " for inode " << inode->number << std::endl;
		abort();
	}

	// TODO: support large uid / gids
	inode->uid = disk_inode->uid;
	inode->gid = disk_inode->gid;

	// Allocate a page cache for the file.
	auto cache_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), cache_size, kHelManagedReadahead,
			&inode->backingMemory, &inode->frontalMemory));

	if (inode->fileType == kTypeDirectory) {
		auto mapSize = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
		inode->fileMapping = helix::Mapping{helix::BorrowedDescriptor{inode->frontalMemory},
				0, mapSize,
				kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
	}

	HelHandle frontalOrder1, frontalOrder2;
	HelHandle backingOrder1, backingOrder2;
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), 3 << blockPagesShift,
			0, &backingOrder1, &frontalOrder1));
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), (blockSize / 4) << blockPagesShift,
			0, &backingOrder2, &frontalOrder2));
	inode->indirectOrder1 = helix::UniqueDescriptor{frontalOrder1};
	inode->indirectOrder2 = helix::UniqueDescriptor{frontalOrder2};

	manageIndirect(inode, 1, helix::UniqueDescriptor{backingOrder1});
	manageIndirect(inode, 2, helix::UniqueDescriptor{backingOrder2});
	manageFileData(inode);

	inode->readyEvent.raise();
}

async::detached FileSystem::manageFileData(std::shared_ptr<Inode> inode) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor(inode->backingMemory),
				&manage, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		assert(manage.offset() + manage.length() <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF)));

		protocols::ostrace::Timer timer;

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{helix::BorrowedDescriptor{inode->backingMemory},
					static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtWrite};

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backed_size = std::min(manage.length(), inode->fileSize() - manage.offset());
			size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(num_blocks * inode->fs.blockSize <= manage.length());
			co_await inode->fs.readDataBlocks(inode, manage.offset() / inode->fs.blockSize,
					num_blocks, file_map.get());

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping fileMap{helix::BorrowedDescriptor{inode->backingMemory},
					static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtRead};

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backedSize = std::min(manage.length(), inode->fileSize() - manage.offset());
			auto blockOffset = manage.offset() / inode->fs.blockSize;
			size_t numBlocks = (backedSize + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(numBlocks * inode->fs.blockSize <= manage.length());

			co_await inode->fs.assignDataBlocks(inode.get(), blockOffset, numBlocks);
			co_await inode->fs.writeDataBlocks(inode, blockOffset, numBlocks, fileMap.get());

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageWriteback,
					manage.offset(), manage.length()));
		}

		ostContext.emit(
			ostEvtExt2ManageFile,
			ostAttrTime(timer.elapsed())
		);
	}
}

async::detached FileSystem::manageIndirect(std::shared_ptr<Inode> inode,
		int order, helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		helix::Mapping outMap{memory,
			static_cast<ptrdiff_t>(manage.offset()), manage.length()};

		// TODO(qookie): This can probably implemented in a more optimal manner.
		for (size_t progress = 0; progress < manage.length(); progress += blockSize) {
			auto offset = manage.offset() + progress;
			uint32_t element = offset >> blockPagesShift;

			uint32_t block;
			if(order == 1) {
				auto disk_inode = inode->diskInode();

				switch(element) {
					case 0: block = disk_inode->data.blocks.singleIndirect; break;
					case 1: block = disk_inode->data.blocks.doubleIndirect; break;
					case 2: block = disk_inode->data.blocks.tripleIndirect; break;
					default:
						assert(!"unexpected offset");
						abort();
				}
			}else{
				assert(order == 2);

				auto indirect_frame = element >> (blockShift - 2);
				auto indirect_index = element & ((1 << (blockShift - 2)) - 1);

				helix::LockMemoryView lock_indirect;
				auto &&submit_indirect = helix::submitLockMemoryView(inode->indirectOrder1,
						&lock_indirect,
						(1 + indirect_frame) << blockPagesShift, 1 << blockPagesShift,
						helix::Dispatcher::global());
				co_await submit_indirect.async_wait();
				HEL_CHECK(lock_indirect.error());

				helix::Mapping indirect_map{inode->indirectOrder1,
					(1 + indirect_frame) << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
				block = reinterpret_cast<uint32_t *>(indirect_map.get())[indirect_index];
			}

			auto ptr = reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(outMap.get()) + progress);
			if (manage.type() == kHelManageInitialize) {
				co_await device->readSectors(block * sectorsPerBlock,
						ptr, sectorsPerBlock);
			} else {
				assert(manage.type() == kHelManageWriteback);
				co_await device->writeSectors(block * sectorsPerBlock,
						ptr, sectorsPerBlock);
			}
		}

		if (manage.type() == kHelManageInitialize) {
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
							manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
							manage.offset(), manage.length()));
		}
	}
}

async::result<std::vector<uint32_t>> FileSystem::allocateBlocks(size_t num, std::optional<uint32_t> ino) {
	protocols::ostrace::Timer timer;
	std::vector<uint32_t> result;

	if (ino) {
		uint32_t preferred_bg = (*ino - 1) / inodesPerGroup;

		if(bgdt[preferred_bg].freeBlocksCount) {
			helix::LockMemoryView lock_bitmap;
			auto &&submit_bitmap = helix::submitLockMemoryView(blockBitmap,
				&lock_bitmap,
				preferred_bg << blockPagesShift, 1 << blockPagesShift,
				helix::Dispatcher::global());
			co_await submit_bitmap.async_wait();
			HEL_CHECK(lock_bitmap.error());

			auto words = reinterpret_cast<uint32_t *>(
				reinterpret_cast<std::byte *>(blockBitmapMapping.get()) + (preferred_bg << blockPagesShift));

			for(unsigned int i = 0; i < (blocksPerGroup + 31) / 32; i++) {
				if(words[i] == 0xFFFFFFFF)
					continue;
				for(int j = 0; j < 32; j++) {
					if(i * 32 + j >= blocksPerGroup)
						break;
					if(words[i] & (static_cast<uint32_t>(1) << j))
						continue;
					// TODO: Make sure we never return reserved blocks.
					// TODO: Make sure we never return blocks higher than the max. block in the SB.
					auto block = preferred_bg * blocksPerGroup + i * 32 + j;
					assert(block);
					assert(block < blocksCount);
					words[i] |= static_cast<uint32_t>(1) << j;

					bgdt[preferred_bg].freeBlocksCount--;

					result.push_back(block);
					if(result.size() == num) {
						auto syncBitmap = co_await helix_ng::synchronizeSpace(
								helix::BorrowedDescriptor{kHelNullHandle},
								words, 1 << blockPagesShift);
						HEL_CHECK(syncBitmap.error());

						ostContext.emit(
							ostEvtExt2AllocateBlocks,
							ostAttrTime(timer.elapsed())
						);
						co_return result;
					}
				}
			}
		}
	}

	for(uint32_t bg_idx = 0; bg_idx < numBlockGroups; bg_idx++) {
		if(!bgdt[bg_idx].freeBlocksCount)
			continue;

		helix::LockMemoryView lock_bitmap;
		auto &&submit_bitmap = helix::submitLockMemoryView(blockBitmap,
				&lock_bitmap,
				bg_idx << blockPagesShift, 1 << blockPagesShift,
				helix::Dispatcher::global());
		co_await submit_bitmap.async_wait();
		HEL_CHECK(lock_bitmap.error());

		auto words = reinterpret_cast<uint32_t *>(
				reinterpret_cast<std::byte *>(blockBitmapMapping.get()) + (bg_idx << blockPagesShift));
		for(unsigned int i = 0; i < (blocksPerGroup + 31) / 32; i++) {
			if(words[i] == 0xFFFFFFFF)
				continue;
			for(int j = 0; j < 32; j++) {
				if(i * 32 + j >= blocksPerGroup)
					break;
				if(words[i] & (static_cast<uint32_t>(1) << j))
					continue;
				// TODO: Make sure we never return reserved blocks.
				// TODO: Make sure we never return blocks higher than the max. block in the SB.
				auto block = bg_idx * blocksPerGroup + i * 32 + j;
				assert(block);
				assert(block < blocksCount);
				words[i] |= static_cast<uint32_t>(1) << j;

				bgdt[bg_idx].freeBlocksCount--;
				result.push_back(block);
				if(result.size() == num) {
					auto syncBitmap = co_await helix_ng::synchronizeSpace(
							helix::BorrowedDescriptor{kHelNullHandle},
							words, 1 << blockPagesShift);
					HEL_CHECK(syncBitmap.error());

					ostContext.emit(
						ostEvtExt2AllocateBlocks,
						ostAttrTime(timer.elapsed())
					);
					co_return result;
				}
			}
		}
	}

	assert(!"Failed to find zero-bit");
}

async::result<uint32_t> FileSystem::allocateInode(uint32_t parentIno, bool directory) {
	protocols::ostrace::Timer timer;

	auto searchBlockGroup = [&](uint32_t bg) -> async::result<std::optional<uint32_t>> {
		helix::LockMemoryView lock_bitmap;
		auto &&submit_bitmap = helix::submitLockMemoryView(inodeBitmap,
				&lock_bitmap,
				bg << blockPagesShift, 1 << blockPagesShift,
				helix::Dispatcher::global());
		co_await submit_bitmap.async_wait();
		HEL_CHECK(lock_bitmap.error());

		auto words = reinterpret_cast<uint32_t *>(
				reinterpret_cast<std::byte *>(inodeBitmapMapping.get()) + (bg << blockPagesShift));
		for(unsigned int i = 0; i < (inodesPerGroup + 31) / 32; i++) {
			if(words[i] == 0xFFFFFFFF)
				continue;
			for(int j = 0; j < 32; j++) {
				if(i * 32 + j >= inodesPerGroup)
					break;
				if(words[i] & (static_cast<uint32_t>(1) << j))
					continue;

				// TODO: Make sure we never return reserved inodes.
				// TODO: Make sure we never return inodes higher than the max. inode in the SB.
				auto ino = bg * inodesPerGroup + i * 32 + j + 1;
				assert(ino);
				assert(ino < inodesCount);
				words[i] |= static_cast<uint32_t>(1) << j;

				bgdt[bg].freeInodesCount--;
				if(directory)
					bgdt[bg].usedDirsCount++;

				bdgtWriteback.raise();

				auto syncBitmap = co_await helix_ng::synchronizeSpace(
						helix::BorrowedDescriptor{kHelNullHandle},
						words, 1 << blockPagesShift);
				HEL_CHECK(syncBitmap.error());

				ostContext.emit(
					ostEvtExt2AllocateInode,
					ostAttrTime(timer.elapsed())
				);

				co_return ino;
			}
		}

		co_return std::nullopt;
	};

	if(parentIno) {
		auto preferred_bg = (parentIno - 1) / inodesPerGroup;
		if(bgdt[preferred_bg].freeInodesCount) {
			auto ino = co_await searchBlockGroup(preferred_bg);
			if(ino)
				co_return *ino;
		}

		// search the next block group in exponential offsets % numBlockGroups
		size_t expOffset = 1;

		while(expOffset < numBlockGroups) {
			auto exp_bg = (preferred_bg + expOffset) % numBlockGroups;
			if(bgdt[exp_bg].freeInodesCount) {
				auto ino = co_await searchBlockGroup(exp_bg);
				if(ino)
					co_return *ino;
			}

			expOffset <<= 1;
		}
	}

	// exhaustive linear search
	for(uint32_t bg_idx = 0; bg_idx < numBlockGroups; bg_idx++) {
		if(!bgdt[bg_idx].freeInodesCount)
			continue;

		auto ino = co_await searchBlockGroup(bg_idx);
		if(ino)
			co_return *ino;
	}

	ostContext.emit(
		ostEvtExt2AllocateInode,
		ostAttrTime(timer.elapsed())
	);

	co_return 0;
}

async::result<void> FileSystem::assignDataBlocks(Inode *inode,
		uint64_t block_offset, size_t num_blocks) {
	protocols::ostrace::Timer timer;

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	auto disk_inode = inode->diskInode();

	size_t prg = 0;
	while(prg < num_blocks) {
		if(block_offset + prg < i_range) {
			while(prg < num_blocks
					&& block_offset + prg < i_range) {
				auto idx = block_offset + prg;

				size_t range = 0;
				for(size_t i = idx; i < i_range; i++) {
					if(prg + range >= num_blocks)
						break;

					if(disk_inode->data.blocks.direct[i])
						break;

					range++;
				}

				if(!range) {
					prg++;
					continue;
				}

				auto allocated = co_await allocateBlocks(range, inode->number);
				for (auto const [blocknum, block] : std::views::enumerate(allocated))
					disk_inode->data.blocks.direct[idx + blocknum] = block;

				disk_inode->blocks += allocated.size() * (blockSize / 512);
				prg += allocated.size();
			}
		}else if(block_offset + prg < s_range) {
			bool needsReset = false;

			// Allocate the single-indirect block itself.
			if(!disk_inode->data.blocks.singleIndirect) {
				auto block = co_await allocateBlocks(1, inode->number);
				assert(!block.empty() && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				disk_inode->data.blocks.singleIndirect = block[0];
				needsReset = true;
			}

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
			auto window = reinterpret_cast<uint32_t *>(indirect_map.get());

			if(needsReset)
				memset(window, 0, size_t{1} << blockPagesShift);

			while(prg < num_blocks
					&& block_offset + prg < s_range) {
				auto idx = block_offset + prg - i_range;

				size_t range = 0;
				for(size_t i = idx; i < per_single; i++) {
					if(prg + range >= num_blocks)
						break;

					if(window[i])
						break;

					range++;
				}

				if(!range) {
					prg++;
					continue;
				}

				auto allocated = co_await allocateBlocks(range, inode->number);
				for (auto const [blocknum, block] : std::views::enumerate(allocated))
					window[idx + blocknum] = block;

				disk_inode->blocks += allocated.size() * (blockSize / 512);
				prg += allocated.size();
			}
		}else if(block_offset + prg < d_range) {
			bool doubleNeedsReset = false;
			if(!disk_inode->data.blocks.doubleIndirect) {
				auto block = co_await allocateBlocks(1, inode->number);
				assert(!block.empty() && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				disk_inode->data.blocks.doubleIndirect = block[0];
				doubleNeedsReset = true;
			}

			helix::LockMemoryView lock_double_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_double_indirect, 1 << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_double_indirect.error());

			helix::Mapping double_indirect_map{inode->indirectOrder1,
					1 << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
			auto double_window = reinterpret_cast<uint32_t *>(double_indirect_map.get());

			if(doubleNeedsReset)
				memset(double_window, 0, size_t{1} << blockPagesShift);

			while(prg < num_blocks
					&& block_offset + prg < d_range) {
				int64_t indirect_frame = (block_offset + prg - s_range) >> (blockShift - 2);
				int64_t indirect_index = (block_offset + prg - s_range) & ((1 << (blockShift - 2)) - 1);

				bool needsReset = false;
				if(!double_window[indirect_frame]) {
					// Allocate the single indirect block.
					auto block = co_await allocateBlocks(1, inode->number);
					assert(!block.empty() && "Out of disk space"); // TODO: Fix this.
					disk_inode->blocks += (blockSize / 512);
					double_window[indirect_frame] = block[0];
					needsReset = true;
				}

				helix::LockMemoryView lock_indirect;
				auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2,
						&lock_indirect, indirect_frame << blockPagesShift, 1 << blockPagesShift,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(lock_indirect.error());

				helix::Mapping indirect_map{inode->indirectOrder2,
						indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
						kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
				auto window = reinterpret_cast<uint32_t *>(indirect_map.get());

				if(needsReset)
					memset(window, 0, size_t{1} << blockPagesShift);

				size_t range = 0;
				for(size_t i = indirect_index; i < per_double; i++) {
					if(prg + range >= num_blocks)
						break;

					if(window[i])
						break;

					range++;
				}

				if(!range) {
					prg++;
					continue;
				}

				auto allocated = co_await allocateBlocks(range, inode->number);
				for (auto const [blocknum, block] : std::views::enumerate(allocated))
					window[indirect_index + blocknum] = block;

				disk_inode->blocks += allocated.size() * (blockSize / 512);
				prg += allocated.size();
			}
		}else{
			assert(!"TODO: Implement allocation in triple indirect blocks");
		}
	}

	bdgtWriteback.raise();
	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			inode->diskInode(), inodeSize);
	HEL_CHECK(syncInode.error());

	ostContext.emit(
		ostEvtExt2AssignDataBlocks,
		ostAttrTime(timer.elapsed())
	);
}

async::result<void> FileSystem::readDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, void *buffer) {
	// We perform "block-fusion" here i.e. we try to read/write multiple
	// consecutive blocks in a single read/writeSectors() operation.
	auto fuse = [] (size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && n < limit) {
			if ((list[0] && (list[n] != list[0] + n)) || (!list[0] && list[n]))
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[0], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	co_await inode->readyEvent.wait();
	// TODO: Assert that we do not read past the EOF.

	constexpr size_t indirectBufferSize = 8;

	std::array<uint32_t, indirectBufferSize> indirectBuffer;

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the readSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Reading " << index << "-th block from inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			auto remaining = num_blocks - progress;
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			if (remaining > indirectBufferSize) {
				helix::LockMemoryView lock_indirect;
				auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2, &lock_indirect,
						indirect_frame << blockPagesShift, 1 << blockPagesShift,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(lock_indirect.error());

				helix::Mapping indirect_map{inode->indirectOrder2,
						indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
						kHelMapProtRead | kHelMapDontRequireBacking};

				issue = fuse(remaining,
						reinterpret_cast<uint32_t *>(indirect_map.get()) + indirect_index,
						per_indirect - indirect_index);
			} else {
				auto readMemory = co_await helix_ng::readMemory(
						helix::BorrowedDescriptor{inode->indirectOrder2},
						(indirect_frame << blockPagesShift) + indirect_index * 4,
						remaining * 4, indirectBuffer.data());
				HEL_CHECK(readMemory.error());

				issue = fuse(remaining, indirectBuffer.data(), remaining);
			}
		}else if(index >= i_range) { // Use the single indirect block.
			auto remaining = num_blocks - progress;
			auto indirect_index = index - i_range;

			if (remaining > indirectBufferSize) {
				helix::LockMemoryView lock_indirect;
				auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
						&lock_indirect, 0, 1 << blockPagesShift,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(lock_indirect.error());

				helix::Mapping indirect_map{inode->indirectOrder1,
						0, size_t{1} << blockPagesShift,
						kHelMapProtRead | kHelMapDontRequireBacking};

				issue = fuse(remaining,
						reinterpret_cast<uint32_t *>(indirect_map.get()) + indirect_index,
						per_indirect - indirect_index);
			} else {
				auto readMemory = co_await helix_ng::readMemory(
						helix::BorrowedDescriptor{inode->indirectOrder1},
						indirect_index * 4, remaining * 4, indirectBuffer.data());
				HEL_CHECK(readMemory.error());

				issue = fuse(remaining, indirectBuffer.data(), remaining);
			}
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(num_blocks - progress,
					disk_inode->data.blocks.direct + index, 12 - index);
		}

//		std::cout << "Issuing read of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		if (issue.first) {
			co_await device->readSectors(issue.first * sectorsPerBlock,
					(uint8_t *)buffer + progress * blockSize,
					issue.second * sectorsPerBlock);
		} else {
			memset((uint8_t *)buffer + progress * blockSize, 0, issue.second * blockSize);
		}
		progress += issue.second;
	}
}

// TODO: There is a lot of overlap between this method and readDataBlocks.
//       Refactor common code into a another method.
async::result<void> FileSystem::writeDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, const void *buffer) {
	// We perform "block-fusion" here i.e. we try to read/write multiple
	// consecutive blocks in a single read/writeSectors() operation.
	auto fuse = [] (size_t index, size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && index + n < limit) {
			if(list[index + n] != list[index] + n)
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[index], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	co_await inode->readyEvent.wait();
	// TODO: Assert that we do not write past the EOF.

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the writeSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Write " << index << "-th block to inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			// TODO: Use shift/and instead of div/mod.
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2, &lock_indirect,
					indirect_frame << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder2,
					indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};

			issue = fuse(indirect_index, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else if(index >= i_range) { // Use the triple indirect block.
			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			issue = fuse(index - i_range, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(index, num_blocks - progress,
					disk_inode->data.blocks.direct, 12);
		}

//		std::cout << "Issuing write of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first);
		co_await device->writeSectors(issue.first * sectorsPerBlock,
				(const uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}
}

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
OpenFile::readEntries() {
	auto inode = std::static_pointer_cast<Inode>(this->inode);

	co_await inode->readyEvent.wait();

	if (inode->fileType != kTypeDirectory) {
		std::cout << "\e[33m" "ext2fs: readEntries called on something that's not a directory\e[39m" << std::endl;
		co_return std::unexpected(managarm::fs::Errors::NOT_DIRECTORY);
	}

	auto map_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(inode->frontalMemory),
			&lock_memory, 0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{inode->frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	// Read the directory structure.
	assert(offset <= inode->fileSize());
	while(offset < inode->fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= inode->fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(file_map.get()) + offset);
		assert(offset + disk_entry->recordLength <= inode->fileSize());

		offset += disk_entry->recordLength;

		if(disk_entry->inode) {
		//	std::cout << "libblockfs: Returning entry "
		//			<< std::string(disk_entry->name, disk_entry->nameLength) << std::endl;
			co_return protocols::fs::ReadEntriesResult{
				.name = std::string(disk_entry->name, disk_entry->nameLength),
				.inode = disk_entry->inode,
				.offset = static_cast<long>(offset),
			};
		}
	}
	assert(offset == inode->fileSize());

	co_return std::unexpected(managarm::fs::Errors::END_OF_FILE);
}

} } // namespace blockfs::ext2fs

