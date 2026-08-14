#pragma once

#include <protocols/ostrace/ostrace.hpp>

namespace blockfs {

inline bool tracingInitialized = false;

inline constinit protocols::ostrace::Event ostEvtGetLink{"libblockfs.getLink"};
inline constinit protocols::ostrace::Event ostEvtTraverseLinks{"libblockfs.traverseLinks"};
inline constinit protocols::ostrace::Event ostEvtRead{"libblockfs.read"};
inline constinit protocols::ostrace::Event ostEvtReadDir{"libblockfs.readDir"};
inline constinit protocols::ostrace::Event ostEvtWrite{"libblockfs.write"};
inline constinit protocols::ostrace::Event ostEvtRawRead{"libblockfs.rawRead"};
inline constinit protocols::ostrace::Event ostEvtExt2AssignDataBlocks{"ext2.assignDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ManageInode{"ext2.manageInode"};
inline constinit protocols::ostrace::Event ostEvtExt2ManageInodeBitmap{"ext2.manageInodeBitmap"};
inline constinit protocols::ostrace::Event ostEvtExt2InitializeFile{"ext2.initializeFile"};
inline constinit protocols::ostrace::Event ostEvtExt2WritebackFile{"ext2.writebackFile"};
inline constinit protocols::ostrace::Event ostEvtExt2WriteDataBlocks{"ext2.writeDataBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2ManageBlockBitmap{"ext2.manageBlockBitmap"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateBlocks{"ext2.allocateBlocks"};
inline constinit protocols::ostrace::Event ostEvtExt2AllocateInode{"ext2.allocateInode"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkReadSectors{"virtio-blk.readSectors"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkWriteSectors{"virtio-blk.writeSectors"};
inline constinit protocols::ostrace::Event ostEvtVirtioBlkRequest{"virtio-blk.request"};
inline constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
inline constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};
inline constinit protocols::ostrace::UintAttribute ostAttrIsWrite{"isWrite"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeImport{"timeImport"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeMapCheck{"timeMapCheck"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeAssign{"timeAssign"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeWrite{"timeWrite"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeQueue{"timeQueue"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeSetup{"timeSetup"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeObtain{"timeObtain"};
inline constinit protocols::ostrace::UintAttribute ostAttrTimeDevice{"timeDevice"};

inline protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtGetLink,
	ostEvtTraverseLinks,
	ostEvtRead,
	ostEvtReadDir,
	ostEvtWrite,
	ostEvtRawRead,
	ostEvtExt2AssignDataBlocks,
	ostEvtExt2ManageInode,
	ostEvtExt2ManageInodeBitmap,
	ostEvtExt2InitializeFile,
	ostEvtExt2WritebackFile,
	ostEvtExt2WriteDataBlocks,
	ostEvtExt2ManageBlockBitmap,
	ostEvtExt2AllocateBlocks,
	ostEvtExt2AllocateInode,
	ostEvtVirtioBlkReadSectors,
	ostEvtVirtioBlkWriteSectors,
	ostEvtVirtioBlkRequest,
	ostAttrTime,
	ostAttrNumBytes,
	ostAttrIsWrite,
	ostAttrTimeImport,
	ostAttrTimeMapCheck,
	ostAttrTimeAssign,
	ostAttrTimeWrite,
	ostAttrTimeQueue,
	ostAttrTimeSetup,
	ostAttrTimeObtain,
	ostAttrTimeDevice,
};

inline protocols::ostrace::Context ostContext{ostVocabulary};

} // namespace blockfs
