#pragma once

#include <async/cancellation.hpp>
#include <core/id-allocator.hpp>
#include <protocols/fs/server.hpp>

#include "protocols/fs/common.hpp"
#include "vfs.hpp"

struct Process;

namespace sysfs {

struct LinkCompare;
struct Link;
struct DirectoryNode;

struct Attribute;
struct Object;
struct Hierarchy;

// ----------------------------------------------------------------------------
// FS data structures.
// This API is only intended for private use.
// ----------------------------------------------------------------------------

struct SysfsSuperblock final : FsSuperblock {
public:
	SysfsSuperblock() {
		deviceMinor_ = getUnnamedDeviceIdAllocator().allocate();
	}

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
			rename(FsLink *source, FsLink *directory, std::string name) override;
	async::result<frg::expected<Error, FsStats>> getFsStats() override;

	std::string getFsType() override {
		return "sysfs";
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

	id_allocator<uint64_t> &inodeAllocator() {
		return inodeAllocator_;
	}

private:
	id_allocator<uint64_t> inodeAllocator_{1};
	unsigned int deviceMinor_;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const smarter::shared_ptr<Link, LinkRc> &a, const smarter::shared_ptr<Link, LinkRc> &b) const;
	bool operator() (const smarter::shared_ptr<Link, LinkRc> &link, const std::string &name) const;
	bool operator() (const std::string &name, const smarter::shared_ptr<Link, LinkRc> &link) const;
};

struct AttributeFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<AttributeFile> file);

	explicit AttributeFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link);

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override;

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) override;

	async::result<std::expected<size_t, Error>>
	pread(Process *, int64_t offset, void *buffer, size_t length) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	bool _cached;
	std::string _buffer;
	size_t _offset;
};

struct DirectoryFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link);

	void handleClose() override;

	FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;
	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	DotEntriesPhase _dots = DotEntriesPhase::dot;
	std::set<smarter::shared_ptr<Link, LinkRc>, LinkCompare>::iterator _iter;
};

struct Link final : FsLink {
	explicit Link(smarter::shared_ptr<FsNode> target);

	explicit Link(smarter::shared_ptr<FsLink, LinkRc> owner,
			std::string name, smarter::shared_ptr<FsNode> target);

	smarter::shared_ptr<FsLink, LinkRc> getParent() override;
	std::string getName() override;
	smarter::shared_ptr<FsNode> getTarget() override;

private:
	smarter::shared_ptr<FsLink, LinkRc> _owner;
	std::string _name;
	smarter::shared_ptr<FsNode> _target;
};

struct AttributeNode final : FsNode {
	friend struct AttributeFile;

	AttributeNode(Object *object, Attribute *attr);
	~AttributeNode() {
		static_cast<SysfsSuperblock *>(superblock())->inodeAllocator().free(inode_);
	}

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;

private:
	Object *_object;
	Attribute *_attr;
	uint64_t inode_;
};

struct SymlinkNode final : FsNode {
	SymlinkNode(std::weak_ptr<Object> target);
	~SymlinkNode()  {
		static_cast<SysfsSuperblock *>(superblock())->inodeAllocator().free(inode_);
	}

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	expected<std::string> readSymlink(FsLink *link, Process *process) override;

private:
	std::weak_ptr<Object> _target;
	uint64_t inode_;
};

struct DirectoryNode final : FsNode {
	friend struct DirectoryFile;

	static smarter::shared_ptr<Link, LinkRc> createRootDirectory();

	DirectoryNode();
	~DirectoryNode() {
		static_cast<SysfsSuperblock *>(superblock())->inodeAllocator().free(inode_);
	}

	smarter::shared_ptr<Link, LinkRc> directMkattr(FsLink *parent, Object *object, Attribute *attr);
	smarter::shared_ptr<Link, LinkRc> directMklink(FsLink *parent, std::string name, std::weak_ptr<Object> target);
	smarter::shared_ptr<Link, LinkRc> directMkdir(FsLink *parent, std::string name);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *parent, std::string name) override;

private:
	std::set<smarter::shared_ptr<Link, LinkRc>, LinkCompare> _entries;
	uint64_t inode_;
};

// ----------------------------------------------------------------------------
// Object abstraction.
// Subsystems should use this API to manage sysfs.
// ----------------------------------------------------------------------------

struct Attribute {
	Attribute(std::string name, bool writable);
	Attribute(std::string name, bool writable, size_t size);

	virtual ~Attribute() = default;

public:
	const std::string &name() {
		return _name;
	}

	bool writable() {
		return _writable;
	}

	size_t size() {
		return _size;
	}

	virtual async::result<frg::expected<Error, std::string>> show(Object *object) = 0;
	virtual async::result<Error> store(Object *object, std::string data);
	virtual async::result<frg::expected<Error, helix::UniqueDescriptor>> accessMemory(Object *object);

protected:
	size_t _size = 4096;
private:
	const std::string _name;
	bool _writable;
};

// Object corresponds to Linux kobjects.
struct Object {
	Object(std::shared_ptr<Object> parent, std::string name);
	virtual ~Object() = default;

	const std::string &name() {
		return _name;
	}

	smarter::shared_ptr<DirectoryNode> directoryNode();

	const smarter::shared_ptr<Link, LinkRc> &dirLink() {
		return _dirLink;
	}

	void realizeAttribute(Attribute *attr);
	void createSymlink(std::string name, std::shared_ptr<Object> target);

	virtual std::optional<std::string> getClassPath();
	void addObject();

	std::unordered_map<std::string, std::shared_ptr<sysfs::Object>> &classDirectories() {
		return classDirectories_;
	}
private:
	std::shared_ptr<Object> _parent;
	std::string _name;

	smarter::shared_ptr<Link, LinkRc> _dirLink;

	std::unordered_map<std::string, std::shared_ptr<sysfs::Object>> classDirectories_;
};

// Hierarchy corresponds to Linux ksets.
struct Hierarchy {

};

} // namespace sysfs

smarter::shared_ptr<FsLink, LinkRc> getSysfs();
