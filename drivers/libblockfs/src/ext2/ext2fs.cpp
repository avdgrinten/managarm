
#include <ranges>
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
#include <frg/scope_exit.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include <array>
#include <bit>

#include "ext2fs.hpp"
#include "extents.hpp"
#include "../checksums.hpp"
#include "../trace.hpp"

namespace blockfs {
namespace ext2fs {

namespace {
	constexpr bool logSuperblock = true;

	void updateInodeChecksum(FileSystem &fs, DiskInode *inode, uint32_t number) {
		if(fs.metadataChecksum) {
			inode->osd2.checksumLow = 0;

			bool extra = fs.inodeSize >= offsetof(DiskInode, extraSize) + 2 && inode->extraSize >= 4;
			if(extra)
				inode->checksumHigh = 0;

			checksums::Crc32c crc32{fs.metadataChecksumSeed};
			crc32.addData(&number, sizeof(number));
			crc32.addData(&inode->generation, sizeof(inode->generation));
			crc32.addData(inode, fs.inodeSize);

			uint32_t value = crc32.finalize();
			inode->osd2.checksumLow = value & 0xffff;
			if(extra)
				inode->checksumHigh = (value >> 16) & 0xffff;
		}
	}

	void updateExtentChecksum(FileSystem &fs, Inode *inode, ExtentHeader *hdr) {
		if(fs.metadataChecksum) {
			size_t contentSize = sizeof(ExtentHeader) + hdr->max * sizeof(Extent);
			auto diskInode = inode->diskInode();
			uint32_t number = inode->number;

			checksums::Crc32c crc32{fs.metadataChecksumSeed};
			crc32.addData(&number, sizeof(number));
			crc32.addData(&diskInode->generation, sizeof(diskInode->generation));
			crc32.addData(hdr, contentSize);

			uint32_t value = crc32.finalize();
			*reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(hdr) + contentSize) = value;
		}
	}

	void updateBlockGroupChecksum(FileSystem &fs, DiskGroupDesc *desc, uint32_t groupNumber) {
		if(fs.metadataChecksum) {
			desc->checksum = 0;

			checksums::Crc32c crc32{fs.metadataChecksumSeed};
			crc32.addData(&groupNumber, sizeof(groupNumber));
			crc32.addData(desc, fs.bgdt.descriptorSize());

			uint32_t value = crc32.finalize();
			desc->checksum = value;
		} else if(fs.bgdtChecksum) {
			desc->checksum = 0;

			checksums::Crc16 crc16{0xffff};
			crc16.addData(fs.uuid, sizeof(fs.uuid));
			crc16.addData(&groupNumber, sizeof(groupNumber));
			crc16.addData(desc, fs.bgdt.descriptorSize());

			uint16_t value = crc16.finalize();
			desc->checksum = value;
		}
	}

	void updateBlockBitmapChecksum(FileSystem &fs, DiskGroupDesc *desc, const void *bitmap, size_t bitmapSize) {
		if(fs.metadataChecksum) {
			checksums::Crc32c crc32{fs.metadataChecksumSeed};
			crc32.addData(bitmap, bitmapSize);

			uint32_t value = crc32.finalize();
			desc->blockBitmapCsumLow = value & 0xffff;

			if(fs.bgdt.descriptorSize() >= offsetof(DiskGroupDesc, blockBitmapCsumHigh) + 2)
				desc->blockBitmapCsumHigh = value >> 16;
		}
	}

	void updateInodeBitmapChecksum(FileSystem &fs, DiskGroupDesc *desc, const void *bitmap, size_t bitmapSize) {
		if(fs.metadataChecksum) {
			checksums::Crc32c crc32{fs.metadataChecksumSeed};
			crc32.addData(bitmap, bitmapSize);

			uint32_t value = crc32.finalize();
			desc->inodeBitmapCsumLow = value & 0xffff;

			if(fs.bgdt.descriptorSize() >= offsetof(DiskGroupDesc, inodeBitmapCsumHigh) + 2)
				desc->inodeBitmapCsumHigh = value >> 16;
		}
	}
}

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: BaseInode{fs, number}, fs{fs}, usesExtents{false} { }

DiskInode *Inode::diskInode() {
	return reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(diskInodeWindow.get()) + diskInodeOffset);
}

void Inode::setFileSize(size_t size) {
	assert(!(size & ~uint64_t(0xFFFFFFFF)));
	diskInode()->size = size;
}

async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
Inode::findEntry(std::string name) {
	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timePin = 0;
	uint64_t scanned = 0;
	bool found = false;
	frg::scope_exit evtOnExit{[&] {
		auto timeScan = timer.split();
		ostContext.emit(
			ostEvtExt2FindEntry,
			ostAttrTime(timer.elapsed()),
			ostAttrNumBytes(scanned),
			ostAttrTimeReady(timeReady),
			ostAttrTimePin(timePin),
			ostAttrTimeScan(timeScan),
			ostAttrFound(found)
		);
	}};

	co_await readyEvent.wait();
	timeReady = timer.split();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;

	assert(fileMapping.size() == ((fileSize() + 0xFFF) & ~size_t(0xFFF)));

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());
	timePin = timer.split();

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
			scanned = offset;
			found = true;

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
	scanned = offset;

	co_return std::nullopt;
}

async::result<frg::expected<protocols::fs::Error, DirEntry>>
Inode::insertEntry(std::string name, int64_t ino, blockfs::FileType type) {
	assert(!name.empty() && name != "." && name != "..");
	assert(ino);

	protocols::ostrace::Timer timer;
	size_t dirSize = 0;
	uint64_t timeReady = 0;
	uint64_t timePin = 0;
	uint64_t timeScan = 0;
	uint64_t growTime = 0;
	uint64_t cleanDirTime = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2InsertEntry,
			ostAttrTime(timer.elapsed()),
			ostAttrFileSize(dirSize),
			ostAttrTimeReady(timeReady),
			ostAttrTimePin(timePin),
			ostAttrTimeScan(timeScan),
			ostAttrTimeGrow(growTime),
			ostAttrTimeCleanDir(cleanDirTime)
		);
	}};

	co_await readyEvent.wait();
	timeReady = timer.split();
	dirSize = fileSize();

	assert(fileType == kTypeDirectory);
	assert(fileMapping.size() == ((fileSize() + 0xFFF) & ~size_t(0xFFF)));

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

		// Hand the modified pages over to writeback.
		// TODO: It would be enough to clean only one or two pages here.
		protocols::ostrace::Timer cleanDirTimer;
		auto syncDir = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
		HEL_CHECK(syncDir.error());
		cleanDirTime += cleanDirTimer.elapsed();

		// Increment the target's link count.
		// This is sound since the caller holds the target's inodeMutex exclusively.
		auto target = std::static_pointer_cast<Inode>(fs.accessInode(ino));
		co_await target->readyEvent.wait();
		target->diskInode()->linksCount++;

		updateInodeChecksum(fs, target->diskInode(), ino);

		// Queue the target inode for writeback.
		target->diskInodeWindow.markDirty();

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
	timePin = timer.split();

	auto time = clk::getRealtime();
	diskInode()->mtime = time.tv_sec;

	// A new subdirectory adds a ".." backlink to this directory.
	if(type == kTypeDirectory)
		diskInode()->linksCount++;

	updateInodeChecksum(fs, diskInode(), number);

	diskInodeWindow.markDirty();

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

			timeScan = timer.split();
			co_return co_await appendDirEntry(offset + contracted, available);
		}

		offset += previous_entry->recordLength;
	}
	assert(offset == fileSize());

	// If we made it this far, we ran out of space in the directory. Resize it.
	timeScan = timer.split();
	protocols::ostrace::Timer growTimer;
	auto blockOffset = (offset & ~(fs.blockSize - 1)) >> fs.blockShift;
	auto newSize = offset + fs.blockSize;
	auto newMappingSize = (newSize + 0xFFF) & ~size_t(0xFFF);
	setFileSize(newSize);

	{
		co_await blockMapMutex.async_lock();
		frg::unique_lock blockMapLock{frg::adopt_lock, blockMapMutex};
		co_await fs.assignDataBlocks(this, blockOffset, 1);
	}

	auto resizeResult = co_await helix_ng::resizeMemory(
			helix::BorrowedDescriptor{backingMemory}, newMappingSize);
	HEL_CHECK(resizeResult.error());
	fileMapping = helix::Mapping{helix::BorrowedDescriptor{frontalMemory},
			0, newMappingSize,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// Now append the entry that we couldn't add before.
	{
		helix::LockMemoryView lock_memory;
		auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
				&lock_memory,
				0, newMappingSize, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(lock_memory.error());
		growTime = growTimer.elapsed();

		co_return co_await appendDirEntry(offset, fileSize() - offset);
	}
}

async::result<frg::expected<protocols::fs::Error>> Inode::removeEntry(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timePin = 0;
	uint64_t scanned = 0;
	uint64_t cleanDirTime = 0;
	frg::scope_exit evtOnExit{[&] {
		auto timeScan = timer.split() - cleanDirTime;
		ostContext.emit(
			ostEvtExt2RemoveEntry,
			ostAttrTime(timer.elapsed()),
			ostAttrNumBytes(scanned),
			ostAttrTimeReady(timeReady),
			ostAttrTimePin(timePin),
			ostAttrTimeScan(timeScan),
			ostAttrTimeCleanDir(cleanDirTime)
		);
	}};

	co_await readyEvent.wait();
	timeReady = timer.split();

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
	timePin = timer.split();

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

			scanned = offset;

			auto target = std::static_pointer_cast<Inode>(fs.accessInode(disk_entry->inode));
			co_await target->readyEvent.wait();

			auto targetIno = disk_entry->inode;
			if(offset & (fs.blockSize - 1)) {
				previous_entry->recordLength += disk_entry->recordLength;
			} else {
				// The directory entry is at the start of a block. We mark it as unused instead of merging it.
				disk_entry->inode = 0;
			}

			// Hand the modified pages over to writeback.
			// TODO: It would be enough to clean only one or two pages here.
			protocols::ostrace::Timer cleanDirTimer;
			auto syncDir = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
			HEL_CHECK(syncDir.error());
			cleanDirTime += cleanDirTimer.elapsed();

			// Decrement the inode's link count
			// This is sound since the caller holds the target's inodeMutex exclusively.
			if(--target->diskInode()->linksCount == 0) {
				// TODO: free the data blocks and set size to 0
				target->diskInode()->dtime = clk::getRealtime().tv_sec;
			}

			updateInodeChecksum(fs, target->diskInode(), targetIno);

			target->diskInodeWindow.markDirty();

			// A removed subdirectory drops its ".." backlink to this directory.
			if(target->fileType == kTypeDirectory) {
				diskInode()->linksCount--;

				updateInodeChecksum(fs, diskInode(), number);

				diskInodeWindow.markDirty();
			}

			co_return {};
		}

		offset += disk_entry->recordLength;
		previous_entry = disk_entry;
	}
	assert(offset == fileSize());
	scanned = offset;

	co_return protocols::fs::Error::fileNotFound;
}

async::result<std::expected<bool, protocols::fs::Error>> Inode::isDirectoryEmpty() {
	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timePin = 0;
	frg::scope_exit evtOnExit{[&] {
		auto timeScan = timer.split();
		ostContext.emit(
			ostEvtExt2IsDirectoryEmpty,
			ostAttrTime(timer.elapsed()),
			ostAttrFileSize(fileSize()),
			ostAttrTimeReady(timeReady),
			ostAttrTimePin(timePin),
			ostAttrTimeScan(timeScan)
		);
	}};

	co_await readyEvent.wait();
	timeReady = timer.split();

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
	timePin = timer.split();

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

async::result<frg::expected<protocols::fs::Error>> Inode::updateDotDot(uint32_t parent) {
	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timePin = 0;
	uint64_t cleanDirTime = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2UpdateDotDot,
			ostAttrTime(timer.elapsed()),
			ostAttrFileSize(fileSize()),
			ostAttrTimeReady(timeReady),
			ostAttrTimePin(timePin),
			ostAttrTimeCleanDir(cleanDirTime)
		);
	}};

	co_await readyEvent.wait();
	timeReady = timer.split();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());
	timePin = timer.split();

	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(disk_entry->recordLength);

		if(disk_entry->inode
				&& disk_entry->nameLength == 2
				&& disk_entry->name[0] == '.'
				&& disk_entry->name[1] == '.') {
			disk_entry->inode = parent;

			protocols::ostrace::Timer cleanDirTimer;
			auto syncDir = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
			HEL_CHECK(syncDir.error());
			cleanDirTime += cleanDirTimer.elapsed();

			co_return {};
		}

		offset += disk_entry->recordLength;
	}

	co_return protocols::fs::Error::fileNotFound;
}

async::result<frg::expected<protocols::fs::Error, bool>> Inode::isSubdirectoryOf(uint32_t ino) {
	co_await readyEvent.wait();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;

	// Note that the caller holds BaseFileSystem::topologyMutex exclusively;
	// hence the .. entries that we walk below cannot change.
	auto current = number;
	while(true) {
		if(current == ino)
			co_return true;
		if(current == EXT2_ROOT_INO)
			co_return false;

		auto dir = std::static_pointer_cast<Inode>(fs.accessInode(current));
		auto parent = co_await dir->findEntry("..");
		if(!parent)
			co_return parent.error();
		// A directory whose ".." is missing or points at itself ends the walk.
		if(!parent.value() || parent.value()->inode == current)
			co_return false;
		current = parent.value()->inode;
	}
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

	// Lock the new inode immediately as it is published by insertEntry() below.
	co_await dirNode->inodeMutex.async_lock();
	frg::unique_lock dirNodeLock{frg::adopt_lock, dirNode->inodeMutex};

	{
		co_await dirNode->blockMapMutex.async_lock();
		frg::unique_lock dirNodeBlockMapLock{frg::adopt_lock, dirNode->blockMapMutex};
		co_await fs.assignDataBlocks(dirNode.get(), 0, 1);
	}

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

	dotDotEntry->inode = number;
	dotDotEntry->recordLength = dirNode->fileSize() - offset;
	dotDotEntry->nameLength = 2;
	dotDotEntry->fileType = EXT2_FT_DIR;
	memcpy(dotDotEntry->name, "..", 3);

	updateInodeChecksum(fs, dirNode->diskInode(), dirNode->number);

	// Queue the new directory's inode for writeback to update its linksCount.
	dirNode->diskInodeWindow.markDirty();

	// Hand the modified data blocks over to writeback.
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

	// Lock the new inode immediately as it is published by insertEntry() below.
	co_await newNode->inodeMutex.async_lock();
	frg::unique_lock newNodeLock{frg::adopt_lock, newNode->inodeMutex};

	newNode->setFileSize(target.size());

	if (target.size() <= 60) {
		// fast symlink: store target in the inode itself.
		memcpy(newNode->diskInode()->data.embedded, target.data(), target.size());
	} else {
		// slow symlink: store target in data blocks.
		auto numBlocks = (target.size() + fs.blockSize - 1) / fs.blockSize;
		{
			co_await newNode->blockMapMutex.async_lock();
			frg::unique_lock newNodeBlockMapLock{frg::adopt_lock, newNode->blockMapMutex};
			co_await fs.assignDataBlocks(newNode.get(), 0, numBlocks);
		}

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

	updateInodeChecksum(fs, newNode->diskInode(), newNode->number);

	newNode->diskInodeWindow.markDirty();

	auto result = co_await insertEntry(name, newNode->number, kTypeSymlink);
	if(!result)
		co_return std::unexpected{result.error()};
	co_return result.value();
}

async::result<protocols::fs::Error> Inode::chmod(int mode) {
	co_await readyEvent.wait();

	diskInode()->mode = (diskInode()->mode & 0xFFFFF000) | mode;

	updateInodeChecksum(fs, diskInode(), number);

	diskInodeWindow.markDirty();

	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Inode::chown(std::optional<uid_t> uid, std::optional<gid_t> gid) {
	co_await readyEvent.wait();

	if (uid)
		diskInode()->uid = *uid;
	if (gid)
		diskInode()->gid = *gid;

	updateInodeChecksum(fs, diskInode(), number);

	diskInodeWindow.markDirty();

	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Inode::updateTimes(
		std::optional<timespec> atime,
		std::optional<timespec> mtime,
		std::optional<timespec> ctime) {
	co_await readyEvent.wait();

	if(atime)
		diskInode()->atime = atime->tv_sec;
	if(mtime)
		diskInode()->mtime = mtime->tv_sec;
	if(ctime)
		diskInode()->ctime = ctime->tv_sec;

	updateInodeChecksum(fs, diskInode(), number);

	diskInodeWindow.markDirty();

	co_return protocols::fs::Error::none;
}


async::result<frg::expected<protocols::fs::Error>>
Inode::ensureBackingBlocks(size_t offset, size_t length) {
	auto [alignedOffset, alignedSize] = core::alignExtend({offset, length}, fs.blockSize);
	size_t blockOffset = alignedOffset / fs.blockSize;
	size_t blockCount = alignedSize / fs.blockSize;

	{
		co_await blockMapMutex.async_lock();
		frg::unique_lock blockMapLock{frg::adopt_lock, blockMapMutex};
		co_await fs.assignDataBlocks(this, blockOffset, blockCount);
	}

	co_return frg::success;
}

async::result<frg::expected<protocols::fs::Error>>
Inode::resizeFile(size_t newSize) {
	auto oldSize = fileSize();
	auto newMappingSize = (newSize + 0xFFF) & ~size_t(0xFFF);

	protocols::ostrace::Timer timer;
	uint64_t timeEnsureBlocks = 0;
	uint64_t timeResizeMemory = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtExt2ResizeFile,
			ostAttrTime(timer.elapsed()),
			ostAttrOldSize(oldSize),
			ostAttrNewSize(newSize),
			ostAttrTimeEnsureBlocks(timeEnsureBlocks),
			ostAttrTimeResizeMemory(timeResizeMemory)
		);
	}};

	if (newSize > oldSize) {
		// TODO(qookie): Technically we only need to assign 0
		// blocks here, not allocate new ones. We also should
		// zero out the new blocks.
		FRG_CO_TRY(co_await ensureBackingBlocks(oldSize, newSize - oldSize));
		timeEnsureBlocks = timer.split();

		// Grow fileSize() first so the backing memory never covers a page beyond EOF.
		setFileSize(newSize);

		// Resize the memory object last (afterwards, initialization requests can appear for pages in the new range).
		auto resizeResult = co_await helix_ng::resizeMemory(
				helix::BorrowedDescriptor{backingMemory}, newMappingSize);
		HEL_CHECK(resizeResult.error());
		timeResizeMemory = timer.split();
	} else if (newSize < oldSize) {
		// TODO(qookie): Deallocate blocks if they're no longer within the file.
		std::println("libblockfs: Shrinking an Ext2 file does not free data blocks!");

		// Shrink the memory object first so that no new pages appear beyond the new size.
		auto resizeResult = co_await helix_ng::resizeMemory(
				helix::BorrowedDescriptor{backingMemory}, newMappingSize);
		HEL_CHECK(resizeResult.error());

		// Discard the truncated pages without writeback.
		auto oldMappingSize = (oldSize + 0xFFF) & ~size_t(0xFFF);
		if (oldMappingSize > newMappingSize) {
			auto invalidateResult = co_await helix_ng::invalidateMemory(
					helix::BorrowedDescriptor{backingMemory}, newMappingSize,
					oldMappingSize - newMappingSize, kHelInvalidateNoWriteback);
			HEL_CHECK(invalidateResult.error());
		}

		// Shrink fileSize() last (after no initialization/writeback is in flight anymore).
		setFileSize(newSize);
		timeResizeMemory = timer.split();
	} else {
		// Nothing to do.
		co_return frg::success;
	}

	updateInodeChecksum(fs, diskInode(), number);

	diskInodeWindow.markDirty();

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
	pool = device->pagePool;
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
	protocols::ostrace::Timer timer;

	size_t deviceSuperBlockSector = superBlockOffset / device->sectorSize;
	size_t deviceSuperBlockOffset = superBlockOffset % device->sectorSize;

	size_t deviceSuperBlockSectors = (1024 + deviceSuperBlockOffset + device->sectorSize - 1) / device->sectorSize;

	arch::dma_buffer buffer{pool, deviceSuperBlockSectors * device->sectorSize};
	co_await device->readSectors(deviceSuperBlockSector, buffer);
	uint64_t superblockDone = timer.elapsed();

	DiskSuperblock sb;
	memcpy(&sb, buffer.byte_data() + deviceSuperBlockOffset, sizeof(DiskSuperblock));
	assert(sb.magic == 0xEF53);

	inodeSize = sb.inodeSize;
	blockShift = 10 + sb.logBlockSize;
	blockSize = 1024 << sb.logBlockSize;
	sectorsPerBlock = blockSize / device->sectorSize;
	blocksPerGroup = sb.blocksPerGroup;
	inodesPerGroup = sb.inodesPerGroup;
	blocksCount = sb.blocksCount;
	inodesCount = sb.inodesCount;
	numBlockGroups = (sb.blocksCount + (sb.blocksPerGroup - 1)) / sb.blocksPerGroup;

	memcpy(uuid, sb.uuid, sizeof(sb.uuid));

	if(sb.featureIncompat & EXT4_INCOMPAT_CSUM_SEED)
		metadataChecksumSeed = sb.checksumSeed;
	else {
		checksums::Crc32c crc32{0xffffffff};
		crc32.addData(&sb.uuid, sizeof(sb.uuid));
		metadataChecksumSeed = crc32.finalize();
	}

	is64Bit = sb.featureIncompat & EXT4_INCOMPAT_64BIT;
	usesExtents = sb.featureIncompat & EXT4_INCOMPAT_EXTENTS;
	metadataChecksum = sb.featureRoCompat & EXT4_RO_COMPAT_METADATA_CSUM;
	uint16_t blockGroupDescriptorSize = is64Bit ? sb.groupDescSize : 32;

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

	metadataCache.emplace(device, blocksCount, blockSize);

	blockGroupDescriptorBuffer = arch::dma_buffer{
	    pool,
	    (numBlockGroups * blockGroupDescriptorSize + device->sectorSize - 1)
	        & ~(device->sectorSize - 1)
	};
	bgdt.init(blockGroupDescriptorBuffer.byte_data(), blockGroupDescriptorSize);

	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	protocols::ostrace::Timer bgdtTimer;
	co_await device->readSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer);
	uint64_t bgdtTime = bgdtTimer.elapsed();

	handleBgdtWriteback();

	ostContext.emit(
		ostEvtExt2Mount,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBytes(blockGroupDescriptorBuffer.size()),
		ostAttrNumBlockGroups(numBlockGroups),
		ostAttrTimeSuperblock(superblockDone),
		ostAttrTimeBgdt(bgdtTime)
	);

	co_return;
}

async::detached FileSystem::handleBgdtWriteback() {
	// Snapshot of blockGroupDescriptorBuffer that we write out.
	arch::dma_buffer writebackBuffer{pool, blockGroupDescriptorBuffer.size()};

	uint64_t seenSeq = 0;
	while(true) {
		co_await bgdtWriteback.async_wait(seenSeq);

		protocols::ostrace::Timer timer;
		uint64_t numCoalesced;
		uint64_t lockDone;
		{
			co_await allocationMutex.async_lock();
			frg::unique_lock allocationLock{frg::adopt_lock, allocationMutex};
			lockDone = timer.elapsed();

			// Take the sequence together with the snapshot, so that no request is missed.
			// TODO: Use async::sequenced_event::current_sequence() once it exists.
			numCoalesced = bgdtWriteback.next_sequence() - 1 - seenSeq;
			seenSeq = bgdtWriteback.next_sequence() - 1;
			assert(writebackBuffer.size() == blockGroupDescriptorBuffer.size());
			memcpy(writebackBuffer.data(), blockGroupDescriptorBuffer.data(),
					blockGroupDescriptorBuffer.size());
		}

		// The device write happens outside of allocationMutex.
		auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
		co_await device->writeSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
				writebackBuffer);

		ostContext.emit(
			ostEvtExt2BgdtWriteback,
			ostAttrTime(timer.elapsed()),
			ostAttrNumBytes(writebackBuffer.size()),
			ostAttrNumCoalesced(numCoalesced),
			ostAttrTimeLock(lockDone)
		);
	}
}

auto FileSystem::accessRoot() -> std::shared_ptr<BaseInode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<BaseInode> {
	assert(number > 0);

	std::shared_ptr<Inode> new_inode;
	{
		std::lock_guard activeInodesLock{activeInodesMutex};

		std::weak_ptr<Inode> &inode_slot = activeInodes[number];
		std::shared_ptr<Inode> active_inode = inode_slot.lock();
		if(active_inode)
			return active_inode;

		new_inode = std::make_shared<Inode>(*this, number);
		inode_slot = std::weak_ptr<Inode>(new_inode);
	}

	helix::DispatcherPool::global().detach(initiateInode(new_inode));

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
	// TODO: Reading the BGDT technically has to take allocationMutex.
	//       Avoid this by using atomic loads/stores when manipulating the BGDT.
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

	auto [inodeBlock, inodeOffset] = locateDiskInode(ino);
	auto inodeWindow = co_await metadataCache->access(inodeBlock, true);

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(inodeWindow.get()) + inodeOffset);
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

	if(usesExtents) {
		auto &hdr = disk_inode->data.extents.hdr;
		hdr.magic = EXT4_EXTENT_MAGIC;
		hdr.max = sizeof(disk_inode->data.extents.extents) / sizeof(Extent);
		disk_inode->flags |= EXT4_EXTENTS_FL;
	}

	updateInodeChecksum(*this, disk_inode, ino);

	co_return accessInode(ino);
}

async::result<std::shared_ptr<Inode>> FileSystem::createDirectory() {
	auto ino = co_await allocateInode(0, true);
	assert(ino);

	auto [inodeBlock, inodeOffset] = locateDiskInode(ino);
	auto inodeWindow = co_await metadataCache->access(inodeBlock, true);

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(inodeWindow.get()) + inodeOffset);
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFDIR;
	disk_inode->generation = generation + 1;
	struct timespec time = clk::getRealtime();
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	if(usesExtents) {
		auto &hdr = disk_inode->data.extents.hdr;
		hdr.magic = EXT4_EXTENT_MAGIC;
		hdr.max = sizeof(disk_inode->data.extents.extents) / sizeof(Extent);
		disk_inode->flags |= EXT4_EXTENTS_FL;
	}

	updateInodeChecksum(*this, disk_inode, ino);

	co_return std::static_pointer_cast<Inode>(accessInode(ino));
}

async::result<std::shared_ptr<Inode>> FileSystem::createSymlink() {
	auto ino = co_await allocateInode();
	assert(ino);

	auto [inodeBlock, inodeOffset] = locateDiskInode(ino);
	auto inodeWindow = co_await metadataCache->access(inodeBlock, true);

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(
			reinterpret_cast<std::byte *>(inodeWindow.get()) + inodeOffset);
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFLNK;
	disk_inode->generation = generation + 1;
	struct timespec time = clk::getRealtime();
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	updateInodeChecksum(*this, disk_inode, ino);

	co_return std::static_pointer_cast<Inode>(accessInode(ino));
}

std::pair<uint64_t, size_t> FileSystem::locateDiskInode(uint32_t number) {
	auto index = number - 1;
	auto byteOffset = uint64_t{index % inodesPerGroup} * inodeSize;
	return {bgdt[index / inodesPerGroup].inodeTable + (byteOffset >> blockShift),
			static_cast<size_t>(byteOffset & (blockSize - 1))};
}

async::result<void> FileSystem::initiateInode(std::shared_ptr<Inode> inode) {
	protocols::ostrace::Timer timer;

	auto [inodeBlock, inodeOffset] = locateDiskInode(inode->number);
	inode->diskInodeWindow = co_await metadataCache->access(inodeBlock, true);
	inode->diskInodeOffset = inodeOffset;
	uint64_t accessDone = timer.elapsed();

	auto disk_inode = inode->diskInode();
	// printf("Inode %lu: file size: %u\n", inode->number, disk_inode->size);

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

	// Allocate a page cache for the file.
	auto cache_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(cache_size, kHelManagedReadahead,
			&inode->backingMemory, &inode->frontalMemory));

	if (inode->fileType == kTypeDirectory) {
		auto mapSize = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
		inode->fileMapping = helix::Mapping{helix::BorrowedDescriptor{inode->frontalMemory},
				0, mapSize,
				kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
	}

	if(disk_inode->flags & EXT4_EXTENTS_FL)
		inode->usesExtents = true;

	// Keep the servicer on the pool member that this inode was initiated on.
	async::detach_on(helix::Dispatcher::global().runQueue(), manageFileData(inode));

	ostContext.emit(
		ostEvtExt2InitiateInode,
		ostAttrTime(timer.elapsed()),
		ostAttrIno(inode->number),
		ostAttrTimeAccess(accessDone)
	);

	inode->readyEvent.raise();
}

async::result<void> FileSystem::manageFileData(std::shared_ptr<Inode> inode) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor(inode->backingMemory),
				&manage, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());

		// This is guaranteed by our resizing logic (since shrinking of fileSize() happens after backing memory resize).
		assert(manage.offset() + manage.length() <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF)));

		protocols::ostrace::Timer timer;
		auto fileView = pool->importMemory(
		    helix::BorrowedDescriptor{inode->backingMemory}, manage.offset(), manage.length()
		);
		uint64_t importDone = timer.elapsed();

		if(manage.type() == kHelManageInitialize) {
			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backed_size = std::min(manage.length(), inode->fileSize() - manage.offset());
			size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;
			assert(num_blocks * inode->fs.blockSize <= manage.length());

			uint64_t lockDone;
			uint64_t readDone;
			{
				co_await inode->blockMapMutex.async_lock();
				frg::unique_lock blockMapLock{frg::adopt_lock, inode->blockMapMutex};
				lockDone = timer.elapsed();
				co_await inode->fs.readDataBlocks(inode, manage.offset() / inode->fs.blockSize, fileView);
				readDone = timer.elapsed();
			}

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageInitialize,
					manage.offset(), manage.length()));

			ostContext.emit(
				ostEvtExt2InitializeFile,
				ostAttrTime(timer.elapsed()),
				ostAttrNumBytes(manage.length()),
				ostAttrTimeImport(importDone),
				ostAttrTimeLock(lockDone - importDone),
				ostAttrTimeRead(readDone - lockDone)
			);
		}else{
			assert(manage.type() == kHelManageWriteback);

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backedSize = std::min(manage.length(), inode->fileSize() - manage.offset());
			auto blockOffset = manage.offset() / inode->fs.blockSize;
			size_t numBlocks = (backedSize + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(numBlocks * inode->fs.blockSize <= manage.length());

			uint64_t lockDone;
			uint64_t assignDone;
			uint64_t writeDone;
			{
				co_await inode->blockMapMutex.async_lock();
				frg::unique_lock blockMapLock{frg::adopt_lock, inode->blockMapMutex};
				lockDone = timer.elapsed();
				co_await inode->fs.assignDataBlocks(inode.get(), blockOffset, numBlocks);
				assignDone = timer.elapsed();
				co_await inode->fs.writeDataBlocks(inode, blockOffset, fileView);
				writeDone = timer.elapsed();
			}

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageWriteback,
					manage.offset(), manage.length()));

			ostContext.emit(
				ostEvtExt2WritebackFile,
				ostAttrTime(timer.elapsed()),
				ostAttrNumBytes(manage.length()),
				ostAttrTimeImport(importDone),
				ostAttrTimeLock(lockDone - importDone),
				ostAttrTimeAssign(assignDone - lockDone),
				ostAttrTimeWrite(writeDone - assignDone)
			);
		}
	}
}

async::result<std::vector<uint32_t>> FileSystem::allocateBlocks(size_t num, std::optional<uint32_t> ino) {
	protocols::ostrace::Timer timer;
	std::vector<uint32_t> result;
	uint64_t accessTime = 0;
	unsigned int numGroups = 0;

	co_await allocationMutex.async_lock();
	frg::unique_lock allocationLock{frg::adopt_lock, allocationMutex};
	uint64_t lockDone = timer.elapsed();

	// Accesses the bitmap of a block group, accounting the access to the emitted event.
	auto accessBitmap = [&] (uint64_t block) -> async::result<MetadataCache::BlockWindow> {
		protocols::ostrace::Timer accessTimer;
		auto window = co_await metadataCache->access(block, true);
		accessTime += accessTimer.elapsed();
		++numGroups;
		co_return window;
	};

	if (ino) {
		uint32_t preferred_bg = (*ino - 1) / inodesPerGroup;

		if(bgdt[preferred_bg].freeBlocksCount) {
			assert(bgdt[preferred_bg].blockBitmap);
			auto bitmapWindow = co_await accessBitmap(bgdt[preferred_bg].blockBitmap);
			auto words = reinterpret_cast<uint32_t *>(bitmapWindow.get());

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
						updateBlockBitmapChecksum(*this, &bgdt[preferred_bg], words, blockSize);
						updateBlockGroupChecksum(*this, &bgdt[preferred_bg], preferred_bg);

						ostContext.emit(
							ostEvtExt2AllocateBlocks,
							ostAttrTime(timer.elapsed()),
							ostAttrNumBlocks(result.size()),
							ostAttrNumGroups(numGroups),
							ostAttrTimeLock(lockDone),
							ostAttrTimeAccess(accessTime)
						);
						co_return result;
					}
				}
			}

			if(!result.empty()) {
				updateBlockBitmapChecksum(*this, &bgdt[preferred_bg], words, blockSize);
				updateBlockGroupChecksum(*this, &bgdt[preferred_bg], preferred_bg);
			}
		}
	}

	for(uint32_t bg_idx = 0; bg_idx < numBlockGroups; bg_idx++) {
		if(!bgdt[bg_idx].freeBlocksCount)
			continue;

		assert(bgdt[bg_idx].blockBitmap);
		auto bitmapWindow = co_await accessBitmap(bgdt[bg_idx].blockBitmap);
		auto words = reinterpret_cast<uint32_t *>(bitmapWindow.get());
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
					updateBlockBitmapChecksum(*this, &bgdt[bg_idx], words, blockSize);
					updateBlockGroupChecksum(*this, &bgdt[bg_idx], bg_idx);

					ostContext.emit(
						ostEvtExt2AllocateBlocks,
						ostAttrTime(timer.elapsed()),
						ostAttrNumBlocks(result.size()),
						ostAttrNumGroups(numGroups),
						ostAttrTimeLock(lockDone),
						ostAttrTimeAccess(accessTime)
					);
					co_return result;
				}
			}
		}

		updateBlockBitmapChecksum(*this, &bgdt[bg_idx], words, blockSize);
		updateBlockGroupChecksum(*this, &bgdt[bg_idx], bg_idx);
	}

	assert(!"Failed to find zero-bit");
}

async::result<uint32_t> FileSystem::allocateInode(uint32_t parentIno, bool directory) {
	protocols::ostrace::Timer timer;
	uint64_t accessTime = 0;
	unsigned int numGroups = 0;

	co_await allocationMutex.async_lock();
	frg::unique_lock allocationLock{frg::adopt_lock, allocationMutex};
	uint64_t lockDone = timer.elapsed();

	auto searchBlockGroup = [&](uint32_t bg) -> async::result<std::optional<uint32_t>> {
		assert(bgdt[bg].inodeBitmap);
		protocols::ostrace::Timer accessTimer;
		auto bitmapWindow = co_await metadataCache->access(bgdt[bg].inodeBitmap, true);
		accessTime += accessTimer.elapsed();
		++numGroups;
		auto words = reinterpret_cast<uint32_t *>(bitmapWindow.get());
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

				updateInodeBitmapChecksum(*this, &bgdt[bg], words, blockSize);
				updateBlockGroupChecksum(*this, &bgdt[bg], bg);

				bgdtWriteback.raise();

				ostContext.emit(
					ostEvtExt2AllocateInode,
					ostAttrTime(timer.elapsed()),
					ostAttrIno(ino),
					ostAttrNumGroups(numGroups),
					ostAttrTimeLock(lockDone),
					ostAttrTimeAccess(accessTime)
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
		ostAttrTime(timer.elapsed()),
		ostAttrIno(0),
		ostAttrNumGroups(numGroups),
		ostAttrTimeLock(lockDone),
		ostAttrTimeAccess(accessTime)
	);

	co_return 0;
}

async::result<std::vector<ExtentBlockRange>> FileSystem::lookupBlocksUsingExtent(Inode *inode,
		uint64_t block_offset, size_t num_blocks, bool errorIfNotFound) {
	std::vector<ExtentBlockRange> ranges;

	ExtentWalker walker{this, inode, true};

	size_t progress = 0;
	while(progress < num_blocks) {
		auto index = block_offset + progress;

		bool res = co_await walker.walk(index,
			[](ExtentWalkInfo &) -> async::result<void> { co_return; },
			async::lambda([&](ExtentWalkInfo &info) -> async::result<ExtentIterDecision> {
			auto &extent = info.extents[info.index];

			assert(index >= extent.block);
			if(!(index < extent.block + extent.len)) {
				std::println(std::cout, "INDEX {}, EXTENT BLOCK {}, EXTENT LEN {}", index, extent.block, extent.len);
			}
			assert(index < extent.block + extent.len);

			size_t startOffset = index - extent.block;
			size_t available = extent.len - startOffset;
			size_t toAdd = std::min(available, num_blocks - progress);

			uint64_t absoluteStartBlock = static_cast<uint64_t>(extent.startLow)
					| (static_cast<uint64_t>(extent.startHigh) << 32);

			ExtentBlockRange range{
				.relativeStartBlock = index,
				.absoluteStartBlock = absoluteStartBlock + startOffset,
				.size = toAdd,
				.found = true
			};
			ranges.push_back(range);

			progress += toAdd;
			co_return ExtentIterDecision::stop;
		}));

		if(!res) {
			if(errorIfNotFound)
				assert(!"Block was not found in extent tree");

			if(!ranges.empty() && ranges.back().relativeStartBlock + ranges.back().size == index
					&& !ranges.back().found) {
				ranges.back().size++;
			} else {
				ExtentBlockRange range{
					.relativeStartBlock = index,
					.absoluteStartBlock = 0,
					.size = 1,
					.found = false
				};
				ranges.push_back(range);
			}

			progress++;
		}
	}

	co_return ranges;
}

async::result<void> FileSystem::assignDataBlocksUsingExtents(Inode *inode,
		uint64_t block_offset, size_t num_blocks) {
	protocols::ostrace::Timer timer;

	auto diskInode = inode->diskInode();
	auto blockRanges = co_await lookupBlocksUsingExtent(inode, block_offset, num_blocks, false);

	bool anyAllocated = false;

	for(auto &range : blockRanges) {
		if(range.found)
			continue;

		auto allocated = co_await allocateBlocks(range.size, inode->number);
		assert(!allocated.empty() && "Out of disk space");
		anyAllocated = true;

		// Merge the allocated blocks to a vector of
		// [begin, end] pairs.
		std::vector<std::pair<uint64_t, uint64_t>> allocatedRanges;
		for(auto block : allocated) {
			bool found = false;
			for(auto &existingRange : allocatedRanges) {
				if(existingRange.first == block + 1) {
					existingRange.first--;
					found = true;
					break;
				} else if(existingRange.second == block) {
					existingRange.second++;
					found = true;
					break;
				}
			}

			if(!found) {
				allocatedRanges.push_back({block, block + 1});
			}
		}

		size_t progress = 0;
		for(auto &allocatedRange : allocatedRanges) {
			size_t allocatedRangeSize = allocatedRange.second - allocatedRange.first;
			size_t index = range.relativeStartBlock + progress;

			struct UpdateMinBlock {};

			assert(allocatedRangeSize);
			std::variant<std::monostate, Extent, ExtentIndex, UpdateMinBlock> writeExtent = Extent{
				.block = static_cast<uint32_t>(index),
				.len = static_cast<uint16_t>(allocatedRangeSize),
				.startHigh = static_cast<uint16_t>((allocatedRange.first >> 32) & 0xffff),
				.startLow = static_cast<uint32_t>(allocatedRange.first & 0xffffffff)
			};

			ExtentWalker walker{this, inode, false};
			co_await walker.walk(index,
				[](const ExtentWalkInfo &) -> async::result<void> { co_return; },
				async::lambda([&](ExtentWalkInfo &info) -> async::result<ExtentIterDecision> {
					MetadataCache::BlockWindow newBlockWindow;
					MetadataCache::BlockWindow newRootWindow;

					if(std::holds_alternative<UpdateMinBlock>(writeExtent)) {
						assert(info.indices);
						auto &idx = info.indices[info.index];

						if(index < idx.block) {
							idx.block = index;

							if(info.block)
								updateExtentChecksum(*this, inode, info.hdr);

							co_return ExtentIterDecision::keepGoing;
						}else {
							co_return ExtentIterDecision::stop;
						}
					}

					// Adjust the index for insertion, the extent walker returns the lookup index.
					if(info.hdr->entries)
						info.index++;

					std::variant<std::monostate, Extent, ExtentIndex, UpdateMinBlock> nextWriteExtent;

					if(info.hdr->entries + 1 > info.hdr->max) {
						auto newBlock = co_await allocateBlocks(1, inode->number);
						assert(!newBlock.empty() && "Out of disk space");

						diskInode->blocks += blockSize / 512;

						newBlockWindow = co_await metadataCache->access(newBlock[0], true);
						auto newHdr = reinterpret_cast<ExtentHeader *>(newBlockWindow.get());

						newHdr->magic = EXT4_EXTENT_MAGIC;
						newHdr->max = (blockSize - sizeof(ExtentHeader)) / sizeof(Extent);
						newHdr->depth = info.hdr->depth;
						newHdr->generation = 0;

						uint16_t splitStart = info.index;

						uint16_t entriesBeforeSplit = info.hdr->entries;
						uint16_t entriesToMove = info.hdr->entries - splitStart;
						info.hdr->entries -= entriesToMove;
						newHdr->entries = entriesToMove;

						uint32_t oldFirstBlock;
						uint32_t newFirstBlock;

						if(info.extents) {
							oldFirstBlock = info.extents[0].block;

							if(splitStart == entriesBeforeSplit)
								newFirstBlock = index;
							else
								newFirstBlock = info.extents[splitStart].block;

							auto newExtents = reinterpret_cast<Extent *>(&newHdr[1]);
							memmove(newExtents, info.extents + splitStart, entriesToMove * sizeof(Extent));
						} else {
							oldFirstBlock = info.indices[0].block;

							if(splitStart == entriesBeforeSplit)
								newFirstBlock = index;
							else
								newFirstBlock = info.indices[splitStart].block;

							auto newIndices = reinterpret_cast<ExtentIndex *>(&newHdr[1]);
							memmove(newIndices, info.indices + splitStart, entriesToMove * sizeof(ExtentIndex));
						}

						assert(newBlock[0] != 0);
						updateExtentChecksum(*this, inode, newHdr);

						// The block lost entriesToMove-many entries, so update its checksum.
						if(info.block)
							updateExtentChecksum(*this, inode, info.hdr);

						if(!info.block) {
							// The root is full, allocate a new level for the entries that would have been left at root.
							auto newRoot = co_await allocateBlocks(1, inode->number);
							assert(!newRoot.empty() && "Out of disk space");

							diskInode->blocks += blockSize / 512;

							newRootWindow = co_await metadataCache->access(newRoot[0], true);
							auto newRootHdr = reinterpret_cast<ExtentHeader *>(newRootWindow.get());

							memcpy(newRootHdr, info.hdr, sizeof(ExtentHeader) + info.hdr->entries * sizeof(Extent));
							// Update the max count as the inode can store less entries than a block.
							newRootHdr->max = (blockSize - sizeof(ExtentHeader)) / sizeof(Extent);

							assert(newRoot[0] != 0);
							updateExtentChecksum(*this, inode, newRootHdr);

							info.hdr->entries = 2;
							info.hdr->depth++;
							assert(info.hdr->depth <= 4);

							auto indices = reinterpret_cast<ExtentIndex *>(&info.hdr[1]);
							indices[0] = {
								.block = oldFirstBlock,
								.leafLow = static_cast<uint32_t>(newRoot[0] & 0xffffffff),
								// TODO: Support larger blocks than 32-bit.
								.leafHigh = 0
								//.leafHigh = static_cast<uint16_t>(newRoot[0] >> 32)
							};
							indices[1] = {
								.block = newFirstBlock,
								.leafLow = static_cast<uint32_t>(newBlock[0] & 0xffffffff),
								// TODO: Support larger blocks than 32-bit.
								.leafHigh = 0
								//.leafHigh = static_cast<uint16_t>(newBlock[0] >> 32)
							};

							if(index >= newFirstBlock) {
								info.hdr = newHdr;
								info.block = newBlock[0];
								info.index -= splitStart;
							}else {
								info.hdr = newRootHdr;
								info.block = newRoot[0];
							}
						}else {
							if(index >= newFirstBlock) {
								info.hdr = newHdr;
								info.block = newBlock[0];
								info.index -= splitStart;
							}

							// The new block needs to be propagated upwards (where it is always an ExtentIndex).
							nextWriteExtent = ExtentIndex{
								.block = newFirstBlock,
								.leafLow = static_cast<uint32_t>(newBlock[0] & 0xffffffff),
								// TODO: Support larger blocks than 32-bit.
								.leafHigh = 0
								//.leafHigh = static_cast<uint16_t>(newBlock[0] >> 32)
							};
						}

						if(info.hdr->depth == 0) {
							info.extents = reinterpret_cast<Extent *>(&info.hdr[1]);
							info.indices = nullptr;
						}else {
							info.indices = reinterpret_cast<ExtentIndex *>(&info.hdr[1]);
							info.extents = nullptr;
						}
					}

					assert(info.hdr->entries < info.hdr->max);
					uint16_t entriesToMove = info.hdr->entries - info.index;

					if(auto newExtent = std::get_if<Extent>(&writeExtent)) {
						assert(info.extents);
						memmove(info.extents + info.index + 1, info.extents + info.index, entriesToMove * sizeof(Extent));

						info.extents[info.index] = *newExtent;
						info.hdr->entries++;
					}else {
						assert(info.indices);
						memmove(info.indices + info.index + 1, info.indices + info.index, entriesToMove * sizeof(ExtentIndex));

						info.indices[info.index] = std::get<ExtentIndex>(writeExtent);
						info.hdr->entries++;
					}

					if(info.block) {
						assert(*info.block != 0);
						updateExtentChecksum(*this, inode, info.hdr);
					}

					writeExtent = nextWriteExtent;

					if(std::holds_alternative<std::monostate>(writeExtent)) {
						writeExtent = UpdateMinBlock{};
					}

					co_return ExtentIterDecision::keepGoing;
				}));

			progress += allocatedRangeSize;
		}

		assert(progress == range.size);

		diskInode->blocks += allocated.size() * (blockSize / 512);
	}

	if(anyAllocated) {
		updateInodeChecksum(*this, diskInode, inode->number);

		bgdtWriteback.raise();
		inode->diskInodeWindow.markDirty();
	}

	ostContext.emit(
		ostEvtExt2AssignDataBlocks,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBlocks(num_blocks)
	);
}

async::result<void> FileSystem::readDataBlocksUsingExtents(std::shared_ptr<Inode> inode, uint64_t block_offset,
		arch::dma_buffer_view buf) {
	co_await inode->readyEvent.wait();
	// TODO: Assert that we do not read past the EOF.

	protocols::ostrace::Timer timer;
	uint64_t deviceTime = 0;

	size_t num_blocks = buf.size() >> blockShift;
	auto blockRanges = co_await lookupBlocksUsingExtent(inode.get(), block_offset, num_blocks, false);

	size_t progress = 0;
	for(auto &range : blockRanges) {
		assert(range.relativeStartBlock == block_offset + progress);

		if(!range.found) {
			memset(buf.byte_data() + progress * blockSize, 0, range.size * blockSize);
		}else {
			assert(range.absoluteStartBlock);
			protocols::ostrace::Timer deviceTimer;
			co_await device->readSectors(
			    range.absoluteStartBlock * sectorsPerBlock,
			    buf.subview(progress * blockSize, range.size * blockSize)
			);
			deviceTime += deviceTimer.elapsed();
		}

		progress += range.size;
	}

	assert(progress == num_blocks);

	ostContext.emit(
		ostEvtExt2ReadDataBlocks,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBytes(buf.size()),
		ostAttrTimeDevice(deviceTime)
	);
}

async::result<void> FileSystem::writeDataBlocksUsingExtents(std::shared_ptr<Inode> inode, uint64_t block_offset,
		arch::dma_buffer_view buf) {
	co_await inode->readyEvent.wait();
	// TODO: Assert that we do not read past the EOF.

	protocols::ostrace::Timer timer;
	uint64_t deviceTime = 0;

	size_t num_blocks = buf.size() >> blockShift;
	auto blockRanges = co_await lookupBlocksUsingExtent(inode.get(), block_offset, num_blocks, true);

	size_t progress = 0;
	for(auto &range : blockRanges) {
		protocols::ostrace::Timer deviceTimer;
		co_await device->writeSectors(
		    range.absoluteStartBlock * sectorsPerBlock,
		    buf.subview(progress * blockSize, range.size * blockSize)
		);
		deviceTime += deviceTimer.elapsed();
		progress += range.size;
	}

	assert(progress == num_blocks);

	ostContext.emit(
		ostEvtExt2WriteDataBlocks,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBytes(buf.size()),
		ostAttrTimeDevice(deviceTime)
	);
}

async::result<void> FileSystem::assignDataBlocks(Inode *inode,
		uint64_t block_offset, size_t num_blocks) {
	if(inode->usesExtents) {
		co_await assignDataBlocksUsingExtents(inode, block_offset, num_blocks);
		co_await helix_ng::asyncNop();
		co_return;
	}

	protocols::ostrace::Timer timer;

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	auto disk_inode = inode->diskInode();

	bool anyAllocated = false;

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
				anyAllocated = true;
				prg += allocated.size();
			}
		}else if(block_offset + prg < s_range) {
			bool needsReset = false;

			// Allocate the single-indirect block itself.
			if(!disk_inode->data.blocks.singleIndirect) {
				auto block = co_await allocateBlocks(1, inode->number);
				assert(!block.empty() && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				anyAllocated = true;
				disk_inode->data.blocks.singleIndirect = block[0];
				needsReset = true;
			}

			auto indirectWindow = co_await metadataCache->access(
					disk_inode->data.blocks.singleIndirect, true);
			auto window = reinterpret_cast<uint32_t *>(indirectWindow.get());

			if(needsReset)
				memset(window, 0, blockSize);

			while(prg < num_blocks
					&& block_offset + prg < s_range) {
				auto idx = block_offset + prg - i_range;

				size_t range = 0;
				for(size_t i = idx; i < per_indirect; i++) {
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
				anyAllocated = true;
				prg += allocated.size();
			}
		}else if(block_offset + prg < d_range) {
			bool doubleNeedsReset = false;
			if(!disk_inode->data.blocks.doubleIndirect) {
				auto block = co_await allocateBlocks(1, inode->number);
				assert(!block.empty() && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				anyAllocated = true;
				disk_inode->data.blocks.doubleIndirect = block[0];
				doubleNeedsReset = true;
			}

			auto doubleIndirectWindow = co_await metadataCache->access(
					disk_inode->data.blocks.doubleIndirect, true);
			auto double_window = reinterpret_cast<uint32_t *>(doubleIndirectWindow.get());

			if(doubleNeedsReset)
				memset(double_window, 0, blockSize);

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
					anyAllocated = true;
					double_window[indirect_frame] = block[0];
					needsReset = true;
				}

				auto indirectWindow = co_await metadataCache->access(
						double_window[indirect_frame], true);
				auto window = reinterpret_cast<uint32_t *>(indirectWindow.get());

				if(needsReset)
					memset(window, 0, blockSize);

				size_t range = 0;
				for(size_t i = indirect_index; i < per_indirect; i++) {
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
				anyAllocated = true;
				prg += allocated.size();
			}
		}else{
			assert(!"TODO: Implement allocation in triple indirect blocks");
		}
	}

	if(anyAllocated) {
		updateInodeChecksum(*this, inode->diskInode(), inode->number);

		bgdtWriteback.raise();
		inode->diskInodeWindow.markDirty();
	}

	ostContext.emit(
		ostEvtExt2AssignDataBlocks,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBlocks(num_blocks)
	);
}

async::result<void> FileSystem::readDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, arch::dma_buffer_view buf) {
	size_t num_blocks = buf.size() >> blockShift;

	if(inode->usesExtents) {
		co_await readDataBlocksUsingExtents(std::move(inode), offset, buf);
		co_await helix_ng::asyncNop();
		co_return;
	}

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

	protocols::ostrace::Timer timer;
	uint64_t deviceTime = 0;

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

			auto disk_inode = inode->diskInode();
			uint32_t indirect_block = 0;
			if(disk_inode->data.blocks.doubleIndirect)
				co_await metadataCache->read(disk_inode->data.blocks.doubleIndirect,
						indirect_frame * 4, 4, &indirect_block);

			if(!indirect_block) {
				issue = {0, std::min<size_t>(remaining, per_indirect - indirect_index)};
			} else if (remaining > indirectBufferSize) {
				auto indirectWindow = co_await metadataCache->access(indirect_block, false);

				issue = fuse(remaining,
						reinterpret_cast<uint32_t *>(indirectWindow.get()) + indirect_index,
						per_indirect - indirect_index);
			} else {
				auto chunk = std::min<size_t>(remaining, per_indirect - indirect_index);
				co_await metadataCache->read(indirect_block,
						indirect_index * 4, chunk * 4, indirectBuffer.data());

				issue = fuse(chunk, indirectBuffer.data(), chunk);
			}
		}else if(index >= i_range) { // Use the single indirect block.
			auto remaining = num_blocks - progress;
			auto indirect_index = index - i_range;

			auto disk_inode = inode->diskInode();
			auto indirect_block = disk_inode->data.blocks.singleIndirect;

			if(!indirect_block) {
				issue = {0, std::min<size_t>(remaining, per_single - indirect_index)};
			} else if (remaining > indirectBufferSize) {
				auto indirectWindow = co_await metadataCache->access(indirect_block, false);

				issue = fuse(remaining,
						reinterpret_cast<uint32_t *>(indirectWindow.get()) + indirect_index,
						per_indirect - indirect_index);
			} else {
				auto chunk = std::min<size_t>(remaining, per_single - indirect_index);
				co_await metadataCache->read(indirect_block,
						indirect_index * 4, chunk * 4, indirectBuffer.data());

				issue = fuse(chunk, indirectBuffer.data(), chunk);
			}
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(num_blocks - progress,
					disk_inode->data.blocks.direct + index, 12 - index);
		}

//		std::cout << "Issuing read of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		if (issue.first) {
			protocols::ostrace::Timer deviceTimer;
			co_await device->readSectors(issue.first * sectorsPerBlock, buf.subview(progress * blockSize, issue.second * blockSize));
			deviceTime += deviceTimer.elapsed();
		} else {
			memset(buf.byte_data() + progress * blockSize, 0, issue.second * blockSize);
		}
		progress += issue.second;
	}

	ostContext.emit(
		ostEvtExt2ReadDataBlocks,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBytes(buf.size()),
		ostAttrTimeDevice(deviceTime)
	);
}

// TODO: There is a lot of overlap between this method and readDataBlocks.
//       Refactor common code into a another method.
async::result<void> FileSystem::writeDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, arch::dma_buffer_view buf) {
	size_t num_blocks = buf.size() >> blockShift;

	if(inode->usesExtents) {
		co_await writeDataBlocksUsingExtents(std::move(inode), offset, buf);
		co_await helix_ng::asyncNop();
		co_return;
	}

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

	protocols::ostrace::Timer timer;
	uint64_t deviceTime = 0;

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

			auto disk_inode = inode->diskInode();
			assert(disk_inode->data.blocks.doubleIndirect);
			uint32_t indirect_block;
			co_await metadataCache->read(disk_inode->data.blocks.doubleIndirect,
					indirect_frame * 4, 4, &indirect_block);
			assert(indirect_block);

			auto indirectWindow = co_await metadataCache->access(indirect_block, false);

			issue = fuse(indirect_index, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirectWindow.get()), per_indirect);
		}else if(index >= i_range) { // Use the single indirect block.
			auto disk_inode = inode->diskInode();
			assert(disk_inode->data.blocks.singleIndirect);

			auto indirectWindow = co_await metadataCache->access(
					disk_inode->data.blocks.singleIndirect, false);

			issue = fuse(index - i_range, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirectWindow.get()), per_indirect);
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(index, num_blocks - progress,
					disk_inode->data.blocks.direct, 12);
		}

//		std::cout << "Issuing write of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first);
		protocols::ostrace::Timer deviceTimer;
		co_await device->writeSectors(
		    issue.first * sectorsPerBlock,
		    buf.subview(progress * blockSize, issue.second * blockSize)
		);
		deviceTime += deviceTimer.elapsed();
		progress += issue.second;
	}

	ostContext.emit(
		ostEvtExt2WriteDataBlocks,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBytes(buf.size()),
		ostAttrTimeDevice(deviceTime)
	);
}

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
OpenFile::readEntries() {
	auto inode = std::static_pointer_cast<Inode>(this->inode);

	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timePin = 0;
	uint64_t timeMap = 0;
	frg::scope_exit evtOnExit{[&] {
		auto timeScan = timer.split();
		ostContext.emit(
			ostEvtReadDir,
			ostAttrTime(timer.elapsed()),
			ostAttrFileSize(inode->fileSize()),
			ostAttrTimeReady(timeReady),
			ostAttrTimePin(timePin),
			ostAttrTimeMap(timeMap),
			ostAttrTimeScan(timeScan)
		);
	}};

	co_await inode->readyEvent.wait();
	timeReady = timer.split();

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
	timePin = timer.split();

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{inode->frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};
	timeMap = timer.split();

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

