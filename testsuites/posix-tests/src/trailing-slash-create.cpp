#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <frg/scope_exit.hpp>

#include "testsuite.hpp"

namespace {

// Per-test scratch directory: the ext2 root on Managarm (to exercise libblockfs,
// which /tmp as tmpfs would bypass), /tmp on hosts where the root is not writable.
std::string make_scratch() {
#if defined(__managarm__)
	std::string tmpl = "/posix-tests-tscreate-XXXXXX";
#else
	std::string tmpl = "/tmp/posix-tests-tscreate-XXXXXX";
#endif
	std::string path(tmpl);
	if(!mkdtemp(path.data()))
		assert(!"mkdtemp() failed");
	return path;
}

void create_file(const std::string &path) {
	int fd = creat(path.c_str(), 0644);
	assert(fd >= 0);
	close(fd);
}

} // namespace

// Each test drives the trailing-slash code paths in the resolver: a missing
// leaf, an existing non-directory leaf and an existing directory leaf. These
// expectations match Linux: for a non-directory creator a trailing slash
// suppresses creation (ENOENT on a missing leaf, EEXIST on any existing one,
// symlinks included); for open(O_CREAT) a trailing slash is always EISDIR.

DEFINE_TEST(mkfifo_trailing_slash, ([] {
	auto dir = make_scratch();
	auto missing = dir + "/missing";
	auto file = dir + "/file";
	auto subdir = dir + "/subdir";
	auto dangling = dir + "/dangling";
	frg::scope_exit cleanup{[&] {
		unlink(file.c_str());
		unlink(dangling.c_str());
		rmdir(subdir.c_str());
		rmdir(dir.c_str());
	}};

	create_file(file);
	assert(mkdir(subdir.c_str(), 0755) == 0);
	assert(symlink("nowhere", dangling.c_str()) == 0);

	errno = 0;
	assert(mkfifo((missing + "/").c_str(), 0644) == -1);
	assert(errno == ENOENT);

	errno = 0;
	assert(mkfifo((file + "/").c_str(), 0644) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(mkfifo((subdir + "/").c_str(), 0644) == -1);
	assert(errno == EEXIST);

	// The leaf symlink counts as existing without being followed.
	errno = 0;
	assert(mkfifo((dangling + "/").c_str(), 0644) == -1);
	assert(errno == EEXIST);
}))

DEFINE_TEST(link_trailing_slash, ([] {
	auto dir = make_scratch();
	auto src = dir + "/src";
	auto missing = dir + "/missing";
	auto file = dir + "/file";
	auto subdir = dir + "/subdir";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		unlink(file.c_str());
		rmdir(subdir.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src);
	create_file(file);
	assert(mkdir(subdir.c_str(), 0755) == 0);

	errno = 0;
	assert(link(src.c_str(), (missing + "/").c_str()) == -1);
	assert(errno == ENOENT);

	errno = 0;
	assert(link(src.c_str(), (file + "/").c_str()) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(link(src.c_str(), (subdir + "/").c_str()) == -1);
	assert(errno == EEXIST);
}))

DEFINE_TEST(symlink_trailing_slash, ([] {
	auto dir = make_scratch();
	auto missing = dir + "/missing";
	auto file = dir + "/file";
	auto subdir = dir + "/subdir";
	frg::scope_exit cleanup{[&] {
		unlink(file.c_str());
		rmdir(subdir.c_str());
		rmdir(dir.c_str());
	}};

	create_file(file);
	assert(mkdir(subdir.c_str(), 0755) == 0);

	errno = 0;
	assert(symlink("target", (missing + "/").c_str()) == -1);
	assert(errno == ENOENT);

	errno = 0;
	assert(symlink("target", (file + "/").c_str()) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(symlink("target", (subdir + "/").c_str()) == -1);
	assert(errno == EEXIST);
}))

DEFINE_TEST(open_creat_trailing_slash, ([] {
	auto dir = make_scratch();
	auto missing = dir + "/missing";
	auto file = dir + "/file";
	auto subdir = dir + "/subdir";
	frg::scope_exit cleanup{[&] {
		unlink(file.c_str());
		rmdir(subdir.c_str());
		rmdir(dir.c_str());
	}};

	create_file(file);
	assert(mkdir(subdir.c_str(), 0755) == 0);

	errno = 0;
	assert(open((missing + "/").c_str(), O_WRONLY | O_CREAT, 0644) == -1);
	assert(errno == EISDIR);

	errno = 0;
	assert(open((file + "/").c_str(), O_WRONLY | O_CREAT, 0644) == -1);
	assert(errno == EISDIR);

	errno = 0;
	assert(open((subdir + "/").c_str(), O_WRONLY | O_CREAT, 0644) == -1);
	assert(errno == EISDIR);

	// Errors on the prefix take precedence over the trailing-slash policy.
	errno = 0;
	assert(open((missing + "/x/").c_str(), O_WRONLY | O_CREAT, 0644) == -1);
	assert(errno == ENOENT);
}))

// rename() is not a creator in the above sense: a trailing slash demands that
// the path names a directory, so a non-directory on either side is ENOTDIR.
DEFINE_TEST(rename_trailing_slash, ([] {
	auto dir = make_scratch();
	auto src = dir + "/src";
	auto missing = dir + "/missing";
	auto file = dir + "/file";
	auto subdir = dir + "/subdir";
	auto srcdir = dir + "/srcdir";
	frg::scope_exit cleanup{[&] {
		unlink(src.c_str());
		unlink(file.c_str());
		rmdir(subdir.c_str());
		rmdir(srcdir.c_str());
		rmdir(dir.c_str());
	}};

	create_file(src);
	create_file(file);
	assert(mkdir(subdir.c_str(), 0755) == 0);

	errno = 0;
	assert(rename(src.c_str(), (missing + "/").c_str()) == -1);
	assert(errno == ENOTDIR);

	errno = 0;
	assert(rename(src.c_str(), (file + "/").c_str()) == -1);
	assert(errno == ENOTDIR);

	errno = 0;
	assert(rename(src.c_str(), (subdir + "/").c_str()) == -1);
	assert(errno == ENOTDIR);

	// A trailing slash on the source is subject to the same rule.
	errno = 0;
	assert(rename((src + "/").c_str(), missing.c_str()) == -1);
	assert(errno == ENOTDIR);

	// None of the failed renames moved anything.
	struct stat st;
	assert(stat(src.c_str(), &st) == 0);
	assert(stat(missing.c_str(), &st) == -1);

	// Renaming a directory is unaffected by the trailing slash.
	assert(rename(subdir.c_str(), (srcdir + "/").c_str()) == 0);
	assert(stat(srcdir.c_str(), &st) == 0);
	assert(S_ISDIR(st.st_mode));
}))

// mkdir() creates a directory anyway, so a trailing slash imposes nothing
// extra: the missing leaf is created and any existing leaf wins with EEXIST.
DEFINE_TEST(mkdir_trailing_slash, ([] {
	auto dir = make_scratch();
	auto missing = dir + "/missing";
	auto file = dir + "/file";
	auto subdir = dir + "/subdir";
	frg::scope_exit cleanup{[&] {
		unlink(file.c_str());
		rmdir(missing.c_str());
		rmdir(subdir.c_str());
		rmdir(dir.c_str());
	}};

	create_file(file);
	assert(mkdir(subdir.c_str(), 0755) == 0);

	assert(mkdir((missing + "/").c_str(), 0755) == 0);
	struct stat st;
	assert(stat(missing.c_str(), &st) == 0);
	assert(S_ISDIR(st.st_mode));

	errno = 0;
	assert(mkdir((file + "/").c_str(), 0755) == -1);
	assert(errno == EEXIST);

	errno = 0;
	assert(mkdir((subdir + "/").c_str(), 0755) == -1);
	assert(errno == EEXIST);
}))

// stat() on a symlink-to-directory with a trailing slash must follow the
// symlink and report the target as a directory.
DEFINE_TEST(stat_symlink_to_dir_trailing_slash, ([] {
	auto dir = make_scratch();
	auto target = dir + "/d";
	auto link = dir + "/l";
	frg::scope_exit cleanup{[&] {
		unlink(link.c_str());
		rmdir(target.c_str());
		rmdir(dir.c_str());
	}};

	assert(mkdir(target.c_str(), 0755) == 0);
	assert(symlink(target.c_str(), link.c_str()) == 0);

	struct stat st;
	assert(stat((link + "/").c_str(), &st) == 0);
	assert(S_ISDIR(st.st_mode));

	// POSIX: a trailing slash forces symlink resolution even for lstat.
	assert(lstat((link + "/").c_str(), &st) == 0);
	assert(S_ISDIR(st.st_mode));
}))
