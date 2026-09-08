#pragma once

#include <async/cancellation.hpp>
#include <protocols/fs/server.hpp>

#include "vfs.hpp"

struct Process;
struct FileDescriptor;

namespace cgroupfs {

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
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ct) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

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

	async::result<Error> chmod(int) override {
		co_return Error::success;
	}

protected:
	virtual async::result<std::string> show() = 0;
	virtual async::result<void> store(std::string buffer) = 0;
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
		return "cgroup2";
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
	async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	mkdir(FsLink *parent, Process *, std::string name, mode_t mode) override;

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> link(FsLink *parent, std::string name,
			smarter::shared_ptr<FsNode> target) override;

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> getLink(FsLink *parent, std::string name) override;
	async::result<frg::expected<Error>> unlink(std::string name) override;

	async::result<Error> chmod(int) override {
		co_return Error::success;
	}

	smarter::shared_ptr<Link, LinkRc> createCgroupDirectory(FsLink *parent, std::string name);
	void createCgroupFiles(FsLink *parent);

private:
	std::set<smarter::shared_ptr<Link, LinkRc>, LinkCompare> _entries;
};

struct ProcsNode final : RegularNode {
	ProcsNode() {}

	async::result<std::string> show() override;
	async::result<void> store(std::string) override;
private:
	// Process *_process;
};

struct ControllersNode final : RegularNode {
	ControllersNode() {}

	async::result<std::string> show() override;
	async::result<void> store(std::string) override;
private:
	// Process *_process;
};

struct LinkNode : FsNode {
	LinkNode();

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix cgroupfs LinkNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}


	VfsType getType() override {
		return VfsType::symlink;
	}
};

} // namespace cgroupfs

smarter::shared_ptr<FsLink, LinkRc> getCgroupfs();
