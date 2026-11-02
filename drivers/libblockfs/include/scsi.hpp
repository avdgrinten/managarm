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
	// READ(10)/WRITE(10) carry a 16-bit transfer length, so limits.maxSectorsPerRequest must be set.
	StorageDevice(size_t sectorSize, int64_t parentId, arch::contiguous_pool *pool, Limits limits)
	: blockfs::BlockDevice(sectorSize, parentId, pool, limits) {
		assert(limits.maxSectorsPerRequest && *limits.maxSectorsPerRequest <= 0xffff);
	}

	async::result<std::expected<void, blockfs::Error>> flush() final;

	async::result<size_t> getSize() final;

	size_t storageSize{};

protected:
	async::result<std::expected<void, blockfs::Error>> issueRead(unsigned int, uint64_t sector,
			arch::dma_buffer_view view) final;

	async::result<std::expected<void, blockfs::Error>> issueWrite(unsigned int, uint64_t sector,
			arch::dma_buffer_view view) final;

private:
	async::result<std::expected<void, blockfs::Error>> performIo(bool isWrite, uint64_t sector,
			arch::dma_buffer_view view);
};

inline constexpr uint8_t WELL_KNOWN_REPORT_LUNS_LUN = 1;

} // namespace scsi
