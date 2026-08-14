#include <bit>
#include <string.h>

#include "metadata-cache.hpp"

namespace blockfs {

namespace {
	constexpr uint32_t pageShift = 12;
}

MetadataCache::MetadataCache(BlockDevice *device, uint64_t numBlocks, size_t blockSize)
: device_{device}, numBlocks_{numBlocks}, blockSize_{blockSize} {
	assert(std::has_single_bit(blockSize));
	assert(blockSize >= device->sectorSize);
	auto blockShift = static_cast<uint32_t>(std::countr_zero(blockSize));
	blockPagesShift_ = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock_ = blockSize / device->sectorSize;

	HelHandle backing, frontal;
	HEL_CHECK(helCreateManagedMemory(numBlocks << blockPagesShift_,
			0, &backing, &frontal));
	frontal_ = helix::UniqueDescriptor{frontal};

	manage_(helix::UniqueDescriptor{backing});
}

async::result<MetadataCache::BlockWindow> MetadataCache::access(uint64_t block, bool writable) {
	assert(block < numBlocks_);
	auto frameSize = size_t{1} << blockPagesShift_;

	helix::LockMemoryView lockMemory;
	auto &&submit = helix::submitLockMemoryView(frontal_, &lockMemory,
			block << blockPagesShift_, frameSize, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lockMemory.error());

	uint32_t flags = kHelMapProtRead | kHelMapDontRequireBacking;
	if(writable)
		flags |= kHelMapProtWrite;
	helix::Mapping mapping{frontal_, static_cast<ptrdiff_t>(block << blockPagesShift_),
			frameSize, flags};

	co_return BlockWindow{lockMemory.descriptor(), std::move(mapping)};
}

async::result<void> MetadataCache::read(uint64_t block, size_t offset, size_t length, void *buffer) {
	assert(block < numBlocks_);
	assert(offset + length <= blockSize_);

	auto readMemory = co_await helix_ng::readMemory(frontal_,
			(block << blockPagesShift_) + offset, length, buffer);
	HEL_CHECK(readMemory.error());
}

async::detached MetadataCache::manage_(helix::UniqueDescriptor backing) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submitManage = helix::submitManageMemory(backing,
				&manage, helix::Dispatcher::global());
		co_await submitManage.async_wait();
		HEL_CHECK(manage.error());

		serviceRequest_(backing, manage.type(), manage.offset(), manage.length());
	}
}

async::detached MetadataCache::serviceRequest_(helix::BorrowedDescriptor backing,
		int type, uintptr_t offset, size_t length) {
	auto frameSize = size_t{1} << blockPagesShift_;
	assert(!(offset & (frameSize - 1)));
	assert(!(length & (frameSize - 1)));

	auto view = device_->pagePool->importMemory(backing, offset, length);

	for(size_t progress = 0; progress < length; progress += frameSize) {
		auto block = (offset + progress) >> blockPagesShift_;
		assert(block < numBlocks_);

		auto subview = view.view().subview(progress, blockSize_);

		if(type == kHelManageInitialize) {
			co_await device_->readSectors(block * sectorsPerBlock_, subview);
			// Zero the tail of the frame that no disk block backs.
			if(blockSize_ < frameSize)
				memset(view.view().subview(progress + blockSize_,
						frameSize - blockSize_).data(), 0, frameSize - blockSize_);
		}else{
			assert(type == kHelManageWriteback);
			co_await device_->writeSectors(block * sectorsPerBlock_, subview);
		}
	}

	HEL_CHECK(helUpdateMemory(backing.getHandle(), type, offset, length));
}

} // namespace blockfs
