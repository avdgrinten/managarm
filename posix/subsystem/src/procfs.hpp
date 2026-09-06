#pragma once

#include <async/cancellation.hpp>
#include <protocols/fs/server.hpp>

#include "vfs.hpp"

struct Process;
struct ThreadGroup;
struct FileDescriptor;

namespace procfs {

struct LinkCompare;
struct Link;
struct DirectoryNode;

// ----------------------------------------------------------------------------
// FS data structures.
// This API is only intended for private use.
// ----------------------------------------------------------------------------

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const smarter::shared_ptr<Link, LinkRc> &a, const smarter::shared_ptr<Link, LinkRc> &b) const;
	bool operator() (const smarter::shared_ptr<Link, LinkRc> &link, const std::string &name) const;
	bool operator() (const std::string &name, const smarter::shared_ptr<Link, LinkRc> &link) const;
};

struct RegularFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<RegularFile> file);

	explicit RegularFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link);

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override;

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override;

	async::result<frg::expected<Error, PollWaitResult>> pollWait(Process *,
		uint64_t sequence, int mask,
		async::cancellation_token cancellation = {}) override;

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
	void unlinkSelf();

private:
	smarter::shared_ptr<FsLink, LinkRc> _owner;
	std::string _name;
	smarter::shared_ptr<FsNode> _target;
};

struct RegularNode : FsNode {
	friend struct RegularFile;

	RegularNode();
	virtual ~RegularNode() = default;

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;

protected:
	virtual async::result<std::expected<std::string, Error>> show(Process *) = 0;
	virtual async::result<void> store(std::string buffer) = 0;

	async::result<frg::expected<Error, FileStats>> getStatsInternal(ThreadGroup *);
};

struct SuperBlock final : FsSuperblock {
public:
	SuperBlock() {
		deviceMinor_ = getUnnamedDeviceIdAllocator().allocate();
	}

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *) override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
			rename(FsLink *source, FsLink *directory, std::string name) override;
	async::result<frg::expected<Error, FsStats>> getFsStats() override;

	std::string getFsType() override {
		return "proc";
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

private:
	unsigned int deviceMinor_;
};

struct DirectoryNode final : FsNode {
	friend struct DirectoryFile;

	static smarter::shared_ptr<Link, LinkRc> createRootDirectory();

	DirectoryNode();

	smarter::shared_ptr<Link, LinkRc> directMkregular(FsLink *parent, std::string name,
			smarter::shared_ptr<RegularNode> regular);

	smarter::shared_ptr<Link, LinkRc> directMknode(FsLink *parent, std::string name,
			smarter::shared_ptr<FsNode> node);
	smarter::shared_ptr<Link, LinkRc> directMkdir(FsLink *parent, std::string name);

	smarter::shared_ptr<Link, LinkRc> createProcTaskDirectory(FsLink *parent, Process *process);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> link(FsLink *parent, std::string name,
			smarter::shared_ptr<FsNode> target) override;

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *parent, std::string name) override;
	async::result<frg::expected<Error>> unlink(std::string name) override;

	Error directUnlink(std::string name);

private:
	smarter::shared_ptr<Link, LinkRc> createProcDirectory(FsLink *parent, Process *process);

	std::set<smarter::shared_ptr<Link, LinkRc>, LinkCompare> _entries;
};

struct LinkNode : FsNode {
	LinkNode();

	VfsType getType() override {
		return VfsType::symlink;
	}

protected:
	async::result<frg::expected<Error, FileStats>> getStatsInternal(Process *);
};

struct SelfLink final : LinkNode {
	SelfLink() = default;

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
};

struct SelfThreadLink final : LinkNode {
	SelfThreadLink() = default;

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
};

struct ExeLink final : LinkNode {
	ExeLink(Process *process);

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct RootLink final : LinkNode {
	RootLink(Process *process);

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct CwdLink final : LinkNode {
	CwdLink(Process *process);

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct MountsLink final : LinkNode {
	MountsLink() = default;

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
};

struct MapNode final : RegularNode {
	MapNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct UptimeNode final : RegularNode {
	UptimeNode() {}

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct OstypeNode final : RegularNode {
	OstypeNode() {}

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct OsreleaseNode final : RegularNode {
	OsreleaseNode() {}

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct ArchNode final : RegularNode {
	ArchNode() {}

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct BootIdNode final : RegularNode {
	BootIdNode();

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
private:
	std::string bootId_;
};

struct HostnameNode final : RegularNode {
	HostnameNode() {}

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct CommNode final : RegularNode {
	CommNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct StatNode final : RegularNode {
	StatNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct StatmNode final : RegularNode {
	StatmNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct ProcessStatusNode final : RegularNode {
	ProcessStatusNode(std::weak_ptr<ThreadGroup> tg)
	: _tg(tg)
	{ }

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<ThreadGroup> _tg;
};

struct StatusNode final : RegularNode {
	StatusNode(std::weak_ptr<Process> process)
	: _process(process)
	{ }

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct FdDirectoryFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<FdDirectoryFile> file);

	explicit FdDirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, Process *process);

	void handleClose() override;

	FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	std::weak_ptr<Process> _process;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	DotEntriesPhase _dots = DotEntriesPhase::dot;
	std::unordered_map<int, FileDescriptor> _fileTable;
	std::unordered_map<int, FileDescriptor>::const_iterator _iter;
};

struct CgroupNode final : RegularNode {
	CgroupNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
};

struct FdDirectoryNode final : FsNode {
public:
	friend DirectoryNode;

	explicit FdDirectoryNode(Process *process);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *parent, std::string name) override;
private:
	std::weak_ptr<Process> _process;
};

struct SymlinkNode final : LinkNode {
	SymlinkNode(Process *, std::shared_ptr<MountView>, smarter::weak_ptr<FsLink, LinkRc>);

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	std::weak_ptr<Process> _process;
	std::shared_ptr<MountView> _mount;
	smarter::weak_ptr<FsLink, LinkRc> _link;
};

struct MountsNode final : RegularNode {
	MountsNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;

private:
	std::weak_ptr<Process> _process;
};

struct MountInfoNode final : RegularNode {
	MountInfoNode(Process *process);

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;

private:
	std::weak_ptr<Process> _process;
};

struct FdInfoDirectoryNode final : FsNode {
public:
	friend DirectoryNode;

	explicit FdInfoDirectoryNode(Process *process);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *parent, std::string name) override;
private:
	std::weak_ptr<Process> _process;
};

struct FdInfoDirectoryFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<FdInfoDirectoryFile> file);

	explicit FdInfoDirectoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, Process* process);

	void handleClose() override;

	FutureMaybe<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	std::weak_ptr<Process> _process;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	DotEntriesPhase _dots = DotEntriesPhase::dot;
	std::unordered_map<int, FileDescriptor> _fileTable;
	std::unordered_map<int, FileDescriptor>::const_iterator _iter;
};

struct FdInfoNode final : RegularNode {
	FdInfoNode(std::shared_ptr<MountView> mountView, smarter::shared_ptr<File, FileHandle> file)
	: mountView_{std::move(mountView)}, file_{std::move(file)} {}

	async::result<std::expected<std::string, Error>> show(Process *) override;
	async::result<void> store(std::string) override;
private:
	std::shared_ptr<MountView> mountView_;
	smarter::shared_ptr<File, FileHandle> file_;
};

} // namespace procfs

smarter::shared_ptr<FsLink, LinkRc> getProcfs();
