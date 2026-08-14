#pragma once

#include <expected>
#include <functional>
#include <string.h>
#include <time.h>
#include <optional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <protocols/fs/file-locks.hpp>

#include <async/oneshot-event.hpp>
#include <async/recurring-event.hpp>
#include <hel.h>

#include <blockfs.hpp>
#include "../common.hpp"
#include "fs.bragi.hpp"
#include "../fs.hpp"
#include "../metadata-cache.hpp"

namespace blockfs {
namespace ext2fs {

// --------------------------------------------------------
// On-disk structures
// --------------------------------------------------------

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct ExtentHeader {
	uint16_t magic;
	uint16_t entries;
	uint16_t max;
	uint16_t depth;
	uint32_t generation;
};
static_assert(sizeof(ExtentHeader) == 12, "Bad ExtentHeader struct size");

enum {
	EXT4_EXTENT_MAGIC = 0xF30A
};

struct ExtentIndex {
	uint32_t block;
	uint32_t leafLow;
	uint16_t leafHigh;
	uint16_t unused;
};
static_assert(sizeof(ExtentIndex) == 12, "Bad ExtentIndex struct size");

struct Extent {
	uint32_t block;
	uint16_t len;
	uint16_t startHigh;
	uint32_t startLow;
};
static_assert(sizeof(Extent) == 12, "Bad Extent struct size");

union FileData {
	struct Blocks {
		uint32_t direct[12];
		uint32_t singleIndirect;
		uint32_t doubleIndirect;
		uint32_t tripleIndirect;
	};

	struct Extents {
		ExtentHeader hdr;
		Extent extents[4];
	};

	Blocks blocks;
	Extents extents;
	uint8_t embedded[60];
};
static_assert(sizeof(FileData) == 60, "Bad FileData struct size");

struct DiskSuperblock {
	uint32_t inodesCount;
	uint32_t blocksCount;
	uint32_t rBlocksCount;
	uint32_t freeBlocksCount;
	uint32_t freeInodesCount;
	uint32_t firstDataBlock;
	uint32_t logBlockSize;
	uint32_t logFragSize;
	uint32_t blocksPerGroup;
	uint32_t fragsPerGroup;
	uint32_t inodesPerGroup;
	uint32_t mtime;
	uint32_t wtime;
	uint16_t mntCount;
	uint16_t maxMntCount;
	uint16_t magic;
	uint16_t state;
	uint16_t errors;
	uint16_t minorRevLevel;
	uint32_t lastcheck;
	uint32_t checkinterval;
	uint32_t creatorOs;
	uint32_t revLevel;
	uint16_t defResuid;
	uint16_t defResgid;
	//-- EXT2_DYNAMIC_REV Specific --
	uint32_t firstIno;
	uint16_t inodeSize;
	uint16_t blockGroupNr;
	uint32_t featureCompat;
	uint32_t featureIncompat;
	uint32_t featureRoCompat;
	uint8_t uuid[16];
	uint8_t volumeName[16];
	uint8_t lastMounted[64];
	uint32_t algoBitmap;
	//-- Performance Hints --
	uint8_t preallocBlocks;
	uint8_t preallocDirBlocks;
	union {
		uint16_t alignment;
		uint16_t reservedGdtBlocks;
	};
	//-- Journaling Support --
	uint8_t journalUuid[16];
	uint32_t journalInum;
	uint32_t journalDev;
	uint32_t lastOrphan;
	//-- Directory Indexing Support --
	uint32_t hashSeed[4];
	uint8_t defHashVersion;
	uint8_t jnlBackupType;
	// Valid only with EXT4_INCOMPAT_64BIT
	uint16_t groupDescSize;
	//-- Other options --
	uint32_t defaultMountOptions;
	uint32_t firstMetaBg;

	uint32_t mkfsTime;
	uint32_t jnlBlocks[17];
	//-- Valid only with EXT4_INCOMPAT_64BIT --
	uint32_t blocksCountHigh;
	uint32_t rBlocksCountHigh;
	uint32_t freeBlocksCountHigh;
	uint16_t minExtraIsize;
	uint16_t wantExtraIsize;
	uint32_t flags;
	uint16_t s_raid_stride;
	uint16_t mmpInterval;
	uint64_t mmpBlock;
	uint32_t raidStripeWidth;
	uint8_t logGroupsPerFlex;
	uint8_t checksumType;
	uint8_t encryptionLevel;
	uint8_t alignment1;
	uint64_t kbytesWritten;
	uint32_t snapshotInum;
	uint32_t snapshotId;
	uint64_t snapshotRBlocksCount;
	uint32_t snapshotList;
	uint32_t errorCount;
	uint32_t firstErrorTime;
	uint32_t firstErrorIno;
	uint64_t firstErrorBlock;
	uint8_t firstErrorFunc[32];
	uint32_t firstErrorLine;
	uint32_t lastErrorTime;
	uint32_t lastErrorIno;
	uint32_t lastErrorLine;
	uint64_t lastErrorBlock;
	uint8_t lastErrorFunc[32];
	uint8_t mountOpts[64];
	uint32_t usrQuotaInum;
	uint32_t grpQuotaInum;
	uint32_t overheadBlocks;
	// Valid only with EXT4_COMPAT_SPARSE_SUPER2
	uint32_t backupBgs[2];
	uint8_t encryptAlgos[4];
	uint8_t encryptPwSalt[16];
	uint32_t lpfIno;
	uint32_t prjQuotaInum;
	uint32_t checksumSeed;
	uint8_t wtimeHigh;
	uint8_t mtimeHigh;
	uint8_t mkfsTimeHigh;
	uint8_t lastcheckHigh;
	uint8_t firstErrorTimeHigh;
	uint8_t lastErrorTimeHigh;
	uint8_t firstErrorErrcode;
	uint8_t lastErrorErrcode;
	uint16_t encoding;
	uint16_t encodingFlags;
	uint32_t orphanFileInum;
	uint32_t unused[94];
	uint32_t checksum;
};
static_assert(sizeof(DiskSuperblock) == 1024, "Bad DiskSuperblock struct size");

enum {
	EXT4_COMPAT_HAS_JOURNAL = 0x4
};

enum {
	EXT4_INCOMPAT_EXTENTS = 0x40,
	EXT4_INCOMPAT_64BIT = 0x80,
	EXT4_INCOMPAT_FLEX_BG = 0x200,
	EXT4_INCOMPAT_CSUM_SEED = 0x2000,
	EXT4_INCOMPAT_LARGEDIR = 0x4000
};

enum {
	EXT4_RO_COMPAT_METADATA_CSUM = 0x400
};

struct DiskGroupDesc {
	uint32_t blockBitmap;
	uint32_t inodeBitmap;
	uint32_t inodeTable;
	uint16_t freeBlocksCount;
	uint16_t freeInodesCount;
	uint16_t usedDirsCount;
	uint16_t flags;
	uint32_t excludeBitmapLow;
	uint16_t blockBitmapCsumLow;
	uint16_t inodeBitmapCsumLow;
	uint16_t itableUnusedLow;
	uint16_t checksum;
	//-- Valid only with EXT4_INCOMPAT_64BIT and desc size > 32 --
	uint32_t blockBitmapHigh;
	uint32_t inodeBitmapHigh;
	uint32_t inodeTableHigh;
	uint16_t freeBlocksCountHigh;
	uint16_t freeInodesCountHigh;
	uint16_t usedDirsCountHigh;
	uint16_t itableUnusedHigh;
	uint32_t excludeBitmapHigh;
	uint16_t blockBitmapCsumHigh;
	uint16_t inodeBitmapCsumHigh;
	uint32_t unused;
};
static_assert(sizeof(DiskGroupDesc) == 64, "Bad DiskGroupDesc struct size");

struct DiskInode {
	uint16_t mode;
	uint16_t uid;
	uint32_t size;
	uint32_t atime;
	uint32_t ctime;
	uint32_t mtime;
	uint32_t dtime;
	uint16_t gid;
	uint16_t linksCount;
	uint32_t blocks;
	uint32_t flags;
	uint32_t osdl;
	FileData data;
	uint32_t generation;
	uint32_t fileAcl;
	uint32_t dirAcl;
	uint32_t faddr;
	union {
		uint8_t data[12];

		struct {
			uint16_t blocksHigh;
			uint16_t fileAclHigh;
			uint16_t uidHigh;
			uint16_t gidHigh;
			uint16_t checksumLow;
			uint16_t reserved;
		};
	} osd2;
	uint16_t extraSize;
	uint16_t checksumHigh;
	uint32_t ctimeExtra;
	uint32_t mtimeExtra;
	uint32_t atimeExtra;
	uint32_t crtime;
	uint32_t crtimeExtra;
	uint32_t versionHigh;
	uint32_t projid;
};
static_assert(sizeof(DiskInode) == 160, "Bad DiskInode struct size");

enum {
	EXT2_ROOT_INO = 2
};

enum {
	EXT2_S_IFMT = 0xF000,
	EXT2_S_IFLNK = 0xA000,
	EXT2_S_IFREG = 0x8000,
	EXT2_S_IFDIR = 0x4000
};

enum {
	EXT4_INDEX_FL = 0x1000,
	EXT4_EXTENTS_FL = 0x80000,
	EXT4_INLINE_DATA_FL = 0x10000000
};

struct DiskDirEntry {
	uint32_t inode;
	uint16_t recordLength;
	uint8_t nameLength;
	uint8_t fileType;
	char name[];
};

enum {
	EXT2_FT_REG_FILE = 1,
	EXT2_FT_DIR = 2,
	EXT2_FT_SYMLINK = 7
};

// --------------------------------------------------------
// DirEntry
// --------------------------------------------------------

struct DirEntry {
	uint32_t inode;
	FileType fileType;
};

// --------------------------------------------------------
// ExtentBlockRange
// --------------------------------------------------------

struct ExtentBlockRange {
	uint64_t relativeStartBlock;
	uint64_t absoluteStartBlock;
	uint64_t size;
	bool found;
};

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

struct FileSystem;

struct Inode final : BaseInode, std::enable_shared_from_this<Inode> {
	Inode(FileSystem &fs, uint32_t number);

	DiskInode *diskInode();

	// Returns the size of the file in bytes.
	// Callers must hold inodeMutex (shared).
	uint64_t fileSize() {
		return diskInode()->size;
	}

	// Callers must hold inodeMutex (exclusive).
	void setFileSize(uint64_t size);

	// Callers must hold inodeMutex (shared).
	async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
	findEntry(std::string name);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive), plus the target inode's inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error, DirEntry>> insertEntry(std::string name, int64_t ino, blockfs::FileType type);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive), plus the target inode's inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>> removeEntry(std::string name);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (shared).
	async::result<std::expected<bool, protocols::fs::Error>> isDirectoryEmpty();

	// Repoints the ".." entry of this directory at a new parent inode.
	// Callers must hold topologyMutex (exclusive).
	// Callers must hold inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>> updateDotDot(uint32_t parent);

	// Returns whether this directory is `ino` itself or one of its descendants,
	// found by walking the ".." chain up to the filesystem root.
	// Callers must hold topologyMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error, bool>> isSubdirectoryOf(uint32_t ino);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive), plus the target inode's inodeMutex (exclusive).
	async::result<std::expected<DirEntry, protocols::fs::Error>> link(std::string name, int64_t ino, blockfs::FileType type);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive).
	async::result<std::expected<DirEntry, protocols::fs::Error>> mkdir(std::string name, uid_t uid, gid_t gid, mode_t mode);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive).
	async::result<std::expected<DirEntry, protocols::fs::Error>> symlink(std::string name, std::string target);

	// Callers must hold inodeMutex (exclusive).
	async::result<protocols::fs::Error> chmod(int mode);

	// Callers must hold inodeMutex (exclusive).
	async::result<protocols::fs::Error> chown(std::optional<uid_t> uid, std::optional<gid_t> gid);

	// Callers must hold inodeMutex (exclusive).
	async::result<protocols::fs::Error> updateTimes(
		std::optional<timespec> atime,
		std::optional<timespec> mtime,
		std::optional<timespec> ctime);

	FileSystem &fs;

	helix::UniqueDescriptor diskLock;

	// Serializes access to this inode's mapping from file offsets to filesystem blocks.
	// blockMapMutex MUST be taken for all operations that access:
	// - the direct pointers in the inode
	// - the single/double/triple indirect blocks
	// Mutations of the block map require the mutex exclusively; read-only lookups
	// (and the I/O that they drive) only require it shared.
	// Ordered after inodeMutex.
	async::shared_mutex blockMapMutex;

	// page cache that stores the contents of this file
	HelHandle backingMemory;
	HelHandle frontalMemory;
	helix::Mapping fileMapping;

	helix::BorrowedDescriptor accessMemory() {
		return helix::BorrowedDescriptor{frontalMemory};
	}

	// Callers must hold inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>>
	ensureBackingBlocks(size_t offset, size_t length);

	// Callers must hold inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>>
	resizeFile(size_t newSize);

	bool usesExtents;
};

// --------------------------------------------------------
// BlockGroupDescriptorTable
// --------------------------------------------------------

struct BlockGroupDescriptorTable {
	inline void init(std::byte *ptr, uint16_t descriptorSize) {
		ptr_ = ptr;
		descriptorSize_ = descriptorSize;
	}

	inline DiskGroupDesc& operator[](size_t index) {
		return *reinterpret_cast<DiskGroupDesc *>(ptr_ + index * descriptorSize_);
	}

	uint16_t descriptorSize() const {
		return descriptorSize_;
	}

private:
	std::byte *ptr_;
	uint16_t descriptorSize_;
};

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

struct OpenFile;

struct FileSystem final : BaseFileSystem {
	using Inode = Inode;
	using File = OpenFile;
	using DirEntry = DirEntry;

	FileSystem(BlockDevice *device);

	const protocols::fs::FileOperations *fileOps() override;
	const protocols::fs::NodeOperations *nodeOps() override;

	async::result<void> init();

	async::recurring_event bdgtWriteback;
	async::detached handleBgdtWriteback();

	async::detached manageBlockBitmap(helix::UniqueDescriptor memory);
	async::detached manageInodeBitmap(helix::UniqueDescriptor memory);
	async::detached manageInodeTable(helix::UniqueDescriptor memory);

	async::detached serviceBlockBitmapRequest(helix::BorrowedDescriptor memory,
			int type, uintptr_t offset, size_t length);
	async::detached serviceInodeBitmapRequest(helix::BorrowedDescriptor memory,
			int type, uintptr_t offset, size_t length);
	async::detached serviceInodeTableRequest(helix::BorrowedDescriptor memory,
			int type, uintptr_t offset, size_t length);

	std::shared_ptr<BaseInode> accessRoot() override;
	std::shared_ptr<BaseInode> accessInode(uint32_t number) override;
	async::result<std::shared_ptr<BaseInode>> createRegular(int uid, int gid, uint32_t parentIno) override;
	protocols::fs::FsStats getFsStats() override;

	async::result<std::shared_ptr<Inode>> createDirectory();
	async::result<std::shared_ptr<Inode>> createSymlink();

	async::detached initiateInode(std::shared_ptr<Inode> inode);
	async::detached manageFileData(std::shared_ptr<Inode> inode);
	async::detached serviceFileDataRequest(std::shared_ptr<Inode> inode,
			int type, uintptr_t offset, size_t length);

	// Allocate up to num blocks for the given inode.
	// This function does not write back the BGDT, this is the caller's responsibility.
	async::result<std::vector<uint32_t>> allocateBlocks(size_t num, std::optional<uint32_t> ino = std::nullopt);
	async::result<uint32_t> allocateInode(uint32_t parentIno = 0, bool directory = false);

	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<void> assignDataBlocks(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Returns whether all blocks of the given range are present in the block map.
	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<bool> dataBlocksMapped(std::shared_ptr<Inode> inode,
			uint64_t block_offset, size_t num_blocks);

	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<void>
	readDataBlocks(std::shared_ptr<Inode> inode, uint64_t block_offset, arch::dma_buffer_view buf);

	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<void> writeDataBlocks(
	    std::shared_ptr<Inode> inode, uint64_t block_offset, arch::dma_buffer_view view
	);


	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<std::vector<ExtentBlockRange>> lookupBlocksUsingExtent(Inode *inode,
			uint64_t block_offset, size_t num_blocks, bool errorIfNotFound);

	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<void> assignDataBlocksUsingExtents(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<void> readDataBlocksUsingExtents(std::shared_ptr<Inode> inode, uint64_t block_offset,
			arch::dma_buffer_view buf);
	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<void> writeDataBlocksUsingExtents(std::shared_ptr<Inode> inode, uint64_t block_offset,
			arch::dma_buffer_view buf);

	BlockDevice *device;
	uint16_t inodeSize;
	uint32_t blockShift;
	uint32_t blockSize;
	uint32_t blockPagesShift;
	uint32_t sectorsPerBlock;
	uint32_t numBlockGroups;
	uint32_t blocksPerGroup;
	uint32_t inodesPerGroup;
	uint32_t blocksCount;
	uint32_t inodesCount;
	uint8_t uuid[16];

	// Mutable fields are protected by allocationMutex. These are:
	// - freeBlocksCount
	// - freeInodesCount
	// - usedDirsCount
	// - checksum
	// - blockBitmapCsumLow
	// - inodeBitmapCsumLow
	// - blockBitmapCsumHigh
	// - inodeBitmapCsumHigh
	// All other fields are immutable.
	arch::dma_buffer blockGroupDescriptorBuffer;
	// View of blockGroupDescriptorBuffer; same locking as above.
	BlockGroupDescriptorTable bgdt;

	uint32_t metadataChecksumSeed;
	bool is64Bit;
	bool usesExtents;
	bool metadataChecksum;
	bool bgdtChecksum;

	helix::UniqueDescriptor blockBitmap;
	// Immutable handle. Access protected by allocationMutex.
	// Only locked and present pages must be accessed.
	helix::Mapping blockBitmapMapping;
	helix::UniqueDescriptor inodeBitmap;
	// Immutable handle. Access protected by allocationMutex.
	// Only locked and present pages must be accessed.
	helix::Mapping inodeBitmapMapping;
	helix::UniqueDescriptor inodeTable;
	helix::Mapping inodeTableMapping;

	// Mount-wide cache of metadata blocks (i.e., indirect blocks), indexed by disk block number.
	std::optional<MetadataCache> metadataCache;

	std::mutex activeInodesMutex;

	// Serializes block/inode allocation and BGDT modifications.
	async::mutex allocationMutex;

	// Protected by activeInodesMutex.
	std::unordered_map<uint32_t, std::weak_ptr<Inode>> activeInodes;

	arch::contiguous_pool *pool;
};

// --------------------------------------------------------
// File operation closures
// --------------------------------------------------------

struct OpenFile final : BaseFile {
	OpenFile(std::shared_ptr<Inode> inode, bool write, bool read, bool append)
	: BaseFile{inode, write, read, append} { }

	// Callers must hold BaseFile::mutex.
	// Callers must hold the inode's inodeMutex (shared).
	async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries();
};

static_assert(blockfs::Inode<Inode>);
static_assert(blockfs::File<OpenFile>);
static_assert(blockfs::FileSystem<FileSystem>);

} } // namespace blockfs::ext2fs

