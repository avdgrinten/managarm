
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
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include <array>
#include <bit>

#include "ext2fs.hpp"
#include "extents.hpp"
#include "../checksums.hpp"

namespace blockfs {
namespace ext2fs {

namespace {
	constexpr bool logSuperblock = true;

	constexpr int pageShift = 12;
	constexpr size_t pageSize = size_t{1} << pageShift;

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

	assert(fileMapping.size() == ((fileSize() + 0xFFF) & ~size_t(0xFFF)));

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

		// Flush the data to disk.
		// TODO: It would be enough to flush only one or two pages here.
		auto syncDir = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
		HEL_CHECK(syncDir.error());

		// Increment the target's link count.
		// This is sound since the caller holds the target's inodeMutex exclusively.
		auto target = std::static_pointer_cast<Inode>(fs.accessInode(ino));
		co_await target->readyEvent.wait();
		target->diskInode()->linksCount++;

		updateInodeChecksum(fs, target->diskInode(), ino);

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

	// A new subdirectory adds a ".." backlink to this directory.
	if(type == kTypeDirectory)
		diskInode()->linksCount++;

	updateInodeChecksum(fs, diskInode(), number);

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

			auto targetIno = disk_entry->inode;
			if(offset & (fs.blockSize - 1)) {
				previous_entry->recordLength += disk_entry->recordLength;
			} else {
				// The directory entry is at the start of a block. We mark it as unused instead of merging it.
				disk_entry->inode = 0;
			}

			// Flush the data to disk.
			// TODO: It would be enough to flush only one or two pages here.
			auto syncDir = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
			HEL_CHECK(syncDir.error());

			// Decrement the inode's link count
			// This is sound since the caller holds the target's inodeMutex exclusively.
			if(--target->diskInode()->linksCount == 0) {
				// TODO: free the data blocks and set size to 0
				target->diskInode()->dtime = clk::getRealtime().tv_sec;
			}

			updateInodeChecksum(fs, target->diskInode(), targetIno);

			auto syncInode = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle},
					target->diskInode(), fs.inodeSize);
			HEL_CHECK(syncInode.error());

			// A removed subdirectory drops its ".." backlink to this directory.
			if(target->fileType == kTypeDirectory) {
				diskInode()->linksCount--;

				updateInodeChecksum(fs, diskInode(), number);

				auto syncParent = co_await helix_ng::synchronizeSpace(
						helix::BorrowedDescriptor{kHelNullHandle},
						diskInode(), fs.inodeSize);
				HEL_CHECK(syncParent.error());
			}

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

async::result<frg::expected<protocols::fs::Error>> Inode::updateDotDot(uint32_t parent) {
	co_await readyEvent.wait();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

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

			auto syncDir = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
			HEL_CHECK(syncDir.error());

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

	updateInodeChecksum(fs, diskInode(), number);

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

	updateInodeChecksum(fs, diskInode(), number);

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
	co_await readyEvent.wait();

	if(atime)
		diskInode()->atime = atime->tv_sec;
	if(mtime)
		diskInode()->mtime = mtime->tv_sec;
	if(ctime)
		diskInode()->ctime = ctime->tv_sec;

	updateInodeChecksum(fs, diskInode(), number);

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

	updateInodeChecksum(fs, diskInode(), number);

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
	size_t deviceSuperBlockSector = superBlockOffset / device->sectorSize;
	size_t deviceSuperBlockOffset = superBlockOffset % device->sectorSize;

	size_t deviceSuperBlockSectors = (1024 + deviceSuperBlockOffset + device->sectorSize - 1) / device->sectorSize;

	arch::dma_buffer buffer{pool, deviceSuperBlockSectors * device->sectorSize};
	co_await device->readSectors(deviceSuperBlockSector, buffer);

	DiskSuperblock sb;
	memcpy(&sb, buffer.byte_data() + deviceSuperBlockOffset, sizeof(DiskSuperblock));
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
	bgdtWritebackBuffer = arch::dma_buffer{pool, blockGroupDescriptorBuffer.size()};

	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	co_await device->readSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer);

	handleBgdtWriteback();

	// Create memory bundles to manage the block and inode bitmaps.
	HelHandle block_bitmap_frontal, inode_bitmap_frontal;
	HelHandle block_bitmap_backing, inode_bitmap_backing;
	HEL_CHECK(helCreateManagedMemory(numBlockGroups << blockPagesShift,
			0, &block_bitmap_backing, &block_bitmap_frontal));
	HEL_CHECK(helCreateManagedMemory(numBlockGroups << blockPagesShift,
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
	HEL_CHECK(helCreateManagedMemory(inodesPerGroup * inodeSize * numBlockGroups,
			0, &inode_table_backing, &inode_table_frontal));
	inodeTable = helix::UniqueDescriptor{inode_table_frontal};
	inodeTableMapping = helix::Mapping{inodeTable,
			0, inodesPerGroup * inodeSize * numBlockGroups,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	manageInodeTable(helix::UniqueDescriptor{inode_table_backing});

	co_return;
}

async::detached FileSystem::handleBgdtWriteback() {
	uint64_t writtenSequence = 0;
	while(true) {
		while(co_await bdgtWriteback.async_wait_if([&] {
			return bgdtDirtySequence.load(std::memory_order_relaxed) == writtenSequence;
		}))
			;

		// Snapshot the table so that the device write does not stall allocations.
		{
			co_await allocationMutex.async_lock();
			frg::unique_lock allocationLock{frg::adopt_lock, allocationMutex};
			memcpy(bgdtWritebackBuffer.data(), blockGroupDescriptorBuffer.data(),
					blockGroupDescriptorBuffer.size());
			writtenSequence = bgdtDirtySequence.load(std::memory_order_relaxed);
		}

		auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
		co_await device->writeSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
				bgdtWritebackBuffer);
	}
}

async::detached FileSystem::manageBlockBitmap(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		serviceBlockBitmapRequest(memory, manage.type(), manage.offset(), manage.length());
	}
}

async::detached FileSystem::serviceBlockBitmapRequest(helix::BorrowedDescriptor memory,
		int type, uintptr_t offset, size_t length) {
	protocols::ostrace::Timer timer;

	assert(!(offset & ((1 << blockPagesShift) - 1))
			&& "TODO: properly support multi-page blocks");

	auto view = pool->importMemory(memory, offset, length);

	for(size_t progress = 0; progress < length; progress += (1 << blockPagesShift)) {
		auto bg_idx = (offset + progress) >> blockPagesShift;
		auto block = bgdt[bg_idx].blockBitmap;
		assert(block);

		auto subview = view.view().subview(progress, 1 << blockPagesShift);

		if(type == kHelManageInitialize) {
			co_await device->readSectors(block * sectorsPerBlock, subview);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					offset + progress, 1 << blockPagesShift));
		}else{
			assert(type == kHelManageWriteback);

			co_await device->writeSectors(block * sectorsPerBlock, subview);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					offset + progress, 1 << blockPagesShift));
		}
	}

	ostContext.emit(
		ostEvtExt2ManageBlockBitmap,
		ostAttrTime(timer.elapsed())
	);
}

async::detached FileSystem::manageInodeBitmap(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		serviceInodeBitmapRequest(memory, manage.type(), manage.offset(), manage.length());
	}
}

async::detached FileSystem::serviceInodeBitmapRequest(helix::BorrowedDescriptor memory,
		int type, uintptr_t offset, size_t length) {
	protocols::ostrace::Timer timer;

	assert(!(offset & ((1 << blockPagesShift) - 1))
			&& "TODO: properly support multi-page blocks");

	auto view = pool->importMemory(memory, offset, length);

	for(size_t progress = 0; progress < length; progress += (1 << blockPagesShift)) {
		auto bg_idx = (offset + progress) >> blockPagesShift;
		auto block = bgdt[bg_idx].inodeBitmap;
		assert(block);

		auto subview = view.view().subview(progress, 1 << blockPagesShift);

		if(type == kHelManageInitialize) {
			co_await device->readSectors(block * sectorsPerBlock, subview);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					offset + progress, 1 << blockPagesShift));
		}else{
			assert(type == kHelManageWriteback);

			co_await device->writeSectors(block * sectorsPerBlock, subview);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					offset + progress, 1 << blockPagesShift));
		}
	}

	ostContext.emit(
		ostEvtExt2ManageInodeBitmap,
		ostAttrTime(timer.elapsed())
	);
}

async::detached FileSystem::manageInodeTable(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		serviceInodeTableRequest(memory, manage.type(), manage.offset(), manage.length());
	}
}

async::detached FileSystem::serviceInodeTableRequest(helix::BorrowedDescriptor memory,
		int type, uintptr_t offset, size_t length) {
	protocols::ostrace::Timer timer;

	// TODO: Make sure that we do not read/write past the end of the table.
	assert(!((inodesPerGroup * inodeSize) & (blockSize - 1)));

	auto sizePerGroup = inodesPerGroup * inodeSize;
	// TODO: It would be possible to support this by separating
	//       different block group inside the managed memory representing the inode table
	//       (or by having per-block-group managed memory objects for the inode table).
	if (sizePerGroup & (pageSize - 1))
		logPanic("Missing support for inode table sizes that are not multiples of the page size");

	auto view = pool->importMemory(memory, offset, length);

	size_t progress = 0;
	while (progress < length) {
		// TODO: Use shifts instead of division.
		auto bg_idx = (offset + progress) / sizePerGroup;
		auto bg_offset = (offset + progress) % sizePerGroup;
		auto block = bgdt[bg_idx].inodeTable;
		assert(block);

		// Do not cross block group boundaries.
		auto chunk = std::min(length - progress, sizePerGroup - bg_offset);
		assert(!(progress & (pageSize - 1))); // Guaranteed by the next assertion.
		assert(!(chunk & (pageSize - 1))); // Otherwise, the panic above would trigger.

		assert(bg_offset % device->sectorSize == 0);
		assert(chunk % device->sectorSize == 0);

		auto subview = view.view().subview(progress, chunk);

		if(type == kHelManageInitialize) {
			co_await device->readSectors(
			    block * sectorsPerBlock + bg_offset / device->sectorSize, subview
			);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					offset + progress, chunk));
		}else{
			assert(type == kHelManageWriteback);

			co_await device->writeSectors(
			    block * sectorsPerBlock + bg_offset / device->sectorSize, subview
			);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					offset + progress, chunk));
		}

		progress += chunk;
	}

	ostContext.emit(
		ostEvtExt2ManageInode,
		ostAttrTime(timer.elapsed())
	);
}

auto FileSystem::accessRoot() -> std::shared_ptr<BaseInode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<BaseInode> {
	assert(number > 0);

	std::lock_guard activeInodesLock{activeInodesMutex};

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

	if(usesExtents) {
		auto &hdr = disk_inode->data.extents.hdr;
		hdr.magic = EXT4_EXTENT_MAGIC;
		hdr.max = sizeof(disk_inode->data.extents.extents) / sizeof(Extent);
		disk_inode->flags |= EXT4_EXTENTS_FL;
	}

	updateInodeChecksum(*this, disk_inode, ino);

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

	if(usesExtents) {
		auto &hdr = disk_inode->data.extents.hdr;
		hdr.magic = EXT4_EXTENT_MAGIC;
		hdr.max = sizeof(disk_inode->data.extents.extents) / sizeof(Extent);
		disk_inode->flags |= EXT4_EXTENTS_FL;
	}

	updateInodeChecksum(*this, disk_inode, ino);

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

	updateInodeChecksum(*this, disk_inode, ino);

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

		// Service the request in a detached coroutine so that multiple requests
		// can be in flight concurrently.
		serviceFileDataRequest(inode, manage.type(), manage.offset(), manage.length());
	}
}

async::detached FileSystem::serviceFileDataRequest(std::shared_ptr<Inode> inode,
		int type, uintptr_t offset, size_t length) {
	if(type == kHelManageInitialize) {
		assert(offset + length <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF)));
	}else{
		if(!(offset + length <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF))))
			co_return;
	}

	protocols::ostrace::Timer timer;
	auto fileView = pool->importMemory(
	    helix::BorrowedDescriptor{inode->backingMemory}, offset, length
	);

	if(type == kHelManageInitialize) {
		assert(!(offset % inode->fs.blockSize));
		size_t backed_size = std::min(length, inode->fileSize() - offset);
		size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;
		assert(num_blocks * inode->fs.blockSize <= length);

		{
			co_await inode->blockMapMutex.async_lock_shared();
			frg::shared_lock blockMapLock{frg::adopt_lock, inode->blockMapMutex};
			co_await inode->fs.readDataBlocks(inode, offset / inode->fs.blockSize, fileView);
		}

		HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageInitialize,
				offset, length));
	}else{
		assert(type == kHelManageWriteback);

		assert(!(offset % inode->fs.blockSize));
		size_t backedSize = std::min(length, inode->fileSize() - offset);
		auto blockOffset = offset / inode->fs.blockSize;
		size_t numBlocks = (backedSize + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

		assert(numBlocks * inode->fs.blockSize <= length);

		// If all blocks are already mapped, the write can proceed under the shared lock
		// (concurrently with other reads and writes) since blocks are never deallocated.
		bool mapped;
		{
			co_await inode->blockMapMutex.async_lock_shared();
			frg::shared_lock blockMapLock{frg::adopt_lock, inode->blockMapMutex};
			mapped = co_await inode->fs.dataBlocksMapped(inode, blockOffset, numBlocks);
			if(mapped)
				co_await inode->fs.writeDataBlocks(inode, blockOffset, fileView);
		}

		if(!mapped) {
			{
				co_await inode->blockMapMutex.async_lock();
				frg::unique_lock blockMapLock{frg::adopt_lock, inode->blockMapMutex};
				co_await inode->fs.assignDataBlocks(inode.get(), blockOffset, numBlocks);
			}

			co_await inode->blockMapMutex.async_lock_shared();
			frg::shared_lock blockMapLock{frg::adopt_lock, inode->blockMapMutex};
			co_await inode->fs.writeDataBlocks(inode, blockOffset, fileView);
		}

		HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageWriteback,
				offset, length));
	}

	ostContext.emit(
		ostEvtExt2ManageFile,
		ostAttrTime(timer.elapsed())
	);
}

async::result<std::vector<uint32_t>> FileSystem::allocateBlocks(size_t num, std::optional<uint32_t> ino) {
	protocols::ostrace::Timer timer;
	std::vector<uint32_t> result;

	co_await allocationMutex.async_lock();
	frg::unique_lock allocationLock{frg::adopt_lock, allocationMutex};

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
						updateBlockBitmapChecksum(*this, &bgdt[preferred_bg], words, blockSize);
						updateBlockGroupChecksum(*this, &bgdt[preferred_bg], preferred_bg);

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

			if(!result.empty()) {
				updateBlockBitmapChecksum(*this, &bgdt[preferred_bg], words, blockSize);
				updateBlockGroupChecksum(*this, &bgdt[preferred_bg], preferred_bg);

				auto syncBitmap = co_await helix_ng::synchronizeSpace(
						helix::BorrowedDescriptor{kHelNullHandle},
						words, 1 << blockPagesShift);
				HEL_CHECK(syncBitmap.error());
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
					updateBlockBitmapChecksum(*this, &bgdt[bg_idx], words, blockSize);
					updateBlockGroupChecksum(*this, &bgdt[bg_idx], bg_idx);

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

		updateBlockBitmapChecksum(*this, &bgdt[bg_idx], words, blockSize);
		updateBlockGroupChecksum(*this, &bgdt[bg_idx], bg_idx);

		auto syncBitmap = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle},
				words, 1 << blockPagesShift);
		HEL_CHECK(syncBitmap.error());
	}

	assert(!"Failed to find zero-bit");
}

async::result<uint32_t> FileSystem::allocateInode(uint32_t parentIno, bool directory) {
	protocols::ostrace::Timer timer;

	co_await allocationMutex.async_lock();
	frg::unique_lock allocationLock{frg::adopt_lock, allocationMutex};

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

				updateInodeBitmapChecksum(*this, &bgdt[bg], words, blockSize);
				updateBlockGroupChecksum(*this, &bgdt[bg], bg);

				markBgdtDirty();

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

	for(auto &range : blockRanges) {
		if(range.found)
			continue;

		auto allocated = co_await allocateBlocks(range.size, inode->number);
		assert(!allocated.empty() && "Out of disk space");

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

	updateInodeChecksum(*this, diskInode, inode->number);

	markBgdtDirty();
	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			inode->diskInode(), inodeSize);
	HEL_CHECK(syncInode.error());

	ostContext.emit(
		ostEvtExt2AssignDataBlocks,
		ostAttrTime(timer.elapsed())
	);
}

async::result<void> FileSystem::readDataBlocksUsingExtents(std::shared_ptr<Inode> inode, uint64_t block_offset,
		arch::dma_buffer_view buf) {
	co_await inode->readyEvent.wait();
	// TODO: Assert that we do not read past the EOF.

	size_t num_blocks = buf.size() >> blockShift;
	auto blockRanges = co_await lookupBlocksUsingExtent(inode.get(), block_offset, num_blocks, false);

	size_t progress = 0;
	for(auto &range : blockRanges) {
		assert(range.relativeStartBlock == block_offset + progress);

		if(!range.found) {
			memset(buf.byte_data() + progress * blockSize, 0, range.size * blockSize);
		}else {
			assert(range.absoluteStartBlock);
			co_await device->readSectors(
			    range.absoluteStartBlock * sectorsPerBlock,
			    buf.subview(progress * blockSize, range.size * blockSize)
			);
		}

		progress += range.size;
	}

	assert(progress == num_blocks);
}

async::result<void> FileSystem::writeDataBlocksUsingExtents(std::shared_ptr<Inode> inode, uint64_t block_offset,
		arch::dma_buffer_view buf) {
	co_await inode->readyEvent.wait();
	// TODO: Assert that we do not read past the EOF.

	size_t num_blocks = buf.size() >> blockShift;
	auto blockRanges = co_await lookupBlocksUsingExtent(inode.get(), block_offset, num_blocks, true);

	size_t progress = 0;
	for(auto &range : blockRanges) {
		co_await device->writeSectors(
		    range.absoluteStartBlock * sectorsPerBlock,
		    buf.subview(progress * blockSize, range.size * blockSize)
		);
		progress += range.size;
	}

	assert(progress == num_blocks);
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
				prg += allocated.size();
			}
		}else{
			assert(!"TODO: Implement allocation in triple indirect blocks");
		}
	}

	updateInodeChecksum(*this, inode->diskInode(), inode->number);

	markBgdtDirty();
	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			inode->diskInode(), inodeSize);
	HEL_CHECK(syncInode.error());

	ostContext.emit(
		ostEvtExt2AssignDataBlocks,
		ostAttrTime(timer.elapsed())
	);
}

async::result<bool> FileSystem::dataBlocksMapped(std::shared_ptr<Inode> inode,
		uint64_t block_offset, size_t num_blocks) {
	co_await inode->readyEvent.wait();

	if(inode->usesExtents) {
		auto blockRanges = co_await lookupBlocksUsingExtent(inode.get(),
				block_offset, num_blocks, false);
		for(auto &range : blockRanges)
			if(!range.found)
				co_return false;
		co_return true;
	}

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	auto disk_inode = inode->diskInode();

	size_t progress = 0;
	while(progress < num_blocks) {
		auto index = block_offset + progress;

		assert(index < d_range);
		if(index >= s_range) { // Use the double indirect block.
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			if(!disk_inode->data.blocks.doubleIndirect)
				co_return false;

			uint32_t indirect_block = 0;
			co_await metadataCache->read(disk_inode->data.blocks.doubleIndirect,
					indirect_frame * 4, 4, &indirect_block);
			if(!indirect_block)
				co_return false;

			auto indirectWindow = co_await metadataCache->access(indirect_block, false);
			auto entries = reinterpret_cast<uint32_t *>(indirectWindow.get());

			auto chunk = std::min<size_t>(num_blocks - progress, per_indirect - indirect_index);
			for(size_t i = 0; i < chunk; i++)
				if(!entries[indirect_index + i])
					co_return false;
			progress += chunk;
		}else if(index >= i_range) { // Use the single indirect block.
			auto indirect_index = index - i_range;

			if(!disk_inode->data.blocks.singleIndirect)
				co_return false;

			auto indirectWindow = co_await metadataCache->access(
					disk_inode->data.blocks.singleIndirect, false);
			auto entries = reinterpret_cast<uint32_t *>(indirectWindow.get());

			auto chunk = std::min<size_t>(num_blocks - progress, per_single - indirect_index);
			for(size_t i = 0; i < chunk; i++)
				if(!entries[indirect_index + i])
					co_return false;
			progress += chunk;
		}else{
			if(!disk_inode->data.blocks.direct[index])
				co_return false;
			progress++;
		}
	}

	co_return true;
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
			co_await device->readSectors(issue.first * sectorsPerBlock, buf.subview(progress * blockSize, issue.second * blockSize));
		} else {
			memset(buf.byte_data() + progress * blockSize, 0, issue.second * blockSize);
		}
		progress += issue.second;
	}
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
		co_await device->writeSectors(
		    issue.first * sectorsPerBlock,
		    buf.subview(progress * blockSize, issue.second * blockSize)
		);
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

