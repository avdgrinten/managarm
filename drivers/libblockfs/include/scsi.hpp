#pragma once

#include <blockfs.hpp>

#include <arch/dma_structs.hpp>

#include <optional>
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
	deviceSpecific,
	transportError
};

enum class SenseKey : uint8_t {
	noSense = 0,
	notReady = 2,
	illegalRequest = 5,
	unitAttention = 6
};

struct SenseData {
	constexpr bool operator==(const SenseData &other) const = default;

	SenseKey key;
	uint8_t asc;
	uint8_t ascq;
};

struct Error {
	constexpr bool operator==(const Error &other) const = default;

	std::string toString();

	ErrorType type;
	uint32_t code;
	std::optional<SenseData> sense = {};
};

struct RequestSense {
	uint8_t opCode;
	uint8_t options;
	uint8_t reserved[2];
	uint8_t allocationLength;
	uint8_t control;
};
static_assert(sizeof(RequestSense) == 6);

struct FixedSense {
	uint8_t responseCode;
	uint8_t obsolete;
	uint8_t flags;
	uint8_t information[4];
	uint8_t additionalLength;
	uint8_t commandSpecific[4];
	uint8_t asc;
	uint8_t ascq;
	uint8_t unitCode;
	uint8_t keySpecific[3];
};
static_assert(sizeof(FixedSense) == 18);

// Returns nothing if the device did not fill in fixed-format sense data.
std::optional<SenseData> decodeSense(const FixedSense &sense);

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
	// Returns the number of bytes transferred, which is less than info.data.size() if the device cut the data short.
	// On CHECK CONDITION errors, the implementation must return the sense data in Error::sense if the device provides any
	// (e.g., by using the REQUEST SENSE command or by obtaining it directly from the device).
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

	// Finds out what the device is. Must succeed before calling runDevice().
	async::result<std::expected<void, blockfs::Error>> probe();

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

	async::result<frg::expected<Error, size_t>> sendWithRetries(const CommandInfo &info);

	async::result<std::expected<void, blockfs::Error>> checkDeviceType();
	async::result<std::expected<void, blockfs::Error>> waitUntilReady();
	async::result<std::expected<void, blockfs::Error>> readCapacity();
	async::result<std::expected<void, blockfs::Error>> probeWriteCache();

	bool writeCacheEnabled = false;
};

inline constexpr uint8_t WELL_KNOWN_REPORT_LUNS_LUN = 1;

} // namespace scsi
