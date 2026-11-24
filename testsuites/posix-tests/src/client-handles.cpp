#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include <hel.h>
#include <hel-syscalls.h>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

#include "testsuite.hpp"

// These tests check that posix releases the handles and pages that it puts into a process.

namespace {

// Thor's handles consist of a slot index (low 20 bits) and a per-slot generation that starts at 1.
// Free slots are reused before new slots are allocated. Hence, allocating until a slot of
// generation 1 shows up consumes all free slots and yields the number of slots that are in use.
size_t countUsedSlots() {
	std::vector<HelHandle> probes;
	size_t used;
	while(true) {
		HelHandle handle;
		HEL_CHECK(helCreateOneshotEvent(&handle));
		probes.push_back(handle);
		if((handle >> 20) == 1) {
			used = (handle & 0xFFFFF) - (probes.size() - 1);
			break;
		}
	}
	for(auto handle : probes)
		HEL_CHECK(helCloseDescriptor(kHelThisUniverse, handle));
	return used;
}

// Closed handles only become reusable after a RCU grace period and posix cleans up after
// threads asynchronously. Hence, retry until the count drops to the expected value.
size_t countHandles(size_t expected = 0, int attempts = 5) {
	size_t used = SIZE_MAX;
	for(int i = 0; i < attempts; i++) {
		usleep(50'000);
		used = std::min(used, countUsedSlots());
		if(used <= expected)
			break;
	}
	return used;
}

// Counts the shared anonymous areas of the given protection.
// "rw--" matches thread pages; "r---" matches file tables and the clock tracker page.
size_t countAreas(const char *protection) {
	FILE *file = fopen("/proc/self/maps", "r");
	assert(file);
	size_t n = 0;
	char line[512];
	while(fgets(line, sizeof(line), file)) {
		char *field = strchr(line, ' ');
		assert(field);
		if(!strncmp(field + 1, protection, strlen(protection))
				&& strstr(line, " 00000000 00:00 0\n"))
			n++;
	}
	fclose(file);
	return n;
}

size_t countAreasSettled(const char *protection, size_t expected) {
	size_t n = 0;
	for(int i = 0; i < 40; i++) {
		n = countAreas(protection);
		if(n <= expected)
			break;
		usleep(50'000);
	}
	return n;
}

posix::ManagarmProcessData getProcessData() {
	posix::ManagarmProcessData data;
	HEL_CHECK(helSyscall2(kHelCallSuper + posix::superGetProcessData,
			reinterpret_cast<HelWord>(&data), sizeof(posix::ManagarmProcessData)));
	return data;
}

// --------------------------------------------------------------------------------------
// Helpers for clone() without CLONE_THREAD.
// The new thread has no TLS. Hence, it must not call into libc or use a stack protector.
// --------------------------------------------------------------------------------------

#define NO_TLS __attribute__((no_stack_protector))

constexpr size_t cloneStackSize = 0x10000;
constexpr uint64_t cloneTimeoutNanos = 10'000'000'000;

int cloneProbeFd;
int cloneExpectedFd;
std::atomic<HelHandle *> cloneChildTable;
std::atomic<bool> cloneParentReady;

[[noreturn]] NO_TLS void exitClone(int code) {
	helSyscall1(kHelCallSuper + posix::superExit, code);
	__builtin_trap();
}

// helDescriptorInfo() does not report any information yet but it does resolve the handle.
NO_TLS bool cloneHandleExists(HelHandle handle) {
	HelDescriptorInfo info;
	return helDescriptorInfo(handle, &info) != kHelErrNoDescriptor;
}

NO_TLS uint64_t cloneClock() {
	uint64_t now;
	if(helGetClock(&now))
		exitClone(100);
	return now;
}

// Entry of a thread that shares the address space but has its own file table.
[[noreturn]] NO_TLS void vmCloneEntry() {
	posix::ManagarmProcessData data;
	if(helSyscall2(kHelCallSuper + posix::superGetProcessData,
			reinterpret_cast<HelWord>(&data), sizeof(posix::ManagarmProcessData)))
		exitClone(10);
	if(!data.fileTable[cloneProbeFd])
		exitClone(11);
	cloneChildTable.store(data.fileTable);

	// The parent closes the FD in its own table; this table must keep it.
	auto deadline = cloneClock() + cloneTimeoutNanos;
	while(!cloneParentReady.load()) {
		if(cloneClock() > deadline)
			exitClone(12);
	}
	if(!data.fileTable[cloneProbeFd])
		exitClone(13);

	// The universe is shared with the parent, so the handle has to be valid.
	if(!cloneHandleExists(data.fileTable[cloneProbeFd]))
		exitClone(14);

	exitClone(0);
}

// Entry of a thread that has its own (copied) address space but shares the file table.
[[noreturn]] NO_TLS void filesCloneEntry() {
	posix::ManagarmProcessData data;
	if(helSyscall2(kHelCallSuper + posix::superGetProcessData,
			reinterpret_cast<HelWord>(&data), sizeof(posix::ManagarmProcessData)))
		exitClone(10);
	if(!data.fileTable[cloneProbeFd])
		exitClone(11);
	if(!cloneHandleExists(data.fileTable[cloneProbeFd]))
		exitClone(12);

	// FDs that the parent opens have to show up in this address space, too.
	auto deadline = cloneClock() + cloneTimeoutNanos;
	while(!data.fileTable[cloneExpectedFd]) {
		if(cloneClock() > deadline)
			exitClone(13);
	}
	if(!cloneHandleExists(data.fileTable[cloneExpectedFd]))
		exitClone(14);

	exitClone(0);
}

// Entry of a thread that shares the address space and replaces its image right away.
[[noreturn]] NO_TLS void execCloneEntry() {
	static const char path[] = "/usr/bin/true";
	static const char args[] = "true";
	static const char env[] = "POSIX_TESTS=1";
	HelWord out;
	helSyscall6_1(kHelCallSuper + posix::superExecve,
			reinterpret_cast<HelWord>(path), sizeof(path) - 1,
			reinterpret_cast<HelWord>(args), sizeof(args),
			reinterpret_cast<HelWord>(env), sizeof(env), &out);
	exitClone(20);
}

void *allocateCloneStack() {
	void *stack = mmap(nullptr, cloneStackSize, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(stack != MAP_FAILED);
	auto top = reinterpret_cast<uintptr_t>(stack) + cloneStackSize;
#if defined(__x86_64__)
	// The entry expects the stack alignment that follows a call instruction.
	top -= 8;
#endif
	return reinterpret_cast<void *>(top);
}

void rawClone(uint64_t flags, void (*entry)(), void *sp) {
	posix::superCloneArgs args{.flags = flags};
	HelWord posixError = 0;
	HelWord tid = 0;
	HEL_CHECK(helSyscall3_2(kHelCallSuper + posix::superClone,
			reinterpret_cast<HelWord>(entry), reinterpret_cast<HelWord>(sp),
			reinterpret_cast<HelWord>(&args), &posixError, &tid));
	assert(!posixError);
}

void waitForClone() {
	int status;
	pid_t pid = waitpid(-1, &status, 0);
	assert_errno("waitpid", pid > 0);
	if(!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "clone child failed with status 0x%x\n", status);
		abort();
	}
}

[[noreturn]] void execSelf(const char *test) {
	char path[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	assert_errno("readlink", n > 0);
	path[n] = 0;
	execl(path, path, test, static_cast<char *>(nullptr));
	_exit(127);
}

} // anonymous namespace

DEFINE_TEST(thread_exit_releases_handles, ([] {
	auto runThread = [] {
		pthread_t thread;
		int e = pthread_create(&thread, nullptr, [] (void *) -> void * { return nullptr; }, nullptr);
		assert(!e);
		e = pthread_join(thread, nullptr);
		assert(!e);
	};

	// libc allocates some state lazily.
	runThread();

	auto handlesBefore = countHandles();
	auto threadPagesBefore = countAreasSettled("rw--", 1);
	for(int i = 0; i < 100; i++)
		runThread();
	auto handlesAfter = countHandles(handlesBefore, 40);
	auto threadPagesAfter = countAreasSettled("rw--", threadPagesBefore);
	if(handlesAfter > handlesBefore || threadPagesAfter > threadPagesBefore) {
		fprintf(stderr, "handles: %zu -> %zu, thread pages: %zu -> %zu\n",
				handlesBefore, handlesAfter, threadPagesBefore, threadPagesAfter);
		abort();
	}
}))

DEFINE_TEST(dup2_releases_handles, ([] {
	int fd = open("/dev/null", O_RDONLY);
	assert_errno("open", fd >= 0);
	int target = dup(fd);
	assert_errno("dup", target >= 0);

	auto handlesBefore = countHandles();
	for(int i = 0; i < 100; i++) {
		int res = dup2(fd, target);
		assert_errno("dup2", res == target);
	}
	auto handlesAfter = countHandles(handlesBefore, 40);
	if(handlesAfter > handlesBefore) {
		fprintf(stderr, "handles: %zu -> %zu\n", handlesBefore, handlesAfter);
		abort();
	}

	close(target);
	close(fd);
}))

// The test re-executes itself; the environment variable holds the remaining depth
// and the number of handles after the first execve().
DEFINE_TEST(exec_releases_handles, ([] {
	constexpr const char *variable = "POSIX_TESTS_EXEC_CHAIN";
	constexpr unsigned long depth = 16;

	const char *chain = getenv(variable);
	if(!chain) {
		pid_t pid = fork();
		assert_errno("fork", pid >= 0);
		if(!pid) {
			char value[64];
			snprintf(value, sizeof(value), "%lu:0", depth);
			setenv(variable, value, 1);
			execSelf("exec_releases_handles");
		}
		int status;
		pid_t res = waitpid(pid, &status, 0);
		assert_errno("waitpid", res == pid);
		assert(WIFEXITED(status) && !WEXITSTATUS(status));
		return;
	}

	unsigned long remaining, handlesBefore;
	int n = sscanf(chain, "%lu:%lu", &remaining, &handlesBefore);
	assert(n == 2);
	if(remaining == depth)
		handlesBefore = countHandles();
	if(remaining) {
		char value[64];
		snprintf(value, sizeof(value), "%lu:%lu", remaining - 1, handlesBefore);
		setenv(variable, value, 1);
		execSelf("exec_releases_handles");
	}

	auto handlesAfter = countHandles(handlesBefore, 40);
	if(handlesAfter > handlesBefore) {
		fprintf(stderr, "handles: %lu -> %zu\n", handlesBefore, handlesAfter);
		_exit(1);
	}
	_exit(0);
}))

DEFINE_TEST(clone_vm_without_files, ([] {
	void *sp = allocateCloneStack();
	int fd = open("/dev/null", O_RDONLY);
	assert_errno("open", fd >= 0);

	auto ownTable = getProcessData().fileTable;
	assert(ownTable[fd]);

	auto handlesBefore = countHandles();
	auto threadPagesBefore = countAreasSettled("rw--", 1);
	auto tablesBefore = countAreasSettled("r---", 2);

	cloneProbeFd = fd;
	cloneChildTable.store(nullptr);
	cloneParentReady.store(false);
	rawClone(CLONE_VM, &vmCloneEntry, sp);

	// The child must not use our file table.
	while(!cloneChildTable.load())
		usleep(1000);
	assert(cloneChildTable.load() != ownTable);

	close(fd);
	assert(!ownTable[fd]);
	cloneParentReady.store(true);
	waitForClone();

	// We closed one FD; all handles and pages of the child have to be gone.
	auto handlesAfter = countHandles(handlesBefore - 1, 40);
	auto threadPagesAfter = countAreasSettled("rw--", threadPagesBefore);
	auto tablesAfter = countAreasSettled("r---", tablesBefore);
	if(handlesAfter > handlesBefore - 1 || threadPagesAfter > threadPagesBefore
			|| tablesAfter > tablesBefore) {
		fprintf(stderr, "handles: %zu -> %zu, thread pages: %zu -> %zu, tables: %zu -> %zu\n",
				handlesBefore, handlesAfter, threadPagesBefore, threadPagesAfter,
				tablesBefore, tablesAfter);
		abort();
	}
}))

// execve() has to release what the thread had in the address space that it leaves.
DEFINE_TEST(clone_vm_exec_releases_handles, ([] {
	void *sp = allocateCloneStack();

	auto handlesBefore = countHandles();
	auto threadPagesBefore = countAreasSettled("rw--", 1);
	auto tablesBefore = countAreasSettled("r---", 2);

	rawClone(CLONE_VM, &execCloneEntry, sp);
	waitForClone();

	auto handlesAfter = countHandles(handlesBefore, 40);
	auto threadPagesAfter = countAreasSettled("rw--", threadPagesBefore);
	auto tablesAfter = countAreasSettled("r---", tablesBefore);
	if(handlesAfter > handlesBefore || threadPagesAfter > threadPagesBefore
			|| tablesAfter > tablesBefore) {
		fprintf(stderr, "handles: %zu -> %zu, thread pages: %zu -> %zu, tables: %zu -> %zu\n",
				handlesBefore, handlesAfter, threadPagesBefore, threadPagesAfter,
				tablesBefore, tablesAfter);
		abort();
	}
}))

DEFINE_TEST(clone_files_without_vm, ([] {
	void *sp = allocateCloneStack();
	int fd = open("/dev/null", O_RDONLY);
	assert_errno("open", fd >= 0);

	// Determine the FD that the next dup() returns.
	int expectedFd = dup(fd);
	assert_errno("dup", expectedFd >= 0);
	close(expectedFd);

	cloneProbeFd = fd;
	cloneExpectedFd = expectedFd;
	rawClone(CLONE_FILES, &filesCloneEntry, sp);

	int dupFd = dup(fd);
	assert(dupFd == expectedFd);
	waitForClone();

	// The file table still works after the view of the child is gone.
	close(dupFd);
	close(fd);
	fd = open("/dev/null", O_RDONLY);
	assert_errno("open", fd >= 0);
	assert(getProcessData().fileTable[fd]);
	close(fd);
}))
