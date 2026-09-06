#pragma once

#include <concepts>
#include <iostream>
#include <set>
#include <deque>
#include <unordered_map>

#include <async/result.hpp>
#include <core/id-allocator.hpp>
#include <hel.h>
#include <smarter.hpp>
#include <sys/types.h>

#include <fcntl.h>

#include "file.hpp"
#include "link-rc.hpp"

using DeviceId = std::pair<int, int>;

enum class VfsType {
	// null means that the file type is undefined in stat().
	// Avoid using null in favor of a more appropriate type.
	null, directory, regular, symlink, charDevice, blockDevice, socket, fifo
};

struct FileStats {
	uint64_t inodeNumber;
	int numLinks;
	uint64_t fileSize;
	uint32_t mode;
	int uid, gid;
	uint64_t atimeSecs, atimeNanos;
	uint64_t mtimeSecs, mtimeNanos;
	uint64_t ctimeSecs, ctimeNanos;
};

// Internal representation of struct statfs.
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


// Forward declarations.
struct FsLink;
struct FsNode;
struct ViewPath;

// ----------------------------------------------------------------------------
// FsLink class.
// ----------------------------------------------------------------------------

// Represents a directory entry on an actual file system.
// FsLinks know their parent FsLink. Hence, FsLinks form a rooted directory tree.
// The chain of parents terminates at the root of the superblock, not at the root of the VFS.
struct FsLink {
protected:
	~FsLink() = default;

public:
	// Returns the link of the directory that this link lives in.
	// Null for the root of a mount and for anonymous links.
	virtual smarter::shared_ptr<FsLink, LinkRc> getParent() = 0;

	// Name of the link. Empty for the root link.
	virtual std::string getName() = 0;

	// Target of the link:
	// directory entry (getParent(), getName()) points to getTarget().
	virtual smarter::shared_ptr<FsNode> getTarget() = 0;

	virtual async::result<frg::expected<Error>> obstruct();

	smarter::shared_ptr<FsNode> getParentNode() {
		auto parent = getParent();
		if(!parent)
			return nullptr;
		return parent->getTarget();
	}

	virtual std::optional<std::string> getProcFsDescription();

	// Only to be called by makeFsShared().
	void setupSelfPtr(smarter::borrowed_ptr<FsLink, LinkRc> ptr) {
		selfPtr_ = ptr;
	}

	smarter::shared_ptr<FsLink, LinkRc> sharedFromThis() {
		assert(selfPtr_);
		return selfPtr_.lock();
	}

private:
	smarter::borrowed_ptr<FsLink, LinkRc> selfPtr_;
};

struct FsSuperblock {
protected:
	~FsSuperblock() = default;

public:
	virtual FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *) = 0;

	virtual async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
			rename(FsLink *source, FsLink *directory, std::string name) = 0;
	virtual async::result<frg::expected<Error, FsStats>> getFsStats() = 0;
	virtual std::string getFsType() = 0;
	virtual dev_t deviceNumber() = 0;
};

FsSuperblock *getAnonymousSuperblock();

// This is used to allocate device IDs for non-device-based file systems such as tmpfs.
id_allocator<unsigned int> &getUnnamedDeviceIdAllocator();

// ----------------------------------------------------------------------------
// FsObserver class.
// ----------------------------------------------------------------------------

struct FsObserver {
protected:
	~FsObserver() = default;

public:
	static constexpr uint32_t deleteEvent = 1;
	static constexpr uint32_t deleteSelfEvent = 2;
	static constexpr uint32_t createEvent = 4;
	static constexpr uint32_t modifyEvent = 8;
	static constexpr uint32_t accessEvent = 16;
	static constexpr uint32_t closeWriteEvent = 32;
	static constexpr uint32_t closeNoWriteEvent = 64;
	static constexpr uint32_t ignoredEvent = 128;

	virtual void observeNotification(uint32_t events,
			const std::string &name, uint32_t cookie, bool isDir) = 0;
};

// ----------------------------------------------------------------------------
// FsNode class.
// ----------------------------------------------------------------------------

using SemanticFlags = uint32_t;
inline constexpr SemanticFlags semanticNonBlock = 1;
inline constexpr SemanticFlags semanticRead = 2;
inline constexpr SemanticFlags semanticWrite = 4;
inline constexpr SemanticFlags semanticAppend = 8;

// Represents an inode on an actual file system (i.e. not in the VFS).
struct FsNode {
	using DefaultOps = uint32_t;
	static inline constexpr DefaultOps defaultSupportsObservers = 1 << 1;

	FsNode(FsSuperblock *superblock, DefaultOps default_ops = 0)
	: _superblock{superblock}, _defaultOps{default_ops} {
		assert(_superblock);
	}

	FsSuperblock *superblock() {
		return _superblock;
	}

protected:
	~FsNode() = default;

public:
	virtual VfsType getType() = 0;

	// TODO: This should be async.
	virtual async::result<frg::expected<Error, FileStats>> getStats();

	virtual void addObserver(std::shared_ptr<FsObserver> observer);

	virtual void removeObserver(FsObserver *observer);

	//! Get an existing link or create one (directories only).
	virtual async::result<std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error>>
	getLinkOrCreate(FsLink *parent, Process *, std::string name, mode_t mode,
			bool exclusive = false);

	//! Resolves a file in a directory (directories only).
	virtual async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *parent, std::string name);

	//! Links an existing node to this directory (directories only).
	virtual async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> link(FsLink *parent, std::string name,
			smarter::shared_ptr<FsNode> target);

	//! Creates a new directory (directories only).
	virtual async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	mkdir(FsLink *parent, Process *, std::string name, mode_t mode);

	//! Creates a new symlink (directories only).
	virtual async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	symlink(FsLink *parent, std::string name, std::string path);

	//! Creates a new device file (directories only).
	virtual async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mkdev(FsLink *parent, std::string name,
			VfsType type, DeviceId id);

	virtual async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mkfifo(FsLink *parent, std::string name, mode_t mode);

	virtual async::result<frg::expected<Error>> unlink(std::string name);

	virtual async::result<frg::expected<Error>> rmdir(std::string name);

	//! Opens the file (regular files only).
	// TODO: Move this to the link instead of the inode?
	virtual async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *process, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags);

	// Reads the target of a symlink (symlinks only).
	// Returns illegalOperationTarget() by default.
	virtual expected<std::string> readSymlink(FsLink *link, Process *process);

	//! Read the major/minor device number (devices only).
	virtual DeviceId readDevice();

	// Changes permissions on a node
	virtual async::result<Error> chmod(int mode);

	// Changes ownership of a node
	virtual async::result<std::expected<void, Error>> chown(std::optional<uid_t> uid, std::optional<gid_t> gid);

	// Changes timestamps on a node
	virtual async::result<Error> utimensat(std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime);

	// Creates an socket
	virtual async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> mksocket(FsLink *parent, std::string name, mode_t mode, uid_t uid, gid_t gid);

	// Recursive path traversal
	virtual bool hasTraverseLinks();
	virtual async::result<frg::expected<Error, std::pair<smarter::shared_ptr<FsLink, LinkRc>, size_t>>> traverseLinks(FsLink *parent, std::deque<std::string> path);

	void notifyObservers(uint32_t inotifyEvents, const std::string &name, uint32_t cookie, bool isDir = false);

	// Only to be called by makeFsShared().
	void setupSelfPtr(smarter::borrowed_ptr<FsNode> ptr) {
		selfPtr_ = ptr;
	}

	smarter::shared_ptr<FsNode> sharedFromThis() {
		assert(selfPtr_);
		return selfPtr_.lock();
	}

	protocols::fs::FlockManager flockManager;

private:
	smarter::borrowed_ptr<FsNode> selfPtr_;
	FsSuperblock *_superblock;
	DefaultOps _defaultOps;

	// Observers, for example for inotify.
	std::unordered_map<FsObserver *, std::shared_ptr<FsObserver>> _observers;
};

// Allocates an FsLink that the given reclaimer keeps alive while it is unreferenced.
// Passing a null reclaimer yields an ordinary link that dies with its last reference.
template<typename T, typename... Args>
requires std::derived_from<T, FsLink>
smarter::shared_ptr<T, LinkRc> makeReclaimableLink(LinkReclaimer *reclaimer, Args &&...args) {
	auto meta = new LinkMetaObject<T>{reclaimer, std::forward<Args>(args)...};
	auto ptr = smarter::shared_ptr<T, LinkRc>{smarter::adopt_rc, meta->get(), LinkRc{meta}};
	ptr->setupSelfPtr(ptr);
	return ptr;
}

// Allocates an FsLink or FsNode and installs the self-pointer that sharedFromThis() returns.
template<typename T, typename... Args>
requires std::derived_from<T, FsLink> || std::derived_from<T, FsNode>
auto makeFsShared(Args &&...args) {
	if constexpr (std::derived_from<T, FsLink>) {
		return makeReclaimableLink<T>(nullptr, std::forward<Args>(args)...);
	}else{
		auto ptr = smarter::make_shared<T>(std::forward<Args>(args)...);
		ptr->setupSelfPtr(ptr);
		return ptr;
	}
}

// ----------------------------------------------------------------------------
// SpecialLink class.
// ----------------------------------------------------------------------------

// This class can be used to construct FsLinks for anonymous special files
// such as epoll, signalfd, timerfd, etc.
struct SpecialLink final : FsLink {
private:
	struct PrivateTag { }; // To tag-dispatch to private methods.

public:
	static smarter::shared_ptr<SpecialLink, LinkRc> makeSpecialLink(VfsType fileType, int mode) {
		return makeFsShared<SpecialLink>(PrivateTag{}, fileType, mode);
	}

	SpecialLink(PrivateTag, VfsType fileType, int mode)
	: node_{makeFsShared<SpecialNode>(fileType, mode)} { }

public:
	smarter::shared_ptr<FsNode> getTarget() override {
		return node_;
	}

	smarter::shared_ptr<FsLink, LinkRc> getParent() override {
		return nullptr;
	}

	std::string getName() override {
		throw std::runtime_error("SpecialLink has no name");
	}

	std::optional<std::string> getProcFsDescription() override {
		return "anon_inode:unimplemented";
	}

private:
	// SpecialLinks can never be linked into "real" file systems,
	// hence the can only ever be one link per node.
	struct SpecialNode final : FsNode {
		SpecialNode(VfsType fileType, int mode)
		: FsNode{getAnonymousSuperblock()}, fileType_{fileType}, mode_{mode} { }

		VfsType getType() override {
			return fileType_;
		}

		async::result<frg::expected<Error, FileStats>> getStats() override {
			FileStats stats{};
			// TODO: Allocate an inode number.
			stats.inodeNumber = 1;
			stats.fileSize = 0;
			stats.numLinks = 1;
			stats.mode = mode_;
			stats.uid = 0;
			stats.gid = 0;
			// TODO: Linux returns the current time for all timestamps.
			co_return stats;
		}

	private:
		VfsType fileType_;
		int mode_;
	};

	smarter::shared_ptr<SpecialNode> node_;
};
