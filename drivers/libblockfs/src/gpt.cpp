
#include <stdlib.h>
#include <format>
#include <print>
#include <array>

#include "gpt.hpp"

namespace blockfs {
namespace gpt {

std::string Guid::toString() const {
	return std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
			a, b, c, d[0], d[1], e[0], e[1], e[2], e[3], e[4], e[5]);
}

namespace {
	constexpr std::array<size_t, 4> commonSectorSizes{512, 2048, 4096, 8192};

	constexpr size_t gptSectorNumber = 1;

	// Standard tables take 16 KiB; this keeps a corrupt header from sizing a huge DMA buffer.
	constexpr uint64_t maxTableSize = 1024 * 1024;
}

// --------------------------------------------------------
// Table
// --------------------------------------------------------

Table::Table(BlockDevice *device)
: device_(device), gptSectorSize_{device->sectorSize} { }

async::result<std::expected<std::pair<arch::dma_buffer, DiskHeader *>, Error>>
Table::probeSectorSize_(size_t sectorSize) {
	size_t deviceGptSector = (sectorSize * gptSectorNumber) / getDevice()->sectorSize;
	size_t deviceGptOffset = (sectorSize * gptSectorNumber) % getDevice()->sectorSize;

	size_t deviceSectors = (sectorSize + deviceGptOffset + getDevice()->sectorSize - 1) / getDevice()->sectorSize;

	arch::dma_buffer headerBuffer{getDevice()->pagePool, deviceSectors * getDevice()->sectorSize};
	auto headerRes = co_await getDevice()->readSectors(deviceGptSector, headerBuffer);
	if (!headerRes)
		co_return std::unexpected{headerRes.error()};

	DiskHeader *header = reinterpret_cast<DiskHeader *>(static_cast<std::byte *>(headerBuffer.data()) + deviceGptOffset);
	if (header->signature == 0x5452415020494645)
		co_return std::make_pair(std::move(headerBuffer), header);

	co_return std::make_pair(arch::dma_buffer{}, nullptr);
}

async::result<std::expected<void, Error>> Table::parse() {
	// probe for the gpt header
	auto probeRes = co_await probeSectorSize_(getDevice()->sectorSize);
	if (!probeRes)
		co_return std::unexpected{probeRes.error()};
	auto header = std::move(probeRes.value());
	if (!header.first.size()) {
		// try other sizes
		for (auto size : commonSectorSizes) {
			if (size == getDevice()->sectorSize)
				continue;

			probeRes = co_await probeSectorSize_(size);
			if (!probeRes)
				co_return std::unexpected{probeRes.error()};
			header = std::move(probeRes.value());
			if (header.first.size()) {
				gptSectorSize_ = size;
				break;
			}
		}

		if(!header.first.size()) {
			std::println(std::cout, "libblockfs: no GPT found, disk has no partitions");
			co_return {};
		}

		std::println(std::cout, "libblockfs: using non-native gpt sector size {}",
			gptSectorSize_);
	}

	uint64_t deviceSize = co_await getDevice()->getSize();
	uint64_t numLbas = deviceSize / gptSectorSize_;

	// Both factors are 32-bit, so the product cannot overflow.
	uint64_t tableSize = uint64_t{header.second->entrySize} * header.second->numEntries;
	if (header.second->entrySize < sizeof(DiskEntry)
			|| header.second->entrySize % sizeof(DiskEntry)
			|| !tableSize || tableSize > maxTableSize
			|| header.second->entryTableLba >= numLbas
			|| header.second->entryTableLba * gptSectorSize_ + tableSize > deviceSize) {
		std::println(std::cout, "libblockfs: Rejecting GPT with an invalid entry table");
		co_return std::unexpected{Error::ioError};
	}

	size_t deviceTableSector = (header.second->entryTableLba * gptSectorSize_) / getDevice()->sectorSize;
	size_t deviceTableOffset = (header.second->entryTableLba * gptSectorSize_) % getDevice()->sectorSize;

	size_t tableSectors = (tableSize + deviceTableOffset + getDevice()->sectorSize - 1) / getDevice()->sectorSize;

	arch::dma_buffer tableBuffer{getDevice()->pagePool, tableSectors * getDevice()->sectorSize};
	auto tableRes = co_await getDevice()->readSectors(deviceTableSector, tableBuffer);
	if (!tableRes)
		co_return std::unexpected{tableRes.error()};

	for (uint32_t i = 0; i < header.second->numEntries; i++) {
		DiskEntry *entry = reinterpret_cast<DiskEntry *>(
		    static_cast<std::byte *>(tableBuffer.data()) + deviceTableOffset
		    + i * header.second->entrySize
		);

		if (entry->typeGuid == type_guids::null)
			continue;

		if (entry->firstLba > entry->lastLba || entry->lastLba >= numLbas) {
			std::println(std::cout, "libblockfs: Skipping GPT entry {}, it is out of bounds", i);
			continue;
		}

		size_t offset = entry->firstLba * gptSectorSize_;
		size_t size = (entry->lastLba - entry->firstLba + 1) * gptSectorSize_;

		size_t deviceSector = offset / getDevice()->sectorSize;
		size_t deviceSectorOffset = offset % getDevice()->sectorSize;

		if (deviceSectorOffset || size % getDevice()->sectorSize) {
			std::println(std::cout, "libblockfs: Skipping GPT entry {}, it is misaligned", i);
			continue;
		}

		auto partition = std::make_unique<Partition>(
		    *this,
		    entry->uniqueGuid,
		    entry->typeGuid,
		    deviceSector,
		    size / getDevice()->sectorSize,
		    this->getDevice()->pagePool
		);
		partitions_.emplace_back(std::move(partition));
	}
	co_return {};
}

BlockDevice *Table::getDevice() {
	return device_;
}

size_t Table::numPartitions() {
	return partitions_.size();
}

Partition &Table::getPartition(int index) {
	return *partitions_[index].get();
}

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

Partition::Partition(
    Table &table,
    Guid id,
    Guid type,
    uint64_t start_lba,
    uint64_t num_sectors,
    arch::contiguous_pool *pool
)
: BlockDevice(table.getDevice()->sectorSize, table.getDevice()->parentId, pool),
  _table(table),
  _id(id),
  _type(type),
  _startLba(start_lba),
  _numSectors(num_sectors) {}

Guid Partition::id() {
	return _id;
}

Guid Partition::type() {
	return _type;
}

uint64_t Partition::startLba() {
	return _startLba;
}

async::result<std::expected<void, Error>> Partition::readSectors(uint64_t sector, arch::dma_buffer_view view) {
	assert(sector + (view.size() / sectorSize) <= _numSectors);
	return _table.getDevice()->readSectors(_startLba + sector, view);
}

async::result<std::expected<void, Error>> Partition::writeSectors(uint64_t sector, arch::dma_buffer_view view) {
	assert(sector + (view.size() / sectorSize) <= _numSectors);
	return _table.getDevice()->writeSectors(_startLba + sector, view);
}

async::result<std::expected<void, Error>> Partition::flush() {
	return _table.getDevice()->flush();
}

async::result<size_t> Partition::getSize() {
	co_return _numSectors * sectorSize;
}

} } // namespace blockfs::gpt
