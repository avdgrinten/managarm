#include "scsi.hpp"

#include <arch/bit.hpp>
#include <helix/timer.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <print>

namespace {
	constexpr bool logRequests = false;
	constexpr bool logSteps = false;

	template<size_t N>
	void storeBigEndian(uint8_t (&bytes)[N], uint64_t value) {
		for (size_t i = 0; i < N; i++)
			bytes[i] = value >> (8 * (N - i - 1));
	}

	template<size_t N>
	uint64_t loadBigEndian(const uint8_t (&bytes)[N]) {
		uint64_t value = 0;
		for (size_t i = 0; i < N; i++)
			value = (value << 8) | bytes[i];
		return value;
	}
}

namespace scsi {

struct Read6 {
	uint8_t opCode;
	uint8_t lba[3];
	uint8_t transferLength;
	uint8_t control;
};
static_assert(sizeof(Read6) == 6);

struct Read10 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t groupNumber;
	uint8_t transferLength[2];
	uint8_t control;
};
static_assert(sizeof(Read10) == 10);

struct Write10 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t groupNumber;
	uint8_t transferLength[2];
	uint8_t control;
};
static_assert(sizeof(Write10) == 10);

struct SynchronizeCache10 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t groupNumber;
	uint8_t numberOfBlocks[2];
	uint8_t control;
};
static_assert(sizeof(SynchronizeCache10) == 10);

struct Read12 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};

struct Read16 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[8];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};

struct Write16 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[8];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};
static_assert(sizeof(Write16) == 16);

struct Read32 {
	uint8_t opCode;
	uint8_t control;
	uint32_t no_use;
	uint8_t grpNumber;
	uint8_t cdbLength;
	uint8_t serviceAction[2];
	uint8_t options;
	uint8_t no_use2;
	uint8_t lba[8];
	uint8_t referenceTag[4];
	uint8_t applicationTag[2];
	uint8_t applicationTagMask[2];
	uint8_t transferLength[4];
};

struct ReportLuns {
	uint8_t opCode;
	uint8_t reserved0;
	uint8_t selectReport;
	uint8_t reserved1[3];
	uint8_t allocationLength[4];
	uint8_t reserved2;
	uint8_t control;
};

struct Inquiry {
	uint8_t opCode;
	uint8_t options;
	uint8_t pageCode;
	uint8_t allocationLength[2];
	uint8_t control;
};
static_assert(sizeof(Inquiry) == 6);

struct TestUnitReady {
	uint8_t opCode;
	uint8_t reserved[4];
	uint8_t control;
};
static_assert(sizeof(TestUnitReady) == 6);

struct ReadCapacity10 {
	uint8_t opCode;
	uint8_t reserved[8];
	uint8_t control;
};
static_assert(sizeof(ReadCapacity10) == 10);

struct ReadCapacity10Data {
	uint8_t lastLba[4];
	uint8_t blockLength[4];
};
static_assert(sizeof(ReadCapacity10Data) == 8);

struct ReadCapacity16 {
	uint8_t opCode;
	uint8_t serviceAction;
	uint8_t lba[8];
	uint8_t allocationLength[4];
	uint8_t reserved;
	uint8_t control;
};
static_assert(sizeof(ReadCapacity16) == 16);

struct ReadCapacity16Data {
	uint8_t lastLba[8];
	uint8_t blockLength[4];
	uint8_t reserved[20];
};
static_assert(sizeof(ReadCapacity16Data) == 32);

struct ModeSense6 {
	uint8_t opCode;
	uint8_t options;
	uint8_t pageCode;
	uint8_t subpageCode;
	uint8_t allocationLength;
	uint8_t control;
};
static_assert(sizeof(ModeSense6) == 6);

Error statusToError(uint8_t status) {
	switch (status) {
	case 0:
		return {.type = ErrorType::success, .code = 0};
	case 2:
		return {.type = ErrorType::checkCondition, .code = 2};
	case 4:
		return {.type = ErrorType::conditionMet, .code = 4};
	case 8:
		return {.type = ErrorType::busy, .code = 8};
	case 0x18:
		return {.type = ErrorType::reservationConflict, .code = 0x18};
	case 0x28:
		return {.type = ErrorType::taskSetFull, .code = 0x28};
	case 0x30:
		return {.type = ErrorType::acaActive, .code = 0x30};
	case 0x40:
		return {.type = ErrorType::taskAborted, .code = 0x40};
	default:
		return {.type = ErrorType::deviceSpecific, .code = status};
	}
}

std::optional<SenseData> decodeSense(const FixedSense &sense) {
	// Bit 7 only says whether the information field is valid.
	auto responseCode = sense.responseCode & 0x7f;
	if (responseCode != 0x70 && responseCode != 0x71)
		return std::nullopt;
	return SenseData{
		.key = static_cast<SenseKey>(sense.flags & 0xf),
		.asc = sense.asc,
		.ascq = sense.ascq
	};
}

std::string Error::toString() {
	std::string_view errorName;
	switch (type) {
	case ErrorType::success:
		errorName = "SCSI_SUCCESS";
		break;
	case ErrorType::checkCondition:
		errorName = "SCSI_CHECK_CONDITION";
		break;
	case ErrorType::conditionMet:
		errorName = "SCSI_CONDITION_MET";
		break;
	case ErrorType::busy:
		errorName = "SCSI_BUSY";
		break;
	case ErrorType::reservationConflict:
		errorName = "SCSI_RESERVATION_CONFLICT";
		break;
	case ErrorType::taskSetFull:
		errorName = "SCSI_TASK_SET_FULL";
		break;
	case ErrorType::acaActive:
		errorName = "SCSI_ACA_ACTIVE";
		break;
	case ErrorType::taskAborted:
		errorName = "SCSI_TASK_ABORTED";
		break;
	case ErrorType::deviceSpecific:
		errorName = "DEVICE_SPECIFIC";
		break;
	case ErrorType::transportError:
		errorName = "TRANSPORT_ERROR";
		break;
	}

	if (sense)
		return std::format("{} (code {:#x}, sense {:#x}/{:#x}/{:#x})", errorName, code,
				static_cast<uint8_t>(sense->key), sense->asc, sense->ascq);
	return std::format("{} (code {:#x})", errorName, code);
}

async::result<frg::expected<Error, std::vector<uint64_t>>> Interface::reportLuns() {
	ReportLuns command{};
	command.opCode = 0xa0;
	command.selectReport = 0;
	command.allocationLength[3] = 4;

	uint32_t lunListLength = 0;

	CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data{nullptr, &lunListLength, 4},
		.isWrite = false
	};
	auto result = co_await sendScsiCommand(info);
	if (!result) {
		co_return result.error();
	}

	lunListLength = arch::from_endian<arch::big_endian, uint32_t>(lunListLength);

	std::vector<uint64_t> data((lunListLength + 8) / 8);
	info.data = arch::dma_buffer_view{nullptr, data.data(), data.size() * 8};

	command.allocationLength[0] = data.size() >> 24;
	command.allocationLength[1] = data.size() >> 16;
	command.allocationLength[2] = data.size() >> 8;
	command.allocationLength[3] = data.size();

	result = co_await sendScsiCommand(info);
	if (!result) {
		co_return result.error();
	}

	data[0] = arch::from_endian<arch::big_endian, uint32_t>(data[0] & 0xffffffff);
	co_return data;
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::performIo(bool isWrite, uint64_t sector,
		arch::dma_buffer_view view) {
	auto numSectors = view.size() >> sectorShift;

	if (logRequests)
		std::println(std::cout, "block-scsi: Reading {} sectors", numSectors);
	assert(numSectors);
	assert(numSectors <= 0xffff);

	uint8_t commandData[16];
	uint8_t commandLength;

	if (!isWrite) {
		if (enableRead6 && sector <= 0x1fffff && numSectors <= 0xff) {
			Read6 command{};
			command.opCode = 0x08;
			command.lba[0] = sector >> 16;
			command.lba[1] = (sector >> 8) & 0xff;
			command.lba[2] = sector & 0xff;
			command.transferLength = numSectors;

			commandLength = sizeof(Read6);
			memcpy(commandData, &command, sizeof(Read6));
		} else if (sector <= 0xffffffff) {
			Read10 command{};
			command.opCode = 0x28;
			command.lba[0] = sector >> 24;
			command.lba[1] = (sector >> 16) & 0xff;
			command.lba[2] = (sector >> 8) & 0xff;
			command.lba[3] = sector & 0xff;
			command.transferLength[0] = numSectors >> 8;
			command.transferLength[1] = numSectors & 0xff;

			commandLength = sizeof(Read10);
			memcpy(commandData, &command, sizeof(Read10));
		} else {
			Read16 command{};
			command.opCode = 0x88;
			storeBigEndian(command.lba, sector);
			storeBigEndian(command.transferLength, numSectors);

			commandLength = sizeof(Read16);
			memcpy(commandData, &command, sizeof(Read16));
		}
	} else {
		if (sector <= 0xffffffff) {
			Write10 command{};
			command.opCode = 0x2a;
			command.lba[0] = sector >> 24;
			command.lba[1] = (sector >> 16) & 0xff;
			command.lba[2] = (sector >> 8) & 0xff;
			command.lba[3] = sector & 0xff;
			command.transferLength[0] = numSectors >> 8;
			command.transferLength[1] = numSectors & 0xff;

			commandLength = sizeof(Write10);
			memcpy(commandData, &command, sizeof(Write10));
		} else {
			Write16 command{};
			command.opCode = 0x8a;
			storeBigEndian(command.lba, sector);
			storeBigEndian(command.transferLength, numSectors);

			commandLength = sizeof(Write16);
			memcpy(commandData, &command, sizeof(Write16));
		}
	}

	if (logSteps)
		std::println(std::cout, "block-scsi: Sending command");

	CommandInfo info{
		.command{nullptr, commandData, commandLength},
		.data = view,
		.isWrite = isWrite
	};
	auto result = co_await sendScsiCommand(info);
	if (!result) {
		std::println(std::cout, "block-scsi: Request failed with error {}",
				result.error().toString());
		co_return std::unexpected{blockfs::Error::ioError};
	}
	if (result.value() != view.size()) {
		std::println(std::cout, "block-scsi: Request transferred only {} of {} bytes",
				result.value(), view.size());
		co_return std::unexpected{blockfs::Error::ioError};
	}

	if (logSteps)
		std::println(std::cout, "block-scsi: Request complete");
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::issueRead(unsigned int, uint64_t sector,
		arch::dma_buffer_view view) {
	co_return co_await performIo(false, sector, view);
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::issueWrite(unsigned int, uint64_t sector,
		arch::dma_buffer_view view) {
	co_return co_await performIo(true, sector, view);
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::flush() {
	if (!writeCacheEnabled)
		co_return {};

	// SYNCHRONIZE CACHE(10): IMMED=0 waits for durability. LBA=0 and
	// NUMBER OF BLOCKS=0 synchronize the entire logical unit.
	SynchronizeCache10 command{.opCode = 0x35};
	CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data = {},
		.isWrite = false
	};
	auto result = co_await sendWithRetries(info);
	if (!result) {
		auto sense = result.error().sense;
		if (sense && sense->key == SenseKey::illegalRequest) {
			std::println(std::cout, "block-scsi: Device rejects SYNCHRONIZE CACHE, not sending it again");
			writeCacheEnabled = false;
			co_return {};
		}

		std::println(std::cout, "block-scsi: Flush failed with error {}",
			result.error().toString());
		co_return std::unexpected{blockfs::Error::ioError};
	}
	co_return {};
}

async::result<frg::expected<Error, size_t>> StorageDevice::sendWithRetries(const CommandInfo &info) {
	// According to Linux, resets can be reported repeatedly; same limit as its sd has for
	// READ CAPACITY.
	constexpr int maxAttempts = 10;

	for (int i = 1; ; i++) {
		auto result = co_await sendScsiCommand(info);
		// UNIT ATTENTION reports an earlier event; the command itself did not run.
		if (result || i == maxAttempts || !result.error().sense
				|| result.error().sense->key != SenseKey::unitAttention)
			co_return result;
	}
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::checkDeviceType() {
	// Same length as Linux uses for USB storage; according to it, many devices choke on any other.
	constexpr uint8_t dataSize = 36;
	std::array<uint8_t, dataSize> data{};
	Inquiry command{.opCode = 0x12, .allocationLength = {0, dataSize}};
	CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data{nullptr, data.data(), data.size()},
		.isWrite = false
	};
	// INQUIRY should not report UNIT ATTENTION, but according to Linux many devices do anyway.
	auto result = co_await sendWithRetries(info);
	if (!result) {
		std::println(std::cout, "block-scsi: INQUIRY failed with error {}",
				result.error().toString());
		co_return std::unexpected{blockfs::Error::ioError};
	}
	// The zeroed buffer would read as a direct-access device.
	if (!result.value()) {
		std::println(std::cout, "block-scsi: INQUIRY returned no data");
		co_return std::unexpected{blockfs::Error::ioError};
	}

	// Other device types need not implement the commands that follow, and a non-zero
	// qualifier means that there is no device on this LUN.
	// Same types as Linux' sd, other than zoned devices, which need sequential writes.
	uint8_t qualifier = data[0] >> 5;
	uint8_t type = data[0] & 0x1f;
	if (qualifier || (type != 0x00 && type != 0x07 && type != 0x0e)) {
		std::println(std::cout, "block-scsi: Ignoring device with peripheral type {:#x}", data[0]);
		co_return std::unexpected{blockfs::Error::notSupported};
	}
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::waitUntilReady() {
	// Same limit as Linux gives a disk to spin up.
	constexpr int maxPolls = 1000;
	constexpr uint64_t pollInterval = 100'000'000;

	TestUnitReady command{};
	CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data = {},
		.isWrite = false
	};

	for (int i = 0; i < maxPolls; i++) {
		auto result = co_await sendScsiCommand(info);
		if (result)
			co_return {};

		auto sense = result.error().sense;
		// According to Linux, devices with slow firmware can keep reporting this until they are ready.
		bool unitAttention = sense && sense->key == SenseKey::unitAttention && sense->asc != 0x3a;
		bool becomingReady = sense && sense->key == SenseKey::notReady && sense->asc == 0x04
				&& sense->ascq <= 0x01;
		if (unitAttention || becomingReady) {
			co_await helix::sleepFor(pollInterval);
			continue;
		}

		// TODO: Send START STOP UNIT to devices that need a start command (04/02).
		// TODO: Publish devices without a medium and handle media changes.
		std::println(std::cout, "block-scsi: Device is not ready: {}", result.error().toString());
		co_return std::unexpected{blockfs::Error::ioError};
	}

	std::println(std::cout, "block-scsi: Timed out waiting for the device to become ready");
	co_return std::unexpected{blockfs::Error::ioError};
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::readCapacity() {
	ReadCapacity10 command{.opCode = 0x25};
	ReadCapacity10Data data{};
	CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data{nullptr, &data, sizeof(data)},
		.isWrite = false
	};
	auto result = co_await sendWithRetries(info);
	if (!result) {
		std::println(std::cout, "block-scsi: READ CAPACITY(10) failed with error {}",
				result.error().toString());
		co_return std::unexpected{blockfs::Error::ioError};
	}

	uint64_t lastLba = loadBigEndian(data.lastLba);
	uint64_t blockLength = loadBigEndian(data.blockLength);

	if (lastLba == 0xffffffff) {
		ReadCapacity16 command16{.opCode = 0x9e, .serviceAction = 0x10};
		ReadCapacity16Data data16{};
		storeBigEndian(command16.allocationLength, sizeof(data16));
		CommandInfo info16{
			.command{nullptr, &command16, sizeof(command16)},
			.data{nullptr, &data16, sizeof(data16)},
			.isWrite = false
		};
		auto result16 = co_await sendWithRetries(info16);
		if (!result16) {
			std::println(std::cout, "block-scsi: READ CAPACITY(16) failed with error {}",
					result16.error().toString());
			co_return std::unexpected{blockfs::Error::ioError};
		}

		lastLba = loadBigEndian(data16.lastLba);
		blockLength = loadBigEndian(data16.blockLength);
	}

	// TODO: Support other block lengths; sectorSize is currently fixed before probing.
	if (blockLength != sectorSize) {
		std::println(std::cout, "block-scsi: Unsupported block length {}", blockLength);
		co_return std::unexpected{blockfs::Error::notSupported};
	}

	// TODO: According to Linux, some devices report the number of blocks here instead of the last LBA.
	storageSize = (lastLba + 1) << sectorShift;
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::probeWriteCache() {
	// According to Linux, some devices report generic mode pages until the media has been accessed.
	arch::dma_buffer sector{pagePool, sectorSize};
	(void)(co_await performIo(false, 0, sector));

	// According to Linux, many devices only accept a length of 192 bytes here, which is what
	// Windows uses, and a number of them have problems with requests for the Caching mode page.
	constexpr uint8_t dataSize = 192;
	std::array<uint8_t, dataSize> data{};
	ModeSense6 command{.opCode = 0x1a, .pageCode = 0x3f, .allocationLength = dataSize};
	CommandInfo info{
		.command{nullptr, &command, sizeof(command)},
		.data{nullptr, data.data(), data.size()},
		.isWrite = false
	};
	auto result = co_await sendWithRetries(info);
	if (!result) {
		// The device cannot be recovered from this, so do not publish a dead disk.
		if (result.error().type == ErrorType::transportError) {
			std::println(std::cout, "block-scsi: Device failed while probing the write cache");
			co_return std::unexpected{blockfs::Error::ioError};
		}

		std::println(std::cout, "block-scsi: No mode pages ({}), assuming write-through",
				result.error().toString());
		co_return {};
	}

	// The transferred size does not matter since the zeroed buffer has no valid pages.
	bool writeProtected = data[2] & 0x80;
	size_t length = std::min<size_t>(data[0] + 1, data.size());
	size_t offset = 4 + data[3];
	while (offset + 2 < length) {
		uint8_t pageCode = data[offset] & 0x3f;
		bool hasSubpages = data[offset] & 0x40;

		if (!hasSubpages && pageCode == 8) {
			writeCacheEnabled = (data[offset + 2] & 0x04) && !writeProtected;
			std::println(std::cout, "block-scsi: Write cache is {}",
					writeCacheEnabled ? "enabled" : "disabled");
			co_return {};
		}

		if (hasSubpages) {
			if (offset + 3 >= length)
				break;
			offset += 4 + ((size_t{data[offset + 2]} << 8) | data[offset + 3]);
		} else {
			offset += 2 + data[offset + 1];
		}
	}

	std::println(std::cout, "block-scsi: No Caching mode page, assuming write-through");
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> StorageDevice::probe() {
	// Same order as Linux: INQUIRY identifies the device, which must then be ready
	// before it can report its capacity.
	if (auto res = co_await checkDeviceType(); !res)
		co_return res;
	if (auto res = co_await waitUntilReady(); !res)
		co_return res;
	if (auto res = co_await readCapacity(); !res)
		co_return res;
	if (auto res = co_await probeWriteCache(); !res)
		co_return res;
	co_return {};
}

async::result<size_t> StorageDevice::getSize() {
	if (!storageSize) {
		std::println(std::cout, "block-scsi: StorageDevice has no size!");
	}
	co_return storageSize;
}

} // namespace scsi
