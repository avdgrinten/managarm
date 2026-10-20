#pragma once

#include <protocols/ostrace/ostrace.hpp>

namespace blockfs {

inline constinit protocols::ostrace::Event ostEvtGetLink{"libblockfs.getLink"};
inline constinit protocols::ostrace::Event ostEvtTraverseLinks{"libblockfs.traverseLinks"};
inline constinit protocols::ostrace::Event ostEvtRead{"libblockfs.read"};
inline constinit protocols::ostrace::Event ostEvtReadDir{"libblockfs.readDir"};
inline constinit protocols::ostrace::Event ostEvtWrite{"libblockfs.write"};
inline constinit protocols::ostrace::Event ostEvtRawRead{"libblockfs.rawRead"};
inline constinit protocols::ostrace::Event ostEvtRawWrite{"libblockfs.rawWrite"};
inline constinit protocols::ostrace::Event ostEvtOpen{"libblockfs.open"};
inline constinit protocols::ostrace::Event ostEvtTruncate{"libblockfs.truncate"};
inline constinit protocols::ostrace::Event ostEvtRename{"libblockfs.rename"};
inline constinit protocols::ostrace::Event ostEvtMetadataInitialize{"libblockfs.metadataInitialize"};
inline constinit protocols::ostrace::Event ostEvtMetadataWriteback{"libblockfs.metadataWriteback"};
inline constinit protocols::ostrace::Event ostEvtMetadataAccess{"libblockfs.metadataAccess"};
inline constinit protocols::ostrace::Event ostEvtMetadataUnmap{"libblockfs.metadataUnmap"};
inline constinit protocols::ostrace::Event ostEvtMetadataRead{"libblockfs.metadataRead"};
inline constinit protocols::ostrace::Event ostEvtExt2Mount{"ext2.mount"};
inline constinit protocols::ostrace::Event ostEvtExt2InitiateInode{"ext2.initiateInode"};
inline constinit protocols::ostrace::Event ostEvtExt2GetStats{"ext2.getStats"};
inline constinit protocols::ostrace::Event ostEvtExt2GetLinkOrCreate{"ext2.getLinkOrCreate"};
inline constinit protocols::ostrace::Event ostEvtExt2Mkdir{"ext2.mkdir"};
inline constinit protocols::ostrace::Event ostEvtExt2Symlink{"ext2.symlink"};
inline constinit protocols::ostrace::Event ostEvtExt2Unlink{"ext2.unlink"};
inline constinit protocols::ostrace::Event ostEvtExt2Rmdir{"ext2.rmdir"};
inline constinit protocols::ostrace::Event ostEvtExt2FindEntry{"ext2.findEntry"};
inline constinit protocols::ostrace::Event ostEvtExt2InsertEntry{"ext2.insertEntry"};
inline constinit protocols::ostrace::Event ostEvtExt2RemoveEntry{"ext2.removeEntry"};
inline constinit protocols::ostrace::Event ostEvtExt2IsDirectoryEmpty{"ext2.isDirectoryEmpty"};
inline constinit protocols::ostrace::Event ostEvtExt2UpdateDotDot{"ext2.updateDotDot"};
inline constinit protocols::ostrace::Event ostEvtExt2AssignDataBlocks{"ext2.assignDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2FreeDataBlocks{"ext2.freeDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ReadDataBlocks{"ext2.readDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2WriteDataBlocks{"ext2.writeDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ResizeFile{"ext2.resizeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2InitializeFile{"ext2.initializeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2WritebackFile{"ext2.writebackFile"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateBlocks{"ext2.allocateBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2FreeBlocks{"ext2.freeBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateInode{"ext2.allocateInode"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkReadSectors{"virtio-blk.readSectors"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkWriteSectors{"virtio-blk.writeSectors"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkRequest{"virtio-blk.request"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkFlush{"virtio-blk.flush"};

// Attributes. Every time* attribute is in nanoseconds and measures a phase of the event it is attached to.
// The phases of one event do not overlap, so they can be stacked against `time`.
//
// Wall-clock duration of the whole traced operation, in nanoseconds.
// All other time* attributes are disjoint sub-intervals of it.
inline constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
// Bytes actually moved or examined by the operation (not the size requested).
inline constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};
// Bytes the caller asked for; exceeds numBytes on a short read at EOF.
inline constinit protocols::ostrace::UintAttribute ostAttrNumRequested{"numRequested"};
// File size before a resize. Absent where the operation only knows the target size.
inline constinit protocols::ostrace::UintAttribute ostAttrOldSize{"oldSize"};
// File size that the operation resizes to. Equal to oldSize if the resize was a no-op.
inline constinit protocols::ostrace::UintAttribute ostAttrNewSize{"newSize"};
// Size of the file that the operation walked; its pin and map costs scale with this.
inline constinit protocols::ostrace::UintAttribute ostAttrFileSize{"fileSize"};
// Filesystem block number the operation acted on.
inline constinit protocols::ostrace::UintAttribute ostAttrBlock{"block"};
// Number of filesystem blocks allocated, assigned or freed.
inline constinit protocols::ostrace::UintAttribute ostAttrNumBlocks{"numBlocks"};
// Block groups whose bitmap was consulted before the search succeeded.
inline constinit protocols::ostrace::UintAttribute ostAttrNumGroups{"numGroups"};
// Block groups that the filesystem consists of.
inline constinit protocols::ostrace::UintAttribute ostAttrNumBlockGroups{"numBlockGroups"};
// Path components the caller asked to resolve, and how many of them were resolved.
inline constinit protocols::ostrace::UintAttribute ostAttrNumComponents{"numComponents"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumResolved{"numResolved"};
// Inode number the operation acted on; 0 if allocation failed.
inline constinit protocols::ostrace::UintAttribute ostAttrIno{"ino"};
// Whether the looked-up name or block was already present (1) or not (0).
inline constinit protocols::ostrace::UintAttribute ostAttrFound{"found"};
// Whether the access was for writing (1) or reading (0).
inline constinit protocols::ostrace::UintAttribute ostAttrIsWrite{"isWrite"};
// Index of the device queue that the request was submitted to.
inline constinit protocols::ostrace::UintAttribute ostAttrQueueIndex{"queueIndex"};
// Waiting to acquire mutexes. Excludes page pinning, which is timePin.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLock{"timeLock"};
// Pinning pages into memory via LockMemoryView, i.e. faulting them in if absent.
inline constinit protocols::ostrace::UintAttribute ostAttrTimePin{"timePin"};
// Importing the managed memory range into the DMA pool.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeImport{"timeImport"};
// Waiting for servicing budget, i.e. for the memory that in-flight manage requests
// pin to drop back below the per-thread limit.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeBudget{"timeBudget"};
// Resolving the file range to runs of disk blocks, i.e. walking the block map.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLookup{"timeLookup"};
// Reading file data blocks off the device.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeRead{"timeRead"};
// Assigning disk blocks to the file range that is being written back.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeAssign{"timeAssign"};
// Writing file data blocks to the device.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeWrite{"timeWrite"};
// Time spent inside device read/writeSectors calls, summed over all of them.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeDevice{"timeDevice"};
// Obtaining MetadataCache windows, summed over all accesses in the operation.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeAccess{"timeAccess"};
// Resizing the page cache backing the file.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeResizeMemory{"timeResizeMemory"};
// Releasing the disk blocks past the new end of a shrunk file.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeFreeBlocks{"timeFreeBlocks"};
// Waiting for readyEvent, i.e. for the inode to finish being initiated.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeReady{"timeReady"};
// Growing the file so that the write fits.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeResize{"timeResize"};
// Copying payload between the caller buffer and the page cache. Includes the
// page faults that this triggers, and hence the I/O to service them.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCopy{"timeCopy"};
// Building the request descriptors before handing them to the device. Excludes timeObtain.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeSetup{"timeSetup"};
// Obtaining descriptors from the virtqueue, i.e. waiting for a free slot.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeObtain{"timeObtain"};
// Walking the directory entries in the page cache. Free of I/O, but a scan through a mapping
// that was just established also faults in its page table entries.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeScan{"timeScan"};
// Establishing the address-space mapping of an already pinned range.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeMap{"timeMap"};
// Extending a directory by one block when no entry slot was free.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeGrow{"timeGrow"};
// Clearing the dirty page table entries of the whole directory file after modifying an entry.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCleanDir{"timeCleanDir"};
// Looking up the name before deciding whether to create it.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeFind{"timeFind"};
// Allocating and initializing a new inode, including its mode update.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeCreate{"timeCreate"};
// Inserting the directory entry that publishes an inode under a name.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLink{"timeLink"};
// Walking .. links to reject a rename that would create a directory cycle.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeLoopCheck{"timeLoopCheck"};
// Removing directory entries, summed over both parents in a rename.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeRemove{"timeRemove"};
// Reading the superblock off the device.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeSuperblock{"timeSuperblock"};
// Reading the block group descriptor table off the device.
inline constinit protocols::ostrace::UintAttribute ostAttrTimeBgdt{"timeBgdt"};

inline protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtGetLink,
	ostEvtTraverseLinks,
	ostEvtRead,
	ostEvtReadDir,
	ostEvtWrite,
	ostEvtRawRead,
	ostEvtRawWrite,
	ostEvtOpen,
	ostEvtTruncate,
	ostEvtRename,
	ostEvtMetadataInitialize,
	ostEvtMetadataWriteback,
	ostEvtMetadataAccess,
	ostEvtMetadataUnmap,
	ostEvtMetadataRead,
	ostEvtExt2Mount,
	ostEvtExt2InitiateInode,
	ostEvtExt2GetStats,
	ostEvtExt2GetLinkOrCreate,
	ostEvtExt2Mkdir,
	ostEvtExt2Symlink,
	ostEvtExt2Unlink,
	ostEvtExt2Rmdir,
	ostEvtExt2FindEntry,
	ostEvtExt2InsertEntry,
	ostEvtExt2RemoveEntry,
	ostEvtExt2IsDirectoryEmpty,
	ostEvtExt2UpdateDotDot,
	ostEvtExt2AssignDataBlocks,
	ostEvtExt2FreeDataBlocks,
	ostEvtExt2ReadDataBlocks,
	ostEvtExt2WriteDataBlocks,
	ostEvtExt2ResizeFile,
	ostEvtExt2InitializeFile,
	ostEvtExt2WritebackFile,
	ostEvtExt2AllocateBlocks,
	ostEvtExt2FreeBlocks,
	ostEvtExt2AllocateInode,
	ostEvtVirtioBlkReadSectors,
	ostEvtVirtioBlkWriteSectors,
	ostEvtVirtioBlkRequest,
	ostEvtVirtioBlkFlush,
	ostAttrTime,
	ostAttrNumBytes,
	ostAttrNumRequested,
	ostAttrOldSize,
	ostAttrNewSize,
	ostAttrFileSize,
	ostAttrBlock,
	ostAttrNumBlocks,
	ostAttrNumGroups,
	ostAttrNumBlockGroups,
	ostAttrNumComponents,
	ostAttrNumResolved,
	ostAttrIno,
	ostAttrFound,
	ostAttrIsWrite,
	ostAttrQueueIndex,
	ostAttrTimeLock,
	ostAttrTimePin,
	ostAttrTimeImport,
	ostAttrTimeBudget,
	ostAttrTimeLookup,
	ostAttrTimeRead,
	ostAttrTimeAssign,
	ostAttrTimeWrite,
	ostAttrTimeDevice,
	ostAttrTimeAccess,
	ostAttrTimeResizeMemory,
	ostAttrTimeFreeBlocks,
	ostAttrTimeReady,
	ostAttrTimeResize,
	ostAttrTimeCopy,
	ostAttrTimeSetup,
	ostAttrTimeObtain,
	ostAttrTimeScan,
	ostAttrTimeMap,
	ostAttrTimeGrow,
	ostAttrTimeCleanDir,
	ostAttrTimeFind,
	ostAttrTimeCreate,
	ostAttrTimeLink,
	ostAttrTimeLoopCheck,
	ostAttrTimeRemove,
	ostAttrTimeSuperblock,
	ostAttrTimeBgdt,
};

inline protocols::ostrace::Context ostContext{ostVocabulary};

} // namespace blockfs
