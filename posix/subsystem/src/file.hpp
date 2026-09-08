#pragma once

#include <variant>
#include <string.h> // for hel.h
#include <print>
#include <vector>

#include <async/result.hpp>
#include <frg/expected.hpp>
#include <hel.h>
#include <protocols/fs/common.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/fs/client.hpp>
#include <protocols/fs/file-locks.hpp>
#include <posix.bragi.hpp>
#include <sys/socket.h>

#include "common.hpp"
#include "link-rc.hpp"

struct File;
struct MountView;
struct FsLink;
struct Process;
struct ControllingTerminalState;

// Synthesizes "." and ".." for filesystems that do not store them: yields each
// in turn and then std::nullopt, with `phase` tracking progress across calls.
enum class DotEntriesPhase { dot, dotDot, done };

inline std::optional<protocols::fs::ReadEntriesResult>
nextDotEntry(DotEntriesPhase &phase, ino_t self, ino_t parent) {
	if(phase == DotEntriesPhase::dot) {
		phase = DotEntriesPhase::dotDot;
		return protocols::fs::ReadEntriesResult{
			.name = ".",
			.inode = self,
			.offset = 1,
			.fileType = managarm::fs::FileType::DIRECTORY,
		};
	}
	if(phase == DotEntriesPhase::dotDot) {
		phase = DotEntriesPhase::done;
		return protocols::fs::ReadEntriesResult{
			.name = "..",
			.inode = parent,
			.offset = 2,
			.fileType = managarm::fs::FileType::DIRECTORY,
		};
	}
	return std::nullopt;
}

struct FileHandle {
	FileHandle() = default;

	explicit FileHandle(File *file)
	: _file{file} { }

	explicit operator bool() const {
		return _file != nullptr;
	}

	void increment() const;
	void decrement() const;

private:
	File *_file{nullptr};
};

using SharedFilePtr = smarter::shared_ptr<File, FileHandle>;

// TODO: Rename this enum as is not part of the VFS.
enum class Error {
	success,
	notDirectory,
	noSuchFile,
	eof,
	fileClosed,

	// Binary is corrupted or does not match a known binary format.
	badExecutable,

	// Indices that the given object does not support the operation
	// (e.g. readSymlink() is called on a file that is not a link).
	illegalOperationTarget,

	seekOnPipe,

	wouldBlock,

	brokenPipe,

	illegalArguments,

	insufficientPermissions,

	accessDenied,

	notConnected,

	alreadyExists,

	notTerminal,

	// Corresponds with ENXIO
	noBackingDevice,

	// Corresponds with ENOSPC
	noSpaceLeft,

	// Corresponds with EISDIR
	isDirectory,

	noMemory,

	directoryNotEmpty,

	// Corresponds with ENAMETOOLONG
	nameTooLong,

	// Failure of the underlying device, corresponds to EIO
	ioError,

	noChildProcesses,

	alreadyConnected,

	unsupportedSocketType,

	notSocket,

	interrupted,

	// Corresponds to ESRCH
	noSuchProcess,

	noFileDescriptorsAvailable,

	notSupported,

	// Corresponds with EBADF
	badFileDescriptor,

	// Credentials passed in a request did not match any known process.
	// Maps to EIO.
	badProcessCredentials,
};

inline protocols::fs::Error operator|(Error e, protocols::fs::ToFsProtoError) {
	switch(e) {
		case Error::success: return protocols::fs::Error::none;
		case Error::noSuchFile: return protocols::fs::Error::fileNotFound;
		case Error::eof: return protocols::fs::Error::endOfFile;
		case Error::illegalArguments: return protocols::fs::Error::illegalArguments;
		case Error::wouldBlock: return protocols::fs::Error::wouldBlock;
		case Error::seekOnPipe: return protocols::fs::Error::seekOnPipe;
		case Error::brokenPipe: return protocols::fs::Error::brokenPipe;
		case Error::accessDenied: return protocols::fs::Error::accessDenied;
		case Error::notDirectory: return protocols::fs::Error::notDirectory;
		case Error::insufficientPermissions: return protocols::fs::Error::insufficientPermissions;
		case Error::notConnected: return protocols::fs::Error::notConnected;
		case Error::alreadyExists: return protocols::fs::Error::alreadyExists;
		case Error::illegalOperationTarget: return protocols::fs::Error::illegalOperationTarget;
		case Error::noSpaceLeft: return protocols::fs::Error::noSpaceLeft;
		case Error::notTerminal: return protocols::fs::Error::notTerminal;
		case Error::noBackingDevice: return protocols::fs::Error::noBackingDevice;
		case Error::isDirectory: return protocols::fs::Error::isDirectory;
		case Error::directoryNotEmpty: return protocols::fs::Error::directoryNotEmpty;
		case Error::fileClosed: return protocols::fs::Error::internalError;
		case Error::badExecutable: return protocols::fs::Error::internalError;
		case Error::noMemory: return protocols::fs::Error::noSpaceLeft;
		case Error::ioError: return protocols::fs::Error::internalError;
		case Error::noChildProcesses: return protocols::fs::Error::internalError;
		case Error::alreadyConnected: return protocols::fs::Error::alreadyConnected;
		case Error::notSocket: return protocols::fs::Error::notSocket;
		case Error::interrupted: return protocols::fs::Error::interrupted;
		case Error::noSuchProcess: return protocols::fs::Error::noSuchProcess;
		case Error::noFileDescriptorsAvailable: return protocols::fs::Error::noFileDescriptorsAvailable;
		case Error::notSupported: return protocols::fs::Error::notSupported;
		case Error::badFileDescriptor: return protocols::fs::Error::badFileDescriptor;
		case Error::badProcessCredentials: return protocols::fs::Error::internalError;
		default:
			std::cout << std::format("posix: unmapped Error {}", static_cast<int>(e)) << std::endl;
			return protocols::fs::Error::internalError;
	}
}

struct ToPosixProtoError {
	template<typename E>
	auto operator() (E e) const { return e | *this; }
};
constexpr ToPosixProtoError toPosixProtoError;

inline managarm::posix::Errors operator|(Error e, ToPosixProtoError) {
	switch(e) {
		case Error::success: return managarm::posix::Errors::SUCCESS;
		case Error::noSuchFile: return managarm::posix::Errors::FILE_NOT_FOUND;
		case Error::eof: return managarm::posix::Errors::END_OF_FILE;
		case Error::illegalArguments: return managarm::posix::Errors::ILLEGAL_ARGUMENTS;
		case Error::wouldBlock: return managarm::posix::Errors::WOULD_BLOCK;
		case Error::brokenPipe: return managarm::posix::Errors::BROKEN_PIPE;
		case Error::accessDenied: return managarm::posix::Errors::ACCESS_DENIED;
		case Error::notDirectory: return managarm::posix::Errors::NOT_A_DIRECTORY;
		case Error::nameTooLong: return managarm::posix::Errors::NAME_TOO_LONG;
		case Error::insufficientPermissions: return managarm::posix::Errors::INSUFFICIENT_PERMISSION;
		case Error::alreadyExists: return managarm::posix::Errors::ALREADY_EXISTS;
		case Error::illegalOperationTarget: return managarm::posix::Errors::ILLEGAL_OPERATION_TARGET;
		case Error::notTerminal: return managarm::posix::Errors::NOT_A_TTY;
		case Error::noBackingDevice: return managarm::posix::Errors::NO_BACKING_DEVICE;
		case Error::isDirectory: return managarm::posix::Errors::IS_DIRECTORY;
		case Error::directoryNotEmpty: return managarm::posix::Errors::DIRECTORY_NOT_EMPTY;
		case Error::noMemory: return managarm::posix::Errors::NO_MEMORY;
		case Error::ioError: return managarm::posix::Errors::INTERNAL_ERROR;
		case Error::noChildProcesses: return managarm::posix::Errors::NO_CHILD_PROCESSES;
		case Error::alreadyConnected: return managarm::posix::Errors::ALREADY_CONNECTED;
		case Error::unsupportedSocketType: return managarm::posix::Errors::UNSUPPORTED_SOCKET_TYPE;
		case Error::interrupted: return managarm::posix::Errors::INTERRUPTED;
		case Error::noSuchProcess: return managarm::posix::Errors::NO_SUCH_RESOURCE;
		case Error::noFileDescriptorsAvailable: return managarm::posix::Errors::NO_FILE_DESCRIPTORS_AVAILABLE;
		case Error::notSupported: return managarm::posix::Errors::NOT_SUPPORTED;
		case Error::badProcessCredentials: return managarm::posix::Errors::INTERNAL_ERROR;
		case Error::fileClosed:
		case Error::badExecutable:
		case Error::seekOnPipe:
		case Error::notConnected:
		case Error::noSpaceLeft:
		case Error::notSocket:
		case Error::badFileDescriptor:
			std::cout << std::format("posix: unmapped Error {}", static_cast<int>(e)) << std::endl;
			return managarm::posix::Errors::INTERNAL_ERROR;
	}
}

struct ToPosixError {
	template<typename E>
	auto operator() (E e) const { return e | *this; }
};
constexpr ToPosixError toPosixError;

inline Error operator|(protocols::fs::Error e, ToPosixError) {
	switch(e) {
		case protocols::fs::Error::none: return Error::success;
		case protocols::fs::Error::fileNotFound: return Error::noSuchFile;
		case protocols::fs::Error::endOfFile: return Error::eof;
		case protocols::fs::Error::illegalArguments: return Error::illegalArguments;
		case protocols::fs::Error::wouldBlock: return Error::wouldBlock;
		case protocols::fs::Error::seekOnPipe: return Error::seekOnPipe;
		case protocols::fs::Error::brokenPipe: return Error::brokenPipe;
		case protocols::fs::Error::accessDenied: return Error::accessDenied;
		case protocols::fs::Error::notDirectory: return Error::notDirectory;
		case protocols::fs::Error::insufficientPermissions: return Error::insufficientPermissions;
		case protocols::fs::Error::notConnected: return Error::notConnected;
		case protocols::fs::Error::alreadyExists: return Error::alreadyExists;
		case protocols::fs::Error::illegalOperationTarget: return Error::illegalOperationTarget;
		case protocols::fs::Error::noSpaceLeft: return Error::noSpaceLeft;
		case protocols::fs::Error::notTerminal: return Error::notTerminal;
		case protocols::fs::Error::noBackingDevice: return Error::noBackingDevice;
		case protocols::fs::Error::isDirectory: return Error::isDirectory;
		case protocols::fs::Error::nameTooLong: return Error::nameTooLong;
		case protocols::fs::Error::directoryNotEmpty: return Error::directoryNotEmpty;
		case protocols::fs::Error::internalError: return Error::fileClosed;
		case protocols::fs::Error::noSuchProcess: return Error::noSuchProcess;
		case protocols::fs::Error::notSupported: return Error::notSupported;
		default:
			std::cout << std::format("posix: unmapped protocols::fs::Error {}", static_cast<int>(e)) << std::endl;
			return Error::ioError;
	}
}

inline Error operator|(managarm::fs::Errors e, ToPosixError) {
	switch(e) {
		case managarm::fs::Errors::SUCCESS: return Error::success;
		case managarm::fs::Errors::FILE_NOT_FOUND: return Error::noSuchFile;
		case managarm::fs::Errors::END_OF_FILE: return Error::eof;
		case managarm::fs::Errors::ILLEGAL_ARGUMENT: return Error::illegalArguments;
		case managarm::fs::Errors::WOULD_BLOCK: return Error::wouldBlock;
		case managarm::fs::Errors::SEEK_ON_PIPE: return Error::seekOnPipe;
		case managarm::fs::Errors::BROKEN_PIPE: return Error::brokenPipe;
		case managarm::fs::Errors::ACCESS_DENIED: return Error::accessDenied;
		case managarm::fs::Errors::INSUFFICIENT_PERMISSIONS: return Error::insufficientPermissions;
		case managarm::fs::Errors::NOT_CONNECTED: return Error::notConnected;
		case managarm::fs::Errors::ALREADY_EXISTS: return Error::alreadyExists;
		case managarm::fs::Errors::ILLEGAL_OPERATION_TARGET: return Error::illegalOperationTarget;
		case managarm::fs::Errors::NOT_DIRECTORY: return Error::notDirectory;
		case managarm::fs::Errors::NO_SPACE_LEFT: return Error::noSpaceLeft;
		case managarm::fs::Errors::NOT_A_TERMINAL: return Error::notTerminal;
		case managarm::fs::Errors::NO_BACKING_DEVICE: return Error::noBackingDevice;
		case managarm::fs::Errors::IS_DIRECTORY: return Error::isDirectory;
		case managarm::fs::Errors::DIRECTORY_NOT_EMPTY: return Error::directoryNotEmpty;
		case managarm::fs::Errors::INTERNAL_ERROR: return Error::ioError;
		case managarm::fs::Errors::ALREADY_CONNECTED: return Error::alreadyConnected;
		case managarm::fs::Errors::NOT_A_SOCKET: return Error::notSocket;
		case managarm::fs::Errors::INTERRUPTED: return Error::interrupted;
		case managarm::fs::Errors::NOT_SUPPORTED: return Error::notSupported;
		default:
			std::println("posix: unmapped managarm::fs::Errors Error {}", static_cast<int>(e));
			return Error::ioError;
	}
}

std::ostream& operator<<(std::ostream& os, const Error& err);

// TODO: Rename this enum as is not part of the VFS.
enum class VfsSeek {
	null, absolute, relative, eof
};

template<typename T>
using FutureMaybe = async::result<T>;

template<typename T>
using expected = async::result<std::variant<Error, T>>;

// ----------------------------------------------------------------------------
// File class.
// ----------------------------------------------------------------------------

using PollResult = std::tuple<uint64_t, int, int>;
using PollWaitResult = std::tuple<uint64_t, int>;
using PollStatusResult = std::tuple<uint64_t, int>;

using AcceptResult = smarter::shared_ptr<File, FileHandle>;

enum class FileKind {
	unknown,
	pidfd,
	timerfd,
	inotify,
};

struct File {
	friend struct FileHandle;

public:
	using DefaultOps = uint32_t;
	static inline constexpr DefaultOps defaultIsTerminal = 1 << 1;
	static inline constexpr DefaultOps defaultPipeLikeSeek = 1 << 2;
	static inline constexpr DefaultOps defaultMapsAnonymously = 1 << 3;

	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------

	static async::result<protocols::fs::SeekResult>
	ptSeekRel(void *object, int64_t offset);

	static async::result<protocols::fs::SeekResult>
	ptSeekAbs(void *object, int64_t offset);

	static async::result<protocols::fs::SeekResult>
	ptSeekEof(void *object, int64_t offset);

	static async::result<protocols::fs::ReadResult>
	ptRead(void *object, helix_ng::CredentialsView credentials, void *buffer, size_t length,
			async::cancellation_token ce);

	static async::result<protocols::fs::ReadResult>
	ptPread(void *object, int64_t offset, helix_ng::CredentialsView credentials, void *buffer, size_t length);

	static async::result<frg::expected<protocols::fs::Error, size_t>>
	ptWrite(void *object, helix_ng::CredentialsView credentials, const void *buffer, size_t length);

	static async::result<frg::expected<protocols::fs::Error, size_t>>
	ptPwrite(void *object, int64_t offset, helix_ng::CredentialsView credentials, const void *buffer, size_t length);

	static async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
	ptReadEntries(void *object);

	static async::result<frg::expected<protocols::fs::Error>>
	ptTruncate(void *object, size_t size);

	static async::result<frg::expected<protocols::fs::Error>>
	ptAllocate(void *object, int64_t offset, size_t size);

	static async::result<protocols::fs::Error>
	ptBind(void *object, helix_ng::CredentialsView credentials,
			const void *addr_ptr, size_t addr_length);

	static async::result<protocols::fs::Error>
	ptConnect(void *object, helix_ng::CredentialsView credentials,
			const void *addr_ptr, size_t addr_length);

	static async::result<size_t>
	ptSockname(void *object, void *addr_ptr, size_t max_addr_length);

	static async::result<void>
	ptIoctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation);

	static async::result<int>
	ptGetFileFlags(void *object);

	static async::result<void>
	ptSetFileFlags(void *object, int flags);

	static async::result<protocols::fs::RecvResult>
	ptRecvMsg(void *object, helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t len,
			void *addr, size_t addr_len,
			size_t max_ctrl_len);

	static async::result<frg::expected<protocols::fs::Error, size_t>>
	ptSendMsg(void *object, helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t len,
			void *addr, size_t addr_len,
			std::vector<uint32_t> fds, struct ucred ucreds);

	static async::result<protocols::fs::Error>
	ptListen(void *object);

	static async::result<frg::expected<protocols::fs::Error, size_t>>
	ptPeername(void *object, void *addr_ptr, size_t max_addr_length);

	static async::result<frg::expected<protocols::fs::Error, int>> ptGetSeals(void *object);
	static async::result<frg::expected<protocols::fs::Error, int>> ptAddSeals(void *object, int seals);

	static async::result<frg::expected<protocols::fs::Error>> ptSetSocketOption(void *obj,
			int layer, int number, std::vector<char> optbuf);
	static async::result<frg::expected<protocols::fs::Error>> ptGetSocketOption(void *obj,
			helix_ng::CredentialsView creds, int layer, int number, std::vector<char> &optbuf);

	static async::result<protocols::fs::Error> ptShutdown(void *obj, int how);

	static async::result<helix::BorrowedDescriptor> ptAccessMemory(void *object);

	static async::result<protocols::fs::Error> ptFlock(void *object, int flags);

	static constexpr auto fileOperations = protocols::fs::FileOperations{
		.seekAbs = &ptSeekAbs,
		.seekRel = &ptSeekRel,
		.seekEof = &ptSeekEof,
		.read = &ptRead,
		.pread = &ptPread,
		.write = &ptWrite,
		.pwrite = &ptPwrite,
		.readEntries = &ptReadEntries,
		.accessMemory = &ptAccessMemory,
		.truncate = &ptTruncate,
		.fallocate = &ptAllocate,
		.ioctl = &ptIoctl,
		.flock = &ptFlock,
		.bind = &ptBind,
		.listen = &ptListen,
		.connect = &ptConnect,
		.sockname = &ptSockname,
		.getFileFlags = &ptGetFileFlags,
		.setFileFlags = &ptSetFileFlags,
		.recvMsg = &ptRecvMsg,
		.sendMsg = &ptSendMsg,
		.peername = &ptPeername,
		.getSeals = &ptGetSeals,
		.addSeals = &ptAddSeals,
		.setSocketOption = &ptSetSocketOption,
		.getSocketOption = &ptGetSocketOption,
		.shutdown = &ptShutdown,
	};

	// ------------------------------------------------------------------------
	// Public File API.
	// ------------------------------------------------------------------------

	static smarter::shared_ptr<File, FileHandle> constructHandle(smarter::shared_ptr<File> ptr) {
		// _weakFile is used in FileHandle::decrement().
		assert(ptr->_weakPtr.policy());
		auto file = ptr.get();
		ptr.release();
		file->_fileCtr.setup(smarter::adopt_rc, 1);
		return smarter::shared_ptr<File, FileHandle>{smarter::adopt_rc, file, FileHandle{file}};
	}

	File(FileKind kind, StructName struct_name, DefaultOps default_ops = 0)
	: kind_{kind}, _structName{struct_name}, _defaultOps{default_ops}, _isOpen{true} { }

	File(FileKind kind, StructName struct_name, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			DefaultOps default_ops = 0)
	: _link{std::move(link)}, kind_{kind}, _structName{struct_name}, _mount{std::move(mount)},
			_defaultOps{default_ops}, _isOpen{true} { }

	virtual ~File();

// TODO: Make this protected:
	void setupWeakFile(smarter::weak_ptr<File> ptr) {
		_weakPtr = std::move(ptr);
	}

public:
	const smarter::weak_ptr<File> &weakFile() {
		return _weakPtr;
	}

	FileKind kind() const {
		return kind_;
	}

	StructName structName() {
		return _structName;
	}

	bool isOpen() {
		return _isOpen;
	}

	virtual void handleClose();

private:
	// Called when the FileHandle refcount becomes zero.
	void dispose() {
		_isOpen = false;
		handleClose();
	}

public:
	// MountView that was used to open the file.
	// See associatedLink().
	std::shared_ptr<MountView> associatedMount() {
		return _mount;
	}

	// This is the link that was used to open the file.
	// Note that this might not be the only link that can be used
	// to reach the file's inode.
	smarter::shared_ptr<FsLink, LinkRc> associatedLink() {
		if(!_link)
			std::cout << "posix \e[1;34m" << structName()
					<< "\e[0m: Object does not support associatedLink()" << std::endl;
		return _link;
	}

	bool isTerminal();

	// Whether mappings of this file are backed by anonymous memory instead of accessMemory().
	bool mapsAnonymously();

	async::result<frg::expected<Error>> readExactly(Process *process, void *data, size_t length);

	virtual async::result<frg::expected<Error, off_t>>
	seek(off_t offset, VfsSeek whence);

	virtual async::result<std::expected<size_t, Error>>
	readSome(Process *process, void *data, size_t max_length,
			async::cancellation_token ce);

	virtual async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length);

	virtual async::result<frg::expected<Error, ControllingTerminalState *>>
	getControllingTerminal();

	virtual async::result<std::expected<size_t, Error>>
	pread(Process *process, int64_t offset, void *buffer, size_t length);

	virtual async::result<frg::expected<Error, size_t>>
	pwrite(Process *process, int64_t offset, const void *data, size_t length);

	virtual FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries();

	virtual async::result<protocols::fs::RecvResult>
		recvMsg(Process *process, uint32_t flags,
			void *data, size_t max_length,
			void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length);

	virtual async::result<frg::expected<protocols::fs::Error, size_t>>
		sendMsg(Process *process, uint32_t flags,
			const void *data, size_t max_length,
			const void *addr_ptr, size_t addr_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files, struct ucred creds);

	virtual async::result<frg::expected<protocols::fs::Error>> truncate(size_t size);

	virtual async::result<frg::expected<protocols::fs::Error>> allocate(int64_t offset, size_t size);

	// pollWait() / pollStatus() uses a sequence number mechansim for synchronization.
	// Waits until the poll sequence changes *and* one of the events in the mask receives an edge.
	// Returns (current-sequence, edges since in-sequence).
	// current-sequence is incremented each time an edge (i.e. an event bit transitions from clear to set) happens.
	virtual async::result<frg::expected<Error, PollWaitResult>> pollWait(Process *,
			uint64_t sequence, int mask,
			async::cancellation_token cancellation = {});

	// Returns immediately.
	// Returns (current-sequence, active events).
	virtual async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *);

	virtual async::result<frg::expected<Error, AcceptResult>> accept(Process *process);

	virtual async::result<protocols::fs::Error> bind(Process *process,
			const void *addr_ptr, size_t addr_length);

	virtual async::result<protocols::fs::Error> connect(Process *process,
			const void *addr_ptr, size_t addr_length);

	virtual async::result<protocols::fs::Error> listen();

	virtual async::result<size_t> sockname(void *addr_ptr, size_t max_addr_length);

	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory();

	virtual async::result<void> ioctl(Process *process, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation);

	virtual async::result<int> getFileFlags();
	virtual async::result<void> setFileFlags(int flags);

	virtual async::result<frg::expected<protocols::fs::Error, size_t>> peername(void *addr_ptr, size_t max_addr_length);

	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;

	virtual async::result<frg::expected<protocols::fs::Error, int>> getSeals();
	virtual async::result<frg::expected<protocols::fs::Error, int>> addSeals(int flags);

	virtual async::result<frg::expected<Error, std::string>> ttyname();

	virtual async::result<frg::expected<protocols::fs::Error>> setSocketOption(int layer,
			int number, std::vector<char> optbuf);

	virtual async::result<frg::expected<protocols::fs::Error>> getSocketOption(Process *process,
			int layer, int number, std::vector<char> &optbuf);

	virtual async::result<protocols::fs::Error> shutdown(int how);

	virtual async::result<std::string> getFdInfo();

	virtual async::result<protocols::fs::Error> flock(int flags);

protected:
	const smarter::shared_ptr<FsLink, LinkRc> _link;

private:
	smarter::counter _fileCtr;
	smarter::weak_ptr<File> _weakPtr;
	FileKind kind_;
	StructName _structName;
	const std::shared_ptr<MountView> _mount;

	DefaultOps _defaultOps;

	bool _isOpen;
};

struct FileWithDefaults : File {
	FileWithDefaults(FileKind kind, StructName struct_name, DefaultOps default_ops = 0)
	: File{kind, struct_name, default_ops} { }

	FileWithDefaults(FileKind kind, StructName struct_name, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			DefaultOps default_ops = 0)
	: File{kind, struct_name, std::move(mount), std::move(link), default_ops} { }

	~FileWithDefaults() override;

	async::result<protocols::fs::Error> flock(int flags) override;

private:
	protocols::fs::Flock flock_;
};

struct DummyFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<DummyFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	DummyFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, DefaultOps default_ops = 0)
	: FileWithDefaults{FileKind::unknown, StructName::get("dummy-file"), std::move(mount), std::move(link), default_ops | defaultPipeLikeSeek} { }

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void handleClose() override {
		_cancelServe.cancel();
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;
};

struct PassthroughFile : File {
	PassthroughFile(helix::UniqueLane lane)
	: File{FileKind::unknown, StructName::get("passthrough")}, _file{std::move(lane)} { }


	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		auto memory = co_await _file.accessMemory();
		co_return std::move(memory);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

	async::result<frg::expected<Error, PollWaitResult>> pollWait(
	    Process *, uint64_t sequence, int mask, async::cancellation_token cancellation
	) override {
		auto res = co_await _file.pollWait(sequence, mask, cancellation);
		co_return res.map_error(toPosixError);
	}

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override {
		auto res = co_await _file.pollStatus();
		co_return res.map_error(toPosixError);
	}

private:
	protocols::fs::File _file;
};

inline void FileHandle::increment() const {
	_file->_fileCtr.increment();
}

inline void FileHandle::decrement() const {
	if(_file->_fileCtr.decrement_and_check_if_zero()) {
		_file->dispose();
		_file->_weakPtr.policy().decrement();
	}
}
