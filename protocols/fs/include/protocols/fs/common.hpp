#pragma once

#include <expected>
#include <optional>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <variant>
#include <vector>

#include "fs.bragi.hpp"

namespace protocols {
namespace fs {

enum class SynchronizeFlags : uint32_t {
	none = 0,
	dataOnly = 1,
};

enum class Error {
	none = 0,
	fileNotFound = 1,
	endOfFile = 2,
	illegalArguments = 4,
	wouldBlock = 5,
	seekOnPipe = 6,
	brokenPipe = 7,
	accessDenied = 8,
	notDirectory = 20,

	afNotSupported = 9,
	destAddrRequired = 10,
	netUnreachable = 11,
	messageSize = 12,
	hostUnreachable = 13,
	insufficientPermissions = 14,
	addressInUse = 15,
	addressNotAvailable = 16,
	notConnected = 17,
	alreadyExists = 18,
	illegalOperationTarget = 19,
	noSpaceLeft = 21,
	notTerminal = 22,
	noBackingDevice = 23,
	isDirectory = 24,
	invalidProtocolOption = 25,
	directoryNotEmpty = 26,
	connectionRefused = 27,
	internalError = 28,
	alreadyConnected = 29,
	notSocket = 30,
	interrupted = 31,
	noSuchProcess = 32,
	nameTooLong = 33,
	noFileDescriptorsAvailable = 34,
	notSupported = 35,
	badFileDescriptor = 36,
	ioError = 37,
	busy = 38,
	noMemory = 39,
};

struct ToFsError {
	template<typename E>
	auto operator() (E e) const { return e | *this; }
};
constexpr ToFsError toFsError;

inline managarm::fs::Errors operator|(Error e, ToFsError) {
	switch(e) {
		case Error::none: return managarm::fs::Errors::SUCCESS;
		case Error::fileNotFound: return managarm::fs::Errors::FILE_NOT_FOUND;
		case Error::endOfFile: return managarm::fs::Errors::END_OF_FILE;
		case Error::illegalArguments: return managarm::fs::Errors::ILLEGAL_ARGUMENT;
		case Error::wouldBlock: return managarm::fs::Errors::WOULD_BLOCK;
		case Error::seekOnPipe: return managarm::fs::Errors::SEEK_ON_PIPE;
		case Error::brokenPipe: return managarm::fs::Errors::BROKEN_PIPE;
		case Error::accessDenied: return managarm::fs::Errors::ACCESS_DENIED;
		case Error::notDirectory: return managarm::fs::Errors::NOT_DIRECTORY;
		case Error::afNotSupported: return managarm::fs::Errors::AF_NOT_SUPPORTED;
		case Error::destAddrRequired: return managarm::fs::Errors::DESTINATION_ADDRESS_REQUIRED;
		case Error::netUnreachable: return managarm::fs::Errors::NETWORK_UNREACHABLE;
		case Error::messageSize: return managarm::fs::Errors::MESSAGE_TOO_LARGE;
		case Error::hostUnreachable: return managarm::fs::Errors::HOST_UNREACHABLE;
		case Error::insufficientPermissions: return managarm::fs::Errors::INSUFFICIENT_PERMISSIONS;
		case Error::addressInUse: return managarm::fs::Errors::ADDRESS_IN_USE;
		case Error::addressNotAvailable: return managarm::fs::Errors::ADDRESS_NOT_AVAILABLE;
		case Error::notConnected: return managarm::fs::Errors::NOT_CONNECTED;
		case Error::alreadyExists: return managarm::fs::Errors::ALREADY_EXISTS;
		case Error::illegalOperationTarget: return managarm::fs::Errors::ILLEGAL_OPERATION_TARGET;
		case Error::noSpaceLeft: return managarm::fs::Errors::NO_SPACE_LEFT;
		case Error::notTerminal: return managarm::fs::Errors::NOT_A_TERMINAL;
		case Error::noBackingDevice: return managarm::fs::Errors::NO_BACKING_DEVICE;
		case Error::isDirectory: return managarm::fs::Errors::IS_DIRECTORY;
		case Error::invalidProtocolOption: return managarm::fs::Errors::INVALID_PROTOCOL_OPTION;
		case Error::directoryNotEmpty: return managarm::fs::Errors::DIRECTORY_NOT_EMPTY;
		case Error::connectionRefused: return managarm::fs::Errors::CONNECTION_REFUSED;
		case Error::internalError: return managarm::fs::Errors::INTERNAL_ERROR;
		case Error::alreadyConnected: return managarm::fs::Errors::ALREADY_CONNECTED;
		case Error::notSocket: return managarm::fs::Errors::NOT_A_SOCKET;
		case Error::interrupted: return managarm::fs::Errors::INTERRUPTED;
		case Error::noSuchProcess: return managarm::fs::Errors::NO_SUCH_PROCESS;
		case Error::nameTooLong: return managarm::fs::Errors::NAME_TOO_LONG;
		case Error::noFileDescriptorsAvailable: return managarm::fs::Errors::NO_FILE_DESCRIPTORS_AVAILABLE;
		case Error::notSupported: return managarm::fs::Errors::NOT_SUPPORTED;
		case Error::badFileDescriptor: return managarm::fs::Errors::BAD_FILE_DESCRIPTOR;
		case Error::ioError: return managarm::fs::Errors::IO_ERROR;
		case Error::busy: return managarm::fs::Errors::BUSY;
		case Error::noMemory: return managarm::fs::Errors::NO_MEMORY;
	}
}

struct ToFsProtoError {
	template<typename E>
	auto operator() (E e) const { return e | *this; }
};
constexpr ToFsProtoError toFsProtoError;

inline Error operator|(managarm::fs::Errors e, ToFsProtoError) {
	switch(e) {
		case managarm::fs::Errors::SUCCESS: return Error::none;
		case managarm::fs::Errors::FILE_NOT_FOUND: return Error::fileNotFound;
		case managarm::fs::Errors::END_OF_FILE: return Error::endOfFile;
		case managarm::fs::Errors::ILLEGAL_ARGUMENT: return Error::illegalArguments;
		case managarm::fs::Errors::WOULD_BLOCK: return Error::wouldBlock;
		case managarm::fs::Errors::SEEK_ON_PIPE: return Error::seekOnPipe;
		case managarm::fs::Errors::BROKEN_PIPE: return Error::brokenPipe;
		case managarm::fs::Errors::ACCESS_DENIED: return Error::accessDenied;
		case managarm::fs::Errors::NOT_DIRECTORY: return Error::notDirectory;
		case managarm::fs::Errors::AF_NOT_SUPPORTED: return Error::afNotSupported;
		case managarm::fs::Errors::DESTINATION_ADDRESS_REQUIRED: return Error::destAddrRequired;
		case managarm::fs::Errors::NETWORK_UNREACHABLE: return Error::netUnreachable;
		case managarm::fs::Errors::MESSAGE_TOO_LARGE: return Error::messageSize;
		case managarm::fs::Errors::HOST_UNREACHABLE: return Error::hostUnreachable;
		case managarm::fs::Errors::INSUFFICIENT_PERMISSIONS: return Error::insufficientPermissions;
		case managarm::fs::Errors::ADDRESS_IN_USE: return Error::addressInUse;
		case managarm::fs::Errors::ADDRESS_NOT_AVAILABLE: return Error::addressNotAvailable;
		case managarm::fs::Errors::NOT_CONNECTED: return Error::notConnected;
		case managarm::fs::Errors::ALREADY_EXISTS: return Error::alreadyExists;
		case managarm::fs::Errors::ILLEGAL_OPERATION_TARGET: return Error::illegalOperationTarget;
		case managarm::fs::Errors::NO_SPACE_LEFT: return Error::noSpaceLeft;
		case managarm::fs::Errors::NOT_A_TERMINAL: return Error::notTerminal;
		case managarm::fs::Errors::NO_BACKING_DEVICE: return Error::noBackingDevice;
		case managarm::fs::Errors::IS_DIRECTORY: return Error::isDirectory;
		case managarm::fs::Errors::INVALID_PROTOCOL_OPTION: return Error::invalidProtocolOption;
		case managarm::fs::Errors::DIRECTORY_NOT_EMPTY: return Error::directoryNotEmpty;
		case managarm::fs::Errors::CONNECTION_REFUSED: return Error::connectionRefused;
		case managarm::fs::Errors::INTERNAL_ERROR: return Error::internalError;
		case managarm::fs::Errors::ALREADY_CONNECTED: return Error::alreadyConnected;
		case managarm::fs::Errors::NOT_A_SOCKET: return Error::notSocket;
		case managarm::fs::Errors::INTERRUPTED: return Error::interrupted;
		case managarm::fs::Errors::NO_SUCH_PROCESS: return Error::noSuchProcess;
		case managarm::fs::Errors::NAME_TOO_LONG: return Error::nameTooLong;
		case managarm::fs::Errors::NO_FILE_DESCRIPTORS_AVAILABLE: return Error::noFileDescriptorsAvailable;
		case managarm::fs::Errors::NOT_SUPPORTED: return Error::notSupported;
		case managarm::fs::Errors::BAD_FILE_DESCRIPTOR: return Error::badFileDescriptor;
		case managarm::fs::Errors::IO_ERROR: return Error::ioError;
		case managarm::fs::Errors::BUSY: return Error::busy;
		case managarm::fs::Errors::NO_MEMORY: return Error::noMemory;
	}
}

using ReadResult = std::expected<size_t, Error>;

struct ReadEntriesResult {
	std::string name;
	ino_t inode;
	long offset;
	int64_t fileType;
};

using PollResult = std::tuple<uint64_t, int, int>;
using PollWaitResult = std::tuple<uint64_t, int>;
using PollStatusResult = std::tuple<uint64_t, int>;

struct RecvData {
	std::vector<char> ctrl;
	size_t dataLength;
	size_t addressLength;
	uint32_t flags;
};

using RecvResult = std::variant<Error, RecvData>;
using SendResult = std::variant<Error, size_t>;

struct CtrlBuilder {
	CtrlBuilder(size_t max_size)
	: _maxSize{max_size}, _offset{0} { }

	// returns true if the message gets truncated
	[[nodiscard("You must check whether the message is truncated")]]
	bool message(int layer, int type, size_t payload) {
		size_t remaining_space = _maxSize - _buffer.size();
		if(remaining_space < CMSG_ALIGN(sizeof(struct cmsghdr)))
			return true;

		auto truncated = CMSG_SPACE(payload) > remaining_space;

		_offset = _buffer.size();
		_buffer.resize(_offset + (truncated ? remaining_space : CMSG_SPACE(payload)));

		struct cmsghdr h;
		memset(&h, 0, sizeof(struct cmsghdr));
		h.cmsg_len = CMSG_LEN(payload);
		h.cmsg_level = layer;
		h.cmsg_type = type;

		memcpy(_buffer.data() + _offset, &h, sizeof(struct cmsghdr));
		_offset += CMSG_ALIGN(sizeof(struct cmsghdr));

		return truncated;
	}

	// returns whether the message gets truncated, and if true, how many bytes of payload
	// (aligned to data_unit_size) still fit in the buffer.
	[[nodiscard("You must check whether the message is truncated")]]
	std::pair<bool, size_t> message_truncated(int layer, int type, size_t payload, size_t data_unit_size) {
		// the space remaining in the control buffer before this message
		size_t remaining_space = _maxSize - _buffer.size();
		// if not even a cmsghdr fits, the message is truncated and no payload space available
		if(remaining_space < CMSG_ALIGN(sizeof(struct cmsghdr)))
			return {true, 0};

		auto truncated = CMSG_SPACE(payload) > remaining_space;
		// the amount of space left for the data payload after the cmsghdr
		size_t remaining_payload_space = remaining_space - CMSG_ALIGN(sizeof(struct cmsghdr));
		// correct the actual amount of available payload length to not overflow the buffer
		auto truncated_payload = std::min(payload, remaining_payload_space);

		_offset = _buffer.size();
		_buffer.resize(_offset + (truncated ? remaining_space : CMSG_SPACE(payload)));

		struct cmsghdr h;
		memset(&h, 0, sizeof(struct cmsghdr));
		h.cmsg_len = CMSG_LEN(truncated_payload - (truncated_payload % data_unit_size));
		h.cmsg_level = layer;
		h.cmsg_type = type;

		// write out the cmsghdr
		memcpy(_buffer.data() + _offset, &h, sizeof(struct cmsghdr));
		// correctly align up the offset to account for the cmsghdr
		_offset += CMSG_ALIGN(sizeof(struct cmsghdr));

		if(CMSG_SPACE(payload) > remaining_space)
			return {true, std::max(0UL, remaining_payload_space - (remaining_payload_space % data_unit_size))};
		return {false, 0};
	}

	template<typename T>
	requires requires(T t) {
		{ t.size() } -> std::same_as<size_t>;
		{ t.data() } -> std::same_as<void *>;
	}
	void write_buffer(T data) {
		assert(_buffer.size() >= _offset + data.size());
		memcpy(_buffer.data() + _offset, data.data(), data.size());
		_offset += data.size();
	}

	template<typename T>
	void write(T data) {
		assert(_buffer.size() >= _offset + sizeof(T));
		memcpy(_buffer.data() + _offset, &data, sizeof(T));
		_offset += sizeof(T);
	}

	std::vector<char> buffer() {
		return std::move(_buffer);
	}

private:
	std::vector<char> _buffer;
	size_t _maxSize;
	size_t _offset;
};

} } // namespace protocols::fs
