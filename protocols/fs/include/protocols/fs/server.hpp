#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <frg/expected.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <protocols/fs/common.hpp>
#include <protocols/fs/defs.hpp>
#include <smarter.hpp>
#include <sys/socket.h>

#include <deque>
#include <memory>

namespace managarm::fs {
	struct CntRequest;
}

namespace protocols::fs {

namespace utils {

// returns whether the default data was written to ucred
bool handleSoPasscred(bool so_passcred, struct ucred &ucred, pid_t process_pid, uid_t process_uid, gid_t process_gid);

} // namespace utils

enum class FileType {
	unknown,
	directory,
	regular,
	symlink
};

struct FileStats {
	int linkCount;
	uint64_t fileSize;
	uint32_t mode;
	int uid, gid;
	struct timespec accessTime;
	struct timespec dataModifyTime;
	struct timespec anyChangeTime;
};

// Representation of struct statfs.
// Data types match the corresponding Bragi message.
struct FsStats {
	unsigned int fsType;
	uint64_t blockSize;
	uint64_t fragmentSize;
	uint64_t numBlocks;
	uint64_t blocksFree;
	uint64_t blocksFreeUser;
	uint64_t numInodes;
	uint64_t inodesFree;
	uint64_t inodesFreeUser;
	uint64_t maxNameLength;
	int fsid[2];
	uint64_t flags;
};

using SeekResult = std::variant<Error, int64_t>;

using GetLinkResult = std::tuple<std::shared_ptr<void>, int64_t, FileType, uint64_t>;

using OpenResult = std::pair<helix::UniqueLane, helix::UniqueLane>;
using AcceptResult = std::pair<helix::UniqueLane, helix::UniqueLane>;

using MkdirResult = std::tuple<std::shared_ptr<void>, int64_t, uint64_t>;
using SymlinkResult = std::tuple<std::shared_ptr<void>, int64_t, uint64_t>;

struct TraversedLink {
	std::shared_ptr<void> node;
	int64_t id;
	// Mutation serial of the directory that the link was resolved in.
	uint64_t serial;
};

struct TraverseLinksError {
	Error error;
	// Mutation serial of the directory in which the failing lookup ran. Only set for fileNotFound.
	uint64_t serial = 0;
};

using TraverseLinksResult = std::expected<std::tuple<std::vector<TraversedLink>, FileType, size_t>,
		TraverseLinksError>;

struct FileOperations {
	constexpr FileOperations &withSeekAbs(async::result<SeekResult> (*f)(void *object,
			int64_t offset)) {
		seekAbs = f;
		return *this;
	}
	constexpr FileOperations &withSeekRel(async::result<SeekResult> (*f)(void *object,
			int64_t offset)) {
		seekRel = f;
		return *this;
	}
	constexpr FileOperations &withSeekEof(async::result<SeekResult> (*f)(void *object,
			int64_t offset)) {
		seekEof = f;
		return *this;
	}
	constexpr FileOperations &withRead(async::result<ReadResult> (*f)(void *object,
			helix_ng::CredentialsView, void *buffer, size_t length, async::cancellation_token cancellation)) {
		read = f;
		return *this;
	}
	constexpr FileOperations &withWrite(async::result<frg::expected<protocols::fs::Error, size_t>> (*f)(void *object,
			helix_ng::CredentialsView, const void *buffer, size_t length)) {
		write = f;
		return *this;
	}
	constexpr FileOperations &withPwrite(async::result<frg::expected<protocols::fs::Error, size_t>> (*f)(void *object,
			int64_t offset, helix_ng::CredentialsView, const void *buffer, size_t length)) {
		pwrite = f;
		return *this;
	}
	constexpr FileOperations &withReadEntries(async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> (*f)(void *object)) {
		readEntries = f;
		return *this;
	}
	constexpr FileOperations &withAccessMemory(async::result<helix::BorrowedDescriptor>(*f)(void *object)) {
		accessMemory = f;
		return *this;
	}
	constexpr FileOperations &withTruncate(async::result<frg::expected<protocols::fs::Error>> (*f)(void *object,
			size_t size)) {
		truncate = f;
		return *this;
	}
	constexpr FileOperations &withFallocate(async::result<frg::expected<protocols::fs::Error>> (*f)(void *object,
			int64_t offset, size_t size)) {
		fallocate = f;
		return *this;
	}
	constexpr FileOperations &withIoctl(async::result<void> (*f)(void *object,
			uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation)) {
		ioctl = f;
		return *this;
	}
	constexpr FileOperations &withFlock(async::result<protocols::fs::Error> (*f)(void *object,
			int flags)) {
		flock = f;
		return *this;
	}
	constexpr FileOperations &withBind(async::result<Error> (*f)(void *object,
			helix_ng::CredentialsView , const void *addr_ptr, size_t addr_length)) {
		bind = f;
		return *this;
	}
	constexpr FileOperations &withConnect(async::result<Error> (*f)(void *object,
			helix_ng::CredentialsView , const void *addr_ptr, size_t addr_length)) {
		connect = f;
		return *this;
	}
	constexpr FileOperations &withSockname(async::result<size_t> (*f)(void *object,
			void *addr_ptr, size_t max_addr_length)) {
		sockname = f;
		return *this;
	}
	constexpr FileOperations &withListen(async::result<Error> (*f)(void *object)) {
		listen = f;
		return *this;
	}
	constexpr FileOperations& withAccept(async::result<frg::expected<Error, AcceptResult>> (*f)(void *object)) {
		accept = f;
		return *this;
	}

	constexpr FileOperations &withPeername(async::result<frg::expected<Error, size_t>> (*f)(void *object,
			void *addr_ptr, size_t max_addr_length)) {
		peername = f;
		return *this;
	}

	async::result<SeekResult> (*seekAbs)(void *object, int64_t offset) = nullptr;
	async::result<SeekResult> (*seekRel)(void *object, int64_t offset) = nullptr;
	async::result<SeekResult> (*seekEof)(void *object, int64_t offset) = nullptr;
	async::result<ReadResult> (*read)(void *object, helix_ng::CredentialsView credentials,
			void *buffer, size_t length, async::cancellation_token cancellation) = nullptr;
	async::result<ReadResult> (*pread)(void *object, int64_t offset, helix_ng::CredentialsView credentials,
			void *buffer, size_t length) = nullptr;
	async::result<frg::expected<protocols::fs::Error, size_t>> (*write)(void *object, helix_ng::CredentialsView credentials,
			const void *buffer, size_t length) = nullptr;
	async::result<frg::expected<protocols::fs::Error, size_t>> (*pwrite)(void *object, int64_t offset, helix_ng::CredentialsView credentials,
			const void *buffer, size_t length) = nullptr;
	async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> (*readEntries)(void *object) = nullptr;
	async::result<helix::BorrowedDescriptor>(*accessMemory)(void *object) = nullptr;
	async::result<frg::expected<protocols::fs::Error>> (*truncate)(void *object, size_t size) = nullptr;
	async::result<frg::expected<protocols::fs::Error>> (*fallocate)(void *object, int64_t offset, size_t size) = nullptr;
	async::result<void> (*ioctl)(void *object, uint32_t id, helix_ng::RecvInlineResult req,
			helix::UniqueLane conversation) = nullptr;
	async::result<protocols::fs::Error> (*flock)(void *object, int flags) = nullptr;
	async::result<frg::expected<Error, PollWaitResult>>
	(*pollWait)(void *object, uint64_t sequence, int mask,
			async::cancellation_token cancellation) = nullptr;
	async::result<frg::expected<Error, PollStatusResult>>
	(*pollStatus)(void *object) = nullptr;
	async::result<Error> (*bind)(void *object, helix_ng::CredentialsView credentials,
			const void *addr_ptr, size_t addr_length) = nullptr;
	async::result<Error> (*listen)(void *object) = nullptr;
	async::result<Error> (*connect)(void *object, helix_ng::CredentialsView credentials,
			const void *addr_ptr, size_t addr_length) = nullptr;
	async::result<frg::expected<Error, AcceptResult>> (*accept)(void *object) = nullptr;
	async::result<size_t> (*sockname)(void *object, void *addr_ptr, size_t max_addr_length) = nullptr;
	async::result<int> (*getFileFlags)(void *object) = nullptr;
	async::result<void> (*setFileFlags)(void *object, int flags) = nullptr;
	async::result<RecvResult> (*recvMsg)(void *object, helix_ng::CredentialsView creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) = nullptr;
	async::result<frg::expected<protocols::fs::Error, size_t>> (*sendMsg)(void *object, helix_ng::CredentialsView creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size,
			std::vector<uint32_t> fds, struct ucred ucreds) = nullptr;
	async::result<frg::expected<Error, size_t>> (*peername)(void *object, void *addr_ptr, size_t max_addr_length) = nullptr;
	async::result<frg::expected<Error, int>> (*getSeals)(void *object) = nullptr;
	async::result<frg::expected<Error, int>> (*addSeals)(void *object, int seals) = nullptr;
	async::result<frg::expected<Error>> (*setSocketOption)(void *object, int layer, int number, std::vector<char> optbuf) = nullptr;
	async::result<frg::expected<Error>> (*getSocketOption)(void *object, helix_ng::CredentialsView creds,
			int layer, int number, std::vector<char> &optbuf) = nullptr;
	async::result<Error> (*shutdown)(void *object, int how);

	bool logRequests = false;
};

struct StatusPageProvider {
	StatusPageProvider();

	helix::BorrowedDescriptor getMemory() {
		return _memory;
	}

	void update(uint64_t sequence, int status);

private:
	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
};

struct NodeOperations {
	async::result<FileStats> (*getStats)(std::shared_ptr<void> object);

	async::result<frg::expected<protocols::fs::Error, GetLinkResult>>
	(*getLink)(std::shared_ptr<void> object, std::string name);

	async::result<std::expected<GetLinkResult, protocols::fs::Error>> (*link)(std::shared_ptr<void> object,
			std::string name, int64_t ino);

	// unlink() and rmdir() return the mutation serial of the directory.
	async::result<std::expected<uint64_t, protocols::fs::Error>> (*unlink)(std::shared_ptr<void> object,
			std::string name);

	async::result<std::expected<uint64_t, protocols::fs::Error>> (*rmdir)(std::shared_ptr<void> object,
			std::string name);

	async::result<OpenResult> (*open)(std::shared_ptr<void> object, bool write, bool read, bool append);

	async::result<std::expected<std::string, protocols::fs::Error>> (*readSymlink)(std::shared_ptr<void> object);

	async::result<std::expected<MkdirResult, protocols::fs::Error>> (*mkdir)(std::shared_ptr<void> object, std::string name, uid_t uid, gid_t gid, mode_t mode);

	async::result<std::expected<SymlinkResult, protocols::fs::Error>> (*symlink)(std::shared_ptr<void> object, std::string name,
			std::string path);

	async::result<Error> (*chmod)(std::shared_ptr<void> object, int mode);
	async::result<Error> (*chown)(std::shared_ptr<void> object, std::optional<uid_t>, std::optional<gid_t>);

	async::result<Error> (*utimensat)(std::shared_ptr<void> object,
		std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime);

	async::result<void> (*obstructLink)(std::shared_ptr<void> object, std::string name);
	async::result<TraverseLinksResult> (*traverseLinks)(std::shared_ptr<void> object, std::deque<std::string> path);

	async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>> (*getLinkOrCreate)(std::shared_ptr<void> object,
		std::string name, mode_t mode, bool exclusive, uid_t uid, gid_t gid);
};

async::result<void>
serveFile(helix::UniqueLane lane, void *file, const FileOperations *file_ops);

async::result<void> servePassthrough(helix::UniqueLane lane, smarter::shared_ptr<void> file,
		const FileOperations *file_ops, async::cancellation_token cancellation = {});

async::detached serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops);

} // namespace protocols::fs
