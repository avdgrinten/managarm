#pragma once

#include "common.hpp"
#include <atomic>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_set>

#include <async/mutex.hpp>

#include <protocols/fs/server.hpp>
#include <protocols/fs/file-locks.hpp>


namespace blockfs {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct BaseFileSystem;

struct BaseInode {
	BaseInode(BaseFileSystem &fs, uint32_t number)
	: fs{fs}, number{number} {  }

	BaseInode(const BaseInode &) = delete;
	BaseInode(BaseInode &&) = delete;
	BaseInode &operator=(const BaseInode &) = delete;
	BaseInode &operator=(BaseInode &&) = delete;

	BaseFileSystem &fs;
	const uint64_t number;

	async::oneshot_event readyEvent;

	std::mutex obstructedLinksMutex;

	// Serializes operations on this inode's metadata and directory entries.
	// inodeMutex MUST be taken for all operations that access these data.
	// inodeMutex may be taken in shared mode for read-only accesses.
	// It must be taken in exclusive mode for modifications.
	// Ordered after BaseFile::mutex -> BaseFileSystem::topologyMutex.
	// When locking multiple inodeMutex at the same time:
	// - Descendents (in the directory hierarchy) are ordered after ancestors.
	// - If there is no ancestry relation, the order is lower inode number first.
	async::shared_mutex inodeMutex;

	FileType fileType;

	FlockManager flockManager;

	// Protected by obstructedLinksMutex.
	std::unordered_set<std::string> obstructedLinks;

	// Serial of the last mutation of this directory's entries.
	// Updated to BaseFileSystem::mutationSerial_ on any mutation.
	// Protected by inodeMutex.
	uint64_t dirSerial = 0;
};

struct BaseFile {
	BaseFile(std::shared_ptr<BaseInode> inode, bool write, bool read, bool append)
	: inode{inode}, write{write}, read{read}, append{append} { }

	BaseFile(const BaseFile &) = delete;
	BaseFile(BaseFile &&) = delete;
	BaseFile &operator=(const BaseFile &) = delete;
	BaseFile &operator=(BaseFile &&) = delete;

	const std::shared_ptr<BaseInode> inode;
	async::shared_mutex mutex;

	uint64_t offset = 0;
	Flock flock;
	bool write;
	bool read;
	bool append;
};

struct BaseFileSystem {
	// TODO(qookie): Ideally, these methods would be a part of the concept
	// instead of being pure virtual methods, but the code that uses these
	// methods currently is generic code that isn't templated on the FS type.
	virtual const protocols::fs::FileOperations *fileOps() = 0;
	virtual const protocols::fs::NodeOperations *nodeOps() = 0;

	virtual std::shared_ptr<BaseInode> accessRoot() = 0;
	virtual std::shared_ptr<BaseInode> accessInode(uint32_t inode) = 0;
	virtual async::result<std::shared_ptr<BaseInode>> createRegular(int uid, int gid, uint32_t parentIno) = 0;
	virtual protocols::fs::FsStats getFsStats() = 0;

	BaseFileSystem() = default;

	// Serializes operations that change the directory hierarchy.
	// topologyMutex MUST be taken for all operations that modify directory entries.
	// For operations that operate on a single directory entry only (i.e., link/unlink/mkdir/rmdir/symlink)
	// taking it in shared mode is enough (assuming that the directory entry is protected by inodeMutex).
	// Taking it in exclusive mode allows an operation that operates on multiple directory entries
	// (i.e., rename()) to exclude all other directory-entry-modifying operations.
	// Ordered after BaseFile::mutex.
	async::shared_mutex topologyMutex;

	// Assigns a fresh mutation serial to the given directories and returns it.
	// Callers must hold the inodeMutex of each directory (exclusive).
	uint64_t recordMutation(std::initializer_list<BaseInode *> dirs) {
		auto serial = mutationSerial_.fetch_add(1, std::memory_order_relaxed) + 1;
		for(auto dir : dirs)
			dir->dirSerial = serial;
		return serial;
	}

	BaseFileSystem(const BaseFileSystem &) = delete;
	BaseFileSystem(BaseFileSystem &&) = delete;
	BaseFileSystem &operator=(const BaseFileSystem &) = delete;
	BaseFileSystem &operator=(BaseFileSystem &&) = delete;

	virtual ~BaseFileSystem() = default;

private:
	std::atomic<uint64_t> mutationSerial_{0};
};

template <typename T>
concept Inode =
	std::derived_from<T, BaseInode>
	&& requires (T ino, std::optional<timespec> ts, size_t sz) {
		{ ino.fileSize() } -> std::same_as<size_t>;
		{ ino.accessMemory() } -> std::same_as<helix::BorrowedDescriptor>;
		{ ino.updateTimes(ts, ts, ts) } -> async::co_awaits_to<protocols::fs::Error>;
		{ ino.resizeFile(sz) } -> async::co_awaits_to<frg::expected<protocols::fs::Error>>;
	};

template <typename T>
concept File =
	std::derived_from<T, BaseFile>;

template <typename T>
concept FileSystem =
	std::derived_from<T, BaseFileSystem>
	&& requires {
		typename T::File;
		typename T::Inode;
	}
	&& File<typename T::File>
	&& Inode<typename T::Inode>;


} // namespace blockfs
