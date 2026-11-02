#include <cstring>
#include <cstdio>
#include <inttypes.h>
#include <unistd.h>

#include <helix/memory.hpp>

#include "controller.hpp"
#include "command.hpp"

Command::Command(Controller *controller, uint64_t sector, size_t numSectors, arch::dma_buffer_view view,
		CommandType type) : controller_{controller}, sector_{sector}, numSectors_{numSectors}, numBytes_{view.size()},
	view_{view}, type_{type}, event_{} {

	// The I/O scheduler splits requests to the limit that Port declares; the head may be unaligned.
	assert(numBytes_ <= (commandTable::prdtEntries - 1) * 0x1000);

	if (logCommands) {
		printf("block/ahci: queueing %zu byte %s to %p at sector %" PRIu64 "\n",
			numBytes_, cmdTypeToString(type_),
			view.byte_data(), sector);
	}
}

void Command::notifyCompletion() {
	if (logCommands) {
		printf("block/ahci: completed %s to %p\n", cmdTypeToString(type_), view_.byte_data());
	}

	event_.raise();
}

async::result<void> Command::prepare(arch::dma_object_view<commandTable> table, commandHeader& header) {
	co_await controller_->dmaSpace().ensure_mapped(table);
	auto tablePhys = controller_->dmaSpace().iova_of(table);
	assert((tablePhys & 0x7F) == 0);
	assert(numSectors_ < std::numeric_limits<uint16_t>::max());

	memset(table.data(), 0, sizeof(commandTable));
	table->commandFis.fisType = 0x27; // Host to Device FIS
	table->commandFis.info = 1 << 7; // Use command register, not control register
	table->commandFis.command = 0; // Set below
	table->commandFis.devHead = 1 << 6; // LBA bit
	table->commandFis.lba0 = sector_ & 0xFF;
	table->commandFis.lba1 = (sector_ >> 8) & 0xFF;
	table->commandFis.lba2 = (sector_ >> 16) & 0xFF;
	table->commandFis.lba3 = (sector_ >> 24) & 0xFF;
	table->commandFis.lba4 = (sector_ >> 32) & 0xFF;
	table->commandFis.lba5 = (sector_ >> 40) & 0xFF;
	table->commandFis.sectorCount = static_cast<uint16_t>(numSectors_);

	auto numEntries = co_await writeScatterGather_(table);

	memset(&header, 0, sizeof(commandHeader));
	header.configBytes[0] = sizeof(fisH2D) / 4; // Supply length in dwords
	header.configBytes[1] = 0;
	header.prdtLength = numEntries;
	header.prdByteCount = 0;
	header.ctBase = static_cast<uint32_t>(tablePhys);
	header.ctBaseUpper = 0;

	switch (type_) {
		case CommandType::read:
			table->commandFis.command = 0x25; // READ DMA EXT
			break;
		case CommandType::write:
			table->commandFis.command = 0x35; // WRITE DMA EXT
			header.configBytes[0] |= 1 << 6; // Indicates we are writing
			break;
		case CommandType::identify:
			table->commandFis.command = 0xEC; // IDENTIFY DEVICE
			break;
		case CommandType::flush:
			table->commandFis.command = 0xE7; // FLUSH CACHE (non-data)
			break;
		case CommandType::flushExt:
			table->commandFis.command = 0xEA; // FLUSH CACHE EXT (non-data)
			break;
		default:
			assert(!"unknown command type");
	}

	if (logCommands) {
		printf("block/ahci: submitting %zu byte %s to %p at sector %" PRIu64 "\n",
				numBytes_, cmdTypeToString(type_), view_.byte_data(), sector_);
	}
}

/* Returns the number of PRDT entries written.
 *
 * Note on buffer_: libblockfs guarantees us that buffer_ is locked into memory,
 * and calling helPointerPhysical ensures that the pages are allocated and present
 * in the page tables. Hence, we know the buffer remains in memory during the DMA.
 */
async::result<size_t> Command::writeScatterGather_(arch::dma_object_view<commandTable> table) {
	if (!numBytes_)
		co_return 0;

	// TODO: Grab the page size for each individual address
	size_t pageSize = getpagesize();

	if(numBytes_)
		co_await controller_->dmaSpace().ensure_mapped(view_);

	size_t prdtIndex = 0;
	auto addEntry = [&](arch::dma_buffer_view view) {
		assert(view.size() <= pageSize);
		uintptr_t phys = controller_->dmaSpace().iova_of(view);

		assert(prdtIndex < commandTable::prdtEntries && !(phys & 1));

		uint32_t upperPhys = phys >> 32;
		table->prdts[prdtIndex++] = prdtEntry {
			static_cast<uint32_t>(phys),
			upperPhys,
			0,
			static_cast<uint32_t>(view.size()) - 1,
		};
	};

	uintptr_t virtStart = reinterpret_cast<uintptr_t>(view_.data());
	size_t progress = 0;

	// As virtStart may not be aligned to pageSize, we split off the initial
	// unaligned part, then work with pageSize aligned chunks.
	if (virtStart % pageSize > 0) {
		auto nextAlignedAddr = (virtStart + pageSize) & ~(pageSize - 1);
		auto bytesUntilAligned = nextAlignedAddr - virtStart;
		auto bytesToWrite = std::min(numBytes_, bytesUntilAligned);

		addEntry(view_.subview(0, bytesToWrite));
		progress += bytesToWrite;
	}

	// Insert every page in the buffer into the scatter-gather list.
	for (; progress < numBytes_; progress += pageSize) {
		auto subview = view_.subview(progress, std::min(pageSize, numBytes_ - progress));

		// TODO: As a small optimisation, we could accumulate into the previous entry if they
		// happen to be physically contiguous.
		addEntry(subview);
	}

	co_return prdtIndex;
}
