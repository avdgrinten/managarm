#include "scsi.hpp"

#include <arch/bit.hpp>
#include <core/logging.hpp>

#include <format>
#include <print>

namespace {
	constexpr bool logRequests = false;
	constexpr bool logSteps = false;
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
	}

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

async::result<void> StorageDevice::performIo(bool isWrite, uint64_t sector,
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
			logPanic("block-scsi: High LBAs are not supported!");
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
			logPanic("block-scsi: High LBAs are not supported!");
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
		logPanic("block-scsi: Request failed with error {}",
				result.error().toString());
	}

	if (logSteps)
		std::println(std::cout, "block-scsi: Request complete");
}

async::result<void> StorageDevice::performSplitIo(bool isWrite, uint64_t sector,
		arch::dma_buffer_view view) {
	size_t maxBytes = maxSectorsPerCommand << sectorShift;
	for (size_t offset = 0; offset < view.size(); offset += maxBytes)
		co_await performIo(isWrite, sector + (offset >> sectorShift),
				view.subview(offset, std::min(maxBytes, view.size() - offset)));
}

async::result<void> StorageDevice::readSectors(uint64_t sector,
		arch::dma_buffer_view view) {
	co_await performSplitIo(false, sector, view);
}

async::result<void> StorageDevice::writeSectors(uint64_t sector,
		arch::dma_buffer_view view) {
	co_await performSplitIo(true, sector, view);
}

async::result<size_t> StorageDevice::getSize() {
	if (!storageSize) {
		std::println(std::cout, "block-scsi: StorageDevice has no size!");
	}
	co_return storageSize;
}

} // namespace scsi
