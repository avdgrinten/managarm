#pragma once

#include <fcntl.h>

#include "file.hpp"
#include "fs.hpp"

struct MemoryFile final : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, bool allowSealing)
	: FileWithDefaults{FileKind::unknown,  StructName::get("memfd-file"), mount, link}, _offset{0} {
		if(!allowSealing) {
			_seals = F_SEAL_SEAL;
		}
	}

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t delta, VfsSeek whence) override;
	async::result<frg::expected<protocols::fs::Error>> allocate(int64_t offset, size_t size) override;

	async::result<frg::expected<protocols::fs::Error>> truncate(size_t size) override;

	async::result<frg::expected<protocols::fs::Error, int>> getSeals() override;
	async::result<frg::expected<protocols::fs::Error, int>> addSeals(int seals) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length) override;

	async::result<std::expected<size_t, Error>>
	readSome(Process *process, void *data, size_t max_length, async::cancellation_token ct) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	size_t fileSize() const {
		return _fileSize;
	}

private:
	async::result<frg::expected<Error>> _resizeFile(size_t new_size);

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	uint64_t _offset;

	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
	size_t _areaSize = 0;
	size_t _fileSize = 0;
	int _seals = 0;
};

// ----------------------------------------------------------------------------
// MemoryFileLink class.
// ----------------------------------------------------------------------------

struct MemoryFileLink final : FsLink {
private:
	struct PrivateTag { }; // To tag-dispatch to private methods.

public:
	static smarter::shared_ptr<MemoryFileLink, LinkRc> makeMemoryFileLink(int mode) {
		return makeFsShared<MemoryFileLink>(PrivateTag{}, mode);
	}

	MemoryFileLink(PrivateTag, int mode)
	: node_{makeFsShared<MemoryFileNode>(mode)} { }

	void setFile(smarter::shared_ptr<MemoryFile> file) {
		node_->setFile(std::move(file));
	}

public:
	smarter::shared_ptr<FsNode> getTarget() override {
		return node_;
	}

	smarter::shared_ptr<FsLink, LinkRc> getParent() override {
		return nullptr;
	}

	std::string getName() override {
		throw std::runtime_error("MemoryFileLink has no name");
	}

	std::optional<std::string> getProcFsDescription() override {
		return "memory_inode:unimplemented";
	}

private:
	// MemoryFileLink can never be linked into "real" file systems,
	// hence the can only ever be one link per node.
	struct MemoryFileNode final : FsNode {
		MemoryFileNode(int mode)
		: FsNode{getAnonymousSuperblock()}, mode_{mode} { }

		void setFile(smarter::shared_ptr<MemoryFile> file) {
			file_ = std::move(file);
		}

		VfsType getType() override {
			return VfsType::regular;
		}

		async::result<frg::expected<Error, FileStats>> getStats() override {
			FileStats stats{};
			stats.inodeNumber = 1;
			stats.fileSize = file_->fileSize();
			stats.numLinks = 1;
			stats.mode = mode_;
			stats.uid = 0;
			stats.gid = 0;
			// TODO: Linux returns the current time for all timestamps.
			co_return stats;
		}

	private:
		smarter::shared_ptr<MemoryFile> file_;
		int mode_;
	};

	smarter::shared_ptr<MemoryFileNode> node_;
};
