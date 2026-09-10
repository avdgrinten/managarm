#pragma once

#include <blockfs.hpp>

#include <arch/dma_structs.hpp>

#include <string>

namespace scsi {

enum class ErrorType {
	success,
	checkCondition,
	conditionMet,
	busy,
	reservationConflict,
	taskSetFull,
	acaActive,
	taskAborted,
	deviceSpecific
};

struct Error {
	constexpr bool operator==(const Error &other) const = default;

	std::string toString();

	ErrorType type;
	uint32_t code;
};

struct CommandInfo {
	arch::dma_buffer_view command;
	arch::dma_buffer_view data;
	bool isWrite;
};

Error statusToError(uint8_t status);

struct Interface {
	virtual ~Interface() = default;

	// Implementations must accept concurrent calls, serializing or pipelining them as
	// the transport allows.
	virtual async::result<frg::expected<Error, size_t>> sendScsiCommand(const CommandInfo &info) = 0;

	async::result<frg::expected<Error, std::vector<uint64_t>>> reportLuns();

	bool enableRead6{};
};

struct StorageDevice : Interface, blockfs::BlockDevice {
	StorageDevice(size_t sectorSize, int64_t parentId, arch::contiguous_pool *pool)
	: blockfs::BlockDevice(sectorSize, parentId, pool) { }

	async::result<void> readSectors(uint64_t sector,
			arch::dma_buffer_view view) final;

	async::result<void> writeSectors(uint64_t sector,
			arch::dma_buffer_view view) final;

	async::result<size_t> getSize() final;

	size_t storageSize{};

	// Larger requests are split into multiple commands. Transports may lower this.
	size_t maxSectorsPerCommand{0xffff};

private:
	async::result<void> performSplitIo(bool isWrite, uint64_t sector,
			arch::dma_buffer_view view);

	async::result<void> performIo(bool isWrite, uint64_t sector,
			arch::dma_buffer_view view);
};

inline constexpr uint8_t WELL_KNOWN_REPORT_LUNS_LUN = 1;

} // namespace scsi
