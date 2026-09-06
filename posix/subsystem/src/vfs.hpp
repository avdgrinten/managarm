#pragma once

#include <string.h>
#include <iostream>
#include <set>
#include <deque>

#include <async/result.hpp>
#include <hel.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "file.hpp"
#include "fs.hpp"

using ResolveFlags = uint32_t;

inline constexpr ResolveFlags resolveDontFollow = (1 << 1);

// Resolution stops before resolving the last component, ignoring trailing slashes.
// In particular, both for a/b/c and for a/b/c/, resolution stops before c.
// If a trailing slash is present, the behavior depends on the mutually exclusive flags
// resolveNoTrailingSlash, resolveOpenCreate and resolveCreatesNonDirectory (see below).
// Without any of these flags (mkdir, rename of a directory):
// a trailing slash after successful prefix resolution returns success.
// Note that the trailing slash handling is modelled after Linux, not POSIX.
inline constexpr ResolveFlags resolvePrefix = (1 << 4);

// The caller operates on an existing non-directory (rename with a non-directory source).
// Requires resolvePrefix.
// A trailing slash after successful prefix resolution fails with ENOTDIR.
inline constexpr ResolveFlags resolveNoTrailingSlash = (1 << 2);
// The caller is open() with O_CREAT.
// Requires resolvePrefix.
// A trailing slash after successful prefix resolution fails with EISDIR.
inline constexpr ResolveFlags resolveOpenCreate = (1 << 5);
// The caller creates a non-directory leaf (mkfifo, mknod, link, symlink, bind).
// Requires resolvePrefix.
// A trailing slash after successful prefix resolution checks the last component (without resolving symlinks) and then fails.
// If the last component does not exist, resolution fails with ENOENT. Otherwise, it fails with EEXIST.
inline constexpr ResolveFlags resolveCreatesNonDirectory = (1 << 6);

using ViewPathPair = std::pair<std::shared_ptr<MountView>, smarter::shared_ptr<FsLink, LinkRc>>;

struct ViewPath : public ViewPathPair {
	ViewPath() = default;

	ViewPath(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
	: ViewPathPair(mount, link) {}

	// smarter::shared_ptr does not compare, hence std::pair's operator== does not apply.
	bool operator== (const ViewPath &other) const {
		return first == other.first && second.get() == other.second.get();
	}

	std::string getPath(ViewPath root) const;
};

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct MountView : std::enable_shared_from_this<MountView> {
	static std::shared_ptr<MountView> createRoot(smarter::shared_ptr<FsLink, LinkRc> origin);

	// TODO: This is an implementation detail that could be hidden.
	explicit MountView(uint64_t mountId, std::shared_ptr<MountView> parent, smarter::shared_ptr<FsLink, LinkRc> anchor,
			smarter::shared_ptr<FsLink, LinkRc> origin, ViewPath deviceLink)
	: mountId_{mountId}, _parent{std::move(parent)}, _anchor{std::move(anchor)}, _origin{std::move(origin)},
		deviceLink_{std::move(deviceLink)}
	{ }

	uint64_t mountId() const {
		return mountId_;
	}

	std::shared_ptr<MountView> getParent() const;
	smarter::shared_ptr<FsLink, LinkRc> getAnchor() const;
	smarter::shared_ptr<FsLink, LinkRc> getOrigin() const;
	ViewPath getDevice() const {
		return deviceLink_;
	}

	async::result<void> mount(smarter::shared_ptr<FsLink, LinkRc> anchor, smarter::shared_ptr<FsLink, LinkRc> origin, ViewPath deviceLink = {});

	std::shared_ptr<MountView> getMount(smarter::shared_ptr<FsLink, LinkRc> link) const;

	struct Compare {
		struct is_transparent { };

		bool operator() (const std::shared_ptr<MountView> &a,
				const smarter::shared_ptr<FsLink, LinkRc> &b) const {
			return a->getAnchor().get() < b.get();
		}
		bool operator() (const smarter::shared_ptr<FsLink, LinkRc> &a,
				const std::shared_ptr<MountView> &b) const {
			return a.get() < b->getAnchor().get();
		}

		bool operator() (const std::shared_ptr<MountView> &a,
				const std::shared_ptr<MountView> &b) const {
			return a->getAnchor().get() < b->getAnchor().get();
		}
	};

	const std::set<std::shared_ptr<MountView>, Compare> &mounts() const {
		return _mounts;
	}

private:

	uint64_t mountId_;
	std::shared_ptr<MountView> _parent;
	smarter::shared_ptr<FsLink, LinkRc> _anchor;
	smarter::shared_ptr<FsLink, LinkRc> _origin;
	ViewPath deviceLink_;
	std::set<std::shared_ptr<MountView>, Compare> _mounts;
};

struct PathResolver {
	void setup(ViewPath root, ViewPath workdir, std::string string, Process *process);

	async::result<frg::expected<protocols::fs::Error, void>> resolve(ResolveFlags flags = 0);

	bool hasComponent() {
		return !_components.empty();
	}

	std::string nextComponent() {
		assert(!_components.empty());
		return _components.front();
	}

	std::shared_ptr<MountView> currentView() {
		return _currentPath.first;
	}

	smarter::shared_ptr<FsLink, LinkRc> currentLink() {
		return _currentPath.second;
	}

private:
	ViewPath _rootPath;
	Process *_process;

	std::deque<std::string> _components;
	bool _trailingSlash;
	ViewPath _currentPath;
};

async::result<void> populateRootView();

ViewPath rootPath();

// TODO: Switch to PathResolver instead of using this function.
async::result<frg::expected<protocols::fs::Error, ViewPath>> resolve(ViewPath root, ViewPath workdir,
		std::string name, Process *process, ResolveFlags flags = 0);

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(ViewPath root,
		ViewPath workdir, std::string name, Process *process, ResolveFlags resolve_flags = 0,
		SemanticFlags semantic_flags = 0);
