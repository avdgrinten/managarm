#include <frg/bitops.hpp>
#include <protocols/posix/procdata.hpp>
#include <linux/btrfs_tree.h>
#include <linux/magic.h>
#include <print>
#include <sys/stat.h>

#include "btrfs.hpp"
#include "pretty-print.hpp"
#include "spec.hpp"

namespace blockfs::btrfs {

Inode::Inode(FileSystem &fs, uint32_t number) : BaseInode{fs, number}, fs_{fs} {}

async::result<protocols::fs::Error> Inode::updateTimes(
    std::optional<::timespec> atime,
    std::optional<::timespec> mtime,
    std::optional<::timespec> ctime
) {
	(void)atime;
	(void)ctime;
	(void)mtime;

	co_return protocols::fs::Error::none;
}

async::result<frg::expected<protocols::fs::Error>> Inode::resizeFile(size_t newSize) {
	(void)newSize;
	std::println("libblockfs/btrfs: resizeFile unimplemented!");
	co_return protocols::fs::Error::internalError;
}

async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
Inode::findEntry(std::string name) {
	co_await readyEvent.wait();
	auto fs = static_cast<btrfs::FileSystem *>(&this->fs_);

	BtreePtr ptr{};
	Key searchKey{number, ItemType::DIR_ITEM};
	auto val = co_await fs->lowerBound(fs->fsTreeRoot_, searchKey, ptr);
	if (!val)
		co_return protocols::fs::Error::fileNotFound;

	do {
		if (ptr.back().key.noOffset() != searchKey.noOffset())
			break;

		auto item = reinterpret_cast<const struct DirItem *>(val->data());
		auto name_span = val->subspan(
		    sizeof(struct DirItem),
		    std::min(val->size() - sizeof(struct DirItem), size_t(item->name_len))
		);
		auto entry_name =
		    std::string_view{reinterpret_cast<const char *>(name_span.data()), name_span.size()};

		if (entry_name != name)
			continue;

		if constexpr (verboseLogging)
			std::println("\tconsidering dir entry '{}' for inode {}", entry_name, this->number);

		if (entry_name == name) {
			DirEntry entry;
			entry.inode = item->location.objectid;

			switch (item->type) {
				case 1:
					entry.fileType = kTypeRegular;
					break;
				case 2:
					entry.fileType = kTypeDirectory;
					break;
				case 7:
					entry.fileType = kTypeSymlink;
					break;
				default:
					entry.fileType = kTypeNone;
			}

			co_return entry;
		}
	} while ((val = co_await fs->nextKey(ptr)));

	co_return protocols::fs::Error::fileNotFound;
}

FileSystem::FileSystem(BlockDevice *device) : device_{device} {}

async::result<void> FileSystem::init() {
	constexpr uint64_t superBlockOffset = 0x10000;

	size_t deviceSuperBlockSector = superBlockOffset / device_->sectorSize;
	size_t deviceSuperBlockOffset = superBlockOffset % device_->sectorSize;
	size_t deviceSuperBlockSectors =
	    frg::align_up(sizeof(Superblock) + deviceSuperBlockOffset, device_->sectorSize)
	    / device_->sectorSize;

	std::vector<uint8_t> buffer(deviceSuperBlockSectors * device_->sectorSize);
	co_await device_->readSectors(deviceSuperBlockSector, buffer.data(), deviceSuperBlockSectors);

	memcpy(&superblock_, buffer.data() + deviceSuperBlockOffset, sizeof(Superblock));
	assert(!strncmp(superblock_.magic, "_BHRfS_M", 8));

	std::println("libblockfs: mounting btrfs fs {}", superblock_.fs_uuid);
	std::println("libblockfs: {}/{} bytes used", superblock_.bytes_used, superblock_.total_bytes);

	// Read the bootstrap chunk table from the superblock. This is needed for translating the
	// logical addresses to physical when reading the chunk tree.
	size_t deviceBootstrapChunkSector =
	    (superBlockOffset + sizeof(Superblock)) / device_->sectorSize;
	size_t deviceBootstrapChunkOffset =
	    (superBlockOffset + sizeof(Superblock)) % device_->sectorSize;
	size_t deviceBootstrapChunkSectors =
	    frg::align_up(
	        sizeof(Superblock) + deviceBootstrapChunkSector + superblock_.sys_chunk_array_size,
	        device_->sectorSize
	    )
	    / device_->sectorSize;
	std::vector<uint8_t> bootstrapChunkBuffer(deviceBootstrapChunkSectors * device_->sectorSize);
	co_await device_->readSectors(
	    deviceBootstrapChunkSector, bootstrapChunkBuffer.data(), deviceBootstrapChunkSectors
	);

	size_t nextChunkOffset = 0;

	while (nextChunkOffset < superblock_.sys_chunk_array_size) {
		assert(
		    superblock_.sys_chunk_array_size - nextChunkOffset >= sizeof(Key) + sizeof(ChunkItem)
		);

		Key chunk_key;
		memcpy(
		    &chunk_key,
		    bootstrapChunkBuffer.data() + deviceBootstrapChunkOffset + nextChunkOffset,
		    sizeof(Key)
		);
		nextChunkOffset += sizeof(Key);

		ChunkItem chunk;
		memcpy(
		    &chunk,
		    bootstrapChunkBuffer.data() + deviceBootstrapChunkOffset + nextChunkOffset,
		    sizeof(ChunkItem)
		);
		nextChunkOffset += sizeof(ChunkItem);

		ChunkStripe stripe;
		memcpy(
		    &stripe,
		    bootstrapChunkBuffer.data() + deviceBootstrapChunkOffset + nextChunkOffset,
		    sizeof(ChunkStripe)
		);
		nextChunkOffset += chunk.stripe_count * sizeof(ChunkStripe);

		CachedChunk cachedChunk{
		    .addr = LogicalAddress{chunk_key.offset},
		    .size = chunk.chunk_size,
		    .stripe = stripe,
		};

		cachedChunks_.emplace(chunk_key.offset, cachedChunk);
	}

	// Traverse the chunk tree and cache the chunks.
	auto gen = traverse(superblock_.chunk_tree_root);

	while (auto val = co_await gen.next()) {
		auto [key, data] = *val;
		if (key.type != ItemType::CHUNK_ITEM)
			continue;

		assert(data.size() >= sizeof(ChunkItem));
		ChunkItem chunk;
		memcpy(&chunk, data.data(), sizeof(ChunkItem));

		assert(data.size() >= sizeof(ChunkItem) + chunk.stripe_count * sizeof(ChunkStripe));
		ChunkStripe stripe;
		memcpy(&stripe, data.data() + sizeof(ChunkItem), sizeof(ChunkStripe));

		CachedChunk cachedChunk{
		    .addr = LogicalAddress{key.offset},
		    .size = chunk.chunk_size,
		    .stripe = stripe,
		};

		cachedChunks_.emplace(key.offset, cachedChunk);
	}

	if constexpr (debugTreeWalking) {
		for (auto [logical, info] : cachedChunks_) {
			std::println(
			    "libblockfs: chunk at logical address {:#x}, size {:#x}, on device id {} at "
			    "physical "
			    "address {:#x}",
			    static_cast<uint64_t>(info.addr),
			    info.size,
			    info.stripe.device_id,
			    static_cast<uint64_t>(info.stripe.offset)
			);
		}
	}

	// Allocate a page cache for the tree structures.
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), 
	    superblock_.total_bytes, 0, &treeBackingMemory, &treeFrontalMemory
	));
	manageTree();

	// Find the root dir's DIR_ITEM for resolving the FS tree ROOT_ITEM.
	BtreePtr ptr{};
	Key searchKey{superblock_.root_dir_objectid, ItemType::DIR_ITEM};
	auto treeRootItem = co_await lowerBound(superblock_.root_tree_root, searchKey, ptr);
	assert(treeRootItem);
	assert(ptr.back().key.noOffset() == searchKey.noOffset());
	auto di = reinterpret_cast<DirItem *>(treeRootItem->data());
	assert(di->location.type == ItemType::ROOT_ITEM);
	assert(di->location.offset == UINT64_C(-1));

	BtreePtr ptrn = {};
	auto val =
	    co_await find(superblock_.root_tree_root, {di->location.objectid, ItemType::ROOT_ITEM}, ptrn);
	assert(val);
	auto root_item = reinterpret_cast<struct RootItem *>(val->data());
	fsTreeRoot_ = LogicalAddress{root_item->bytenr};
	rootInode_ = root_item->root_dir_id;
}

async::detached FileSystem::manageTree() {
	while (true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(
		    helix::BorrowedDescriptor(treeBackingMemory), &manage, helix::Dispatcher::global()
		);
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		assert(manage.offset() + manage.length() <= superblock_.total_bytes);

		if (manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{
			    helix::BorrowedDescriptor{treeBackingMemory},
			    static_cast<ptrdiff_t>(manage.offset()),
			    manage.length(),
			    kHelMapProtWrite
			};

			assert(!(manage.offset() % superblock_.sector_size));
			size_t backed_size =
			    std::min(manage.length(), superblock_.total_bytes - manage.offset());
			size_t num_sectors =
			    frg::align_up(backed_size, device_->sectorSize) / device_->sectorSize;

			assert(num_sectors * device_->sectorSize <= manage.length());

			auto managedChunk = std::span<std::byte>{
			    reinterpret_cast<std::byte *>(file_map.get()), manage.length()
			};

			co_await device_->readSectors(
			    manage.offset() / device_->sectorSize, managedChunk.data(), num_sectors
			);

			HEL_CHECK(helUpdateMemory(
			    treeBackingMemory, kHelManageInitialize, manage.offset(), manage.length()
			));
		} else {
			std::println("libblockfs/btrfs: writeback unimplemented for tree data!");
			HEL_CHECK(helUpdateMemory(treeBackingMemory, kHelManageWriteback,
				manage.offset(), manage.length()));
		}
	}
}

#pragma mark - tree traversal utilities

// Obtain a Mapping for the b-tree node at the logical address `start`.
async::result<helix::Mapping>
FileSystem::getBtreeNode(LogicalAddress start) {
	PhysicalAddress phys{this, start};
	assert(static_cast<uint64_t>(phys) % device_->sectorSize == 0);

	helix::LockMemoryView lock;
	auto &&submit = helix::submitLockMemoryView(
	    helix::BorrowedDescriptor{treeFrontalMemory},
	    &lock,
	    ptrdiff_t(uint64_t(phys)),
	    superblock_.node_size,
	    helix::Dispatcher::global()
	);
	co_await submit.async_wait();
	HEL_CHECK(lock.error());

	helix::Mapping nodeMapping{
	    helix::BorrowedDescriptor{treeFrontalMemory},
	    ptrdiff_t(uint64_t(phys)),
	    superblock_.node_size
	};

	co_return std::move(nodeMapping);
}

// Recursively find the btrfs key `k` in the tree rooted at `start`.
// For every level traversed, a layer is pushed onto the traversal stack `stack`.
// If the key was not found, std::nullopt is returned. The stack is guaranteed to only hold
// traversal layers (of inner nodes) whose range could contain the search key.
async::result<std::optional<std::span<std::byte>>>
FileSystem::find(LogicalAddress start, Key k, BtreePtr &stack) {
	auto nodeMapping = co_await getBtreeNode(start);
	auto header = BlockHeader::fromMapping(nodeMapping);

	if (header->level != 0) {
		auto ub = std::ranges::upper_bound(header->keyPtrs(), k, {}, &KeyPtr::k);
		if (ub == header->keyPtrs().begin())
			co_return std::nullopt;
		auto prev = std::ranges::prev(ub);

		stack.emplace_back(start, prev->k, std::move(nodeMapping));

		co_return co_await find(prev->addr, k, stack);
	} else {
		auto lb = std::ranges::lower_bound(header->items(), k, {}, &Item::k);
		if (lb != header->items().end() && lb->k == k) {
			stack.emplace_back(start, lb->k, std::move(nodeMapping));
			co_return std::span<std::byte>{
			    reinterpret_cast<std::byte *>(stack.back().mapping.get()) + sizeof(*header)
			        + lb->data_offset,
			    lb->data_size
			};
		}
	}

	co_return std::nullopt;
}

// Similar to `find()`, but finds the first key that compares greater or equal to `k`.
async::result<std::optional<std::span<std::byte>>>
FileSystem::lowerBound(LogicalAddress start, Key k, BtreePtr &stack) {
	auto nodeMapping = co_await getBtreeNode(start);
	auto header = BlockHeader::fromMapping(nodeMapping);

	if (header->level != 0) {
		auto ub = std::ranges::upper_bound(header->keyPtrs(), k, {}, &KeyPtr::k);
		if (ub == header->keyPtrs().begin())
			co_return std::nullopt;

		stack.emplace_back(start, std::ranges::prev(ub)->k, std::move(nodeMapping));

		co_return co_await lowerBound(std::ranges::prev(ub)->addr, k, stack);
	} else {
		auto lb = std::ranges::lower_bound(header->items(), k, {}, &Item::k);
		if (lb != header->items().end()) {
			stack.emplace_back(start, lb->k, std::move(nodeMapping));
			co_return std::span<std::byte>{
				reinterpret_cast<std::byte *>(stack.back().mapping.get()) + sizeof(*header) + lb->data_offset, lb->data_size
			};
		}
	}

	co_return std::nullopt;
}

// Similar to `find()`, but finds the first key that compares greater than `k`.
async::result<std::optional<std::span<std::byte>>>
FileSystem::upperBound(LogicalAddress start, Key k, BtreePtr &stack) {
	auto nodeMapping = co_await getBtreeNode(start);
	auto header = BlockHeader::fromMapping(nodeMapping);

	if (header->level != 0) {
		auto ub = std::ranges::upper_bound(header->keyPtrs(), k, {}, &KeyPtr::k);
		if (ub == header->keyPtrs().begin())
			co_return std::nullopt;

		stack.emplace_back(start, std::ranges::prev(ub)->k, std::move(nodeMapping));

		auto first = co_await upperBound(std::ranges::prev(ub)->addr, k, stack);
		if (first || ub == header->keyPtrs().end())
			co_return first;

		co_return co_await upperBound(ub->addr, k, stack);
	} else {
		auto lb = std::ranges::upper_bound(header->items(), k, {}, &Item::k);
		if (lb != header->items().end()) {
			stack.emplace_back(start, lb->k, std::move(nodeMapping));
			co_return std::span<std::byte>{
				reinterpret_cast<std::byte *>(stack.back().mapping.get()) + sizeof(*header) + lb->data_offset, lb->data_size
			};
		}
	}

	co_return std::nullopt;
}

// Return the data for the lowest-ordered key in the tree rooted at `root`.
async::result<std::optional<std::span<std::byte>>>
FileSystem::firstKey(LogicalAddress root, BtreePtr &stack) {
	auto nodeMapping = co_await getBtreeNode(root);
	auto header = BlockHeader::fromMapping(nodeMapping);

	if (header->level != 0) {
		stack.emplace_back(root, header->keyPtrs().at(0).k, std::move(nodeMapping));
		co_return co_await firstKey(header->keyPtrs().at(0).addr, stack);
	} else {
		stack.emplace_back(root, header->items().at(0).k, std::move(nodeMapping));
		co_return std::span<std::byte>{
			reinterpret_cast<std::byte *>(stack.back().mapping.get()) + sizeof(*header) + header->items().at(0).data_offset,
			header->items().at(0).data_size
		};
	}
}

// From the current stack pointing to a key in a leaf node, find the next key. If the key is not
// on the same leaf node, the b-tree will be traversed to the next leaf node, and the `stack`
// adjusted to reflect that. If there is no next node, std::nullopt is returned.
async::result<std::optional<std::span<std::byte>>> FileSystem::nextKey(BtreePtr &stack) {
	assert(!stack.empty());

	{
		auto &layer = stack.back();

		BlockHeader *header = reinterpret_cast<BlockHeader *>(layer.mapping.get());
		assert(header->level == 0);

		auto ub = std::ranges::upper_bound(header->items(), layer.key, {}, &Item::k);
		if (ub != header->items().end()) {
			layer.key = ub->k;

			co_return std::span<std::byte>{
			    reinterpret_cast<std::byte *>(layer.mapping.get()) + sizeof(*header) + ub->data_offset, ub->data_size
			};
		}
	}

	while (true) {
		stack.pop_back();
		if (stack.empty())
			co_return std::nullopt;

		auto &layer = stack.back();
		BlockHeader *header = reinterpret_cast<BlockHeader *>(layer.mapping.get());

		auto ub = std::ranges::upper_bound(header->keyPtrs(), layer.key, {}, &KeyPtr::k);
		if (ub == header->keyPtrs().end())
			continue;

		co_return co_await firstKey(ub->addr, stack);
	}
}

// Produce an async generator for lazy in-order traversal of the tree rooted at `start`.
async::generator<std::tuple<Key, std::span<std::byte>>> FileSystem::traverse(LogicalAddress start) {
	std::vector<std::byte> blockBuffer(
	    device_->sectorSize * (superblock_.node_size / device_->sectorSize)
	);
	if constexpr (debugTreeWalking)
		std::println(
		    "libblockfs: traversing tree at logical address {:#x}", static_cast<uint64_t>(start)
		);
	PhysicalAddress phys{this, start};
	assert(static_cast<uint64_t>(phys) % device_->sectorSize == 0);

	if constexpr (debugTreeWalking)
		std::println(
		    "libblockfs: traversing tree at logical address {:#x} (physical address {:#x})",
		    static_cast<uint64_t>(start),
		    static_cast<uint64_t>(phys)
		);

	co_await device_->readSectors(
	    static_cast<uint64_t>(phys) / device_->sectorSize,
	    reinterpret_cast<uint8_t *>(blockBuffer.data()),
	    superblock_.node_size / device_->sectorSize
	);

	BlockHeader *header = reinterpret_cast<BlockHeader *>(blockBuffer.data());

	// TODO: base this on a BtreePtr
	if (header->level != 0) {
		std::println("libblockfs: non-leaf node traversal not implemented yet");
		assert(!"non-leaf node traversal not implemented yet");
	}

	if constexpr (debugTreeWalking) {
		std::println("libblockfs: leaf node with {} items", header->nritems);
		std::println(
		    "libblockfs: owner={} bytenr={:#x} fs_uuid={}",
		    header->owner,
		    header->bytenr,
		    header->fs_uuid
		);
	}

	for (size_t i = 0; i < header->nritems; i++) {
		Item it;
		memcpy(&it, blockBuffer.data() + sizeof(BlockHeader) + i * sizeof(Item), sizeof(Item));
		co_yield std::make_tuple(
		    it.k,
		    std::span<std::byte>{
		        blockBuffer.data() + sizeof(*header) + it.data_offset, it.data_size
		    }
		);
	}
}

#pragma mark - file/inode operations

extern protocols::fs::FileOperations fileOperations;
extern protocols::fs::NodeOperations nodeOperations;

const protocols::fs::FileOperations *FileSystem::fileOps() { return &fileOperations; }

const protocols::fs::NodeOperations *FileSystem::nodeOps() { return &nodeOperations; }

auto FileSystem::accessRoot() -> std::shared_ptr<BaseInode> { return accessInode(rootInode_); }

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<BaseInode> {
	std::weak_ptr<Inode> &inode_slot = activeInodes[number];
	std::shared_ptr<Inode> active_inode = inode_slot.lock();
	if (active_inode)
		return active_inode;

	auto new_inode = std::make_shared<Inode>(*this, number);
	inode_slot = std::weak_ptr<Inode>(new_inode);
	initiateInode(new_inode);

	return new_inode;
}

async::detached FileSystem::initiateInode(std::shared_ptr<Inode> inode) {
	BtreePtr ptr{};
	auto val = co_await find(fsTreeRoot_, {inode->number, ItemType::INODE_ITEM}, ptr);
	assert(val);

	auto disk_inode = reinterpret_cast<const InodeItem *>(val->data());

	inode->size_ = disk_inode->size;
	inode->uid = disk_inode->uid;
	inode->gid = disk_inode->gid;
	if (S_ISDIR(disk_inode->mode))
		inode->fileType = kTypeDirectory;
	else if (S_ISREG(disk_inode->mode))
		inode->fileType = kTypeRegular;
	else if (S_ISLNK(disk_inode->mode))
		inode->fileType = kTypeSymlink;
	else
		assert(!"unsupported inode type");

	// Allocate a page cache for the file.
	auto cache_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), 
	    cache_size, kHelManagedReadahead, &inode->backingMemory, &inode->frontalMemory
	));

	manageFileData(inode);

	inode->readyEvent.raise();
}

async::detached FileSystem::manageFileData(std::shared_ptr<Inode> inode) {
	while (true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(
		    helix::BorrowedDescriptor(inode->backingMemory), &manage, helix::Dispatcher::global()
		);
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		assert(manage.offset() + manage.length() <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF)));

		if (manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{
			    helix::BorrowedDescriptor{inode->backingMemory},
			    static_cast<ptrdiff_t>(manage.offset()),
			    manage.length(),
			    kHelMapProtWrite
			};

			assert(!(manage.offset() % inode->fs_.superblock_.sector_size));

			size_t progress = 0;
			auto managedChunk = std::span<std::byte>{
			    reinterpret_cast<std::byte *>(file_map.get()), manage.length()
			};

			BtreePtr ptr{};
			Key searchKey{inode->number, ItemType::EXTENTDATA_ITEM};
			auto val = co_await lowerBound(fsTreeRoot_, searchKey, ptr);
			assert(val);

			do {
				if (ptr.back().key.noOffset() != searchKey.noOffset())
					break;

				auto ed = reinterpret_cast<const ExtentData *>(val->data());
				if (ed->type == 0) {
					size_t extentDataSize = val->size_bytes() - sizeof(*ed);
					size_t to_copy = frg::min(manage.length() - progress, extentDataSize);
					std::ranges::copy(
					    val->subspan(sizeof(*ed), to_copy), managedChunk.subspan(progress).begin()
					);
					progress += to_copy;
				} else {
					auto extraData =
					    reinterpret_cast<const ExtentDataExtra *>(val->data() + sizeof(*ed));
					size_t to_copy = frg::min(manage.length() - progress, extraData->num_bytes);

					// handle sparse extent
					if (uint64_t{extraData->extent_addr} == 0) {
						std::ranges::fill(managedChunk.subspan(progress, to_copy), std::byte{0});
						progress += to_copy;
						continue;
					}

					assert(ed->compression == 0);
					assert(extraData->extent_offset == 0);
					assert((to_copy % device_->sectorSize) == 0);
					assert((to_copy % superblock_.sector_size) == 0);

					PhysicalAddress extent{this, extraData->extent_addr};
					co_await device_->readSectors(
					    uint64_t{extent} / device_->sectorSize,
					    reinterpret_cast<uint8_t *>(managedChunk.subspan(progress).data()),
					    to_copy / device_->sectorSize
					);
					progress += to_copy;
				}
			} while (progress < manage.length() && (val = co_await nextKey(ptr)).has_value());

			HEL_CHECK(helUpdateMemory(
			    inode->backingMemory, kHelManageInitialize, manage.offset(), manage.length()
			));
		} else {
			std::println("libblockfs/btrfs: writeback unimplemented for file data!");
			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageWriteback,
				manage.offset(), manage.length()));
		}
	}
}

async::result<std::shared_ptr<BaseInode>>
FileSystem::createRegular(int uid, int gid, uint32_t parentIno) {
	(void)uid;
	(void)gid;
	(void)parentIno;

	assert(!"unimplemented");
}

protocols::fs::FsStats FileSystem::getFsStats() {
	protocols::fs::FsStats stats{};
	stats.fsType = BTRFS_SUPER_MAGIC;
	stats.blockSize = superblock_.sector_size;
	stats.fragmentSize = superblock_.sector_size;
	stats.numBlocks = superblock_.total_bytes / superblock_.sector_size;
	stats.blocksFree = (superblock_.total_bytes - superblock_.bytes_used) / superblock_.sector_size;
	stats.blocksFreeUser = stats.blocksFree;
	stats.numInodes = stats.numBlocks;
	stats.inodesFree = stats.blocksFree;
	stats.inodesFreeUser = stats.inodesFree;
	stats.maxNameLength = BTRFS_NAME_LEN;

	uint32_t fsid[4];
	memcpy(fsid, &superblock_.fs_uuid, sizeof(superblock_.fs_uuid));

	// Linux generates the stat.fsid member exactly like this:
	stats.fsid[0] = std::byteswap(fsid[0]) ^ std::byteswap(fsid[2]);
	stats.fsid[1] = std::byteswap(fsid[1]) ^ std::byteswap(fsid[3]);
	// the root ino number is mixed in to disambigulate btrfs subvolumes.
	stats.fsid[0] ^= this->accessRoot()->number >> 32;
	stats.fsid[1] ^= this->accessRoot()->number;

	return stats;
}

PhysicalAddress::PhysicalAddress(FileSystem *fs, LogicalAddress logicalAddr) {
	if constexpr (verboseLogging)
		std::println(
		    "libblockfs: translating logical address {:#x}", static_cast<uint64_t>(logicalAddr)
		);

	// Find the chunk containing the logical address.
	auto it = fs->cachedChunks_.upper_bound(static_cast<uint64_t>(logicalAddr));
	assert(it != fs->cachedChunks_.begin());
	--it;

	auto &chunk = it->second;

	if constexpr (verboseLogging)
		std::println(
		    "\tfound chunk at logical address {:#x}, size {:#x}, on device id {} at physical "
		    "address "
		    "{:#x}",
		    static_cast<uint64_t>(chunk.addr),
		    chunk.size,
		    chunk.stripe.device_id,
		    static_cast<uint64_t>(chunk.stripe.offset)
		);

	assert(static_cast<uint64_t>(logicalAddr) >= static_cast<uint64_t>(chunk.addr));
	assert(static_cast<uint64_t>(logicalAddr) < static_cast<uint64_t>(chunk.addr) + chunk.size);

	// Compute the offset within the chunk.
	uint64_t offsetInChunk = static_cast<uint64_t>(logicalAddr) - static_cast<uint64_t>(chunk.addr);

	// For now, assume RAID0 with a single stripe.
	assert(chunk.stripe.device_id == 1);

	addr_ = static_cast<uint64_t>(chunk.stripe.offset) + offsetInChunk;
}

} // namespace blockfs::btrfs
