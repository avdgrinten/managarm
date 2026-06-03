#pragma once

#include <map>
#include <memory>
#include <sys/signalfd.h>
#include <unordered_map>

#include <async/result.hpp>
#include <async/oneshot-event.hpp>
#include <async/recurring-event.hpp>
#include <frg/intrusive.hpp>
#include <core/cancel-events.hpp>
#include <frg/expected.hpp>
#include <frg/intrusive.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>
#include <sys/time.h>

#include "device.hpp"
#include "interval-timer.hpp"
#include "vfs.hpp"
#include "procfs.hpp"

struct Generation;
struct Process;
struct ThreadGroup;
struct ProcessGroup;
struct TerminalSession;
struct ControllingTerminalState;

typedef int ProcessId;

HelHandle rootHierarchy();

// TODO: This struct should store the process' VMAs once we implement them.
// TODO: We need a clarification here: Does mmap() keep file descriptions open (e.g. for flock())?
struct VmContext {
	static std::shared_ptr<VmContext> create(HelHandle hierarchyHandle);
	static async::result<std::shared_ptr<VmContext>> clone(
			std::shared_ptr<VmContext> original, HelHandle hierarchyHandle);

	~VmContext();

	helix::BorrowedDescriptor getSpace() {
		return _space;
	}

	// Handle to the hierarchy that owns this address space's memory.
	HelHandle hierarchyHandle() {
		return _hierarchyHandle;
	}

	// TODO: Pass abstract instead of hel flags to this function?
	async::result<frg::expected<Error, void *>> mapFile(uintptr_t hint, helix::UniqueDescriptor memory,
			smarter::shared_ptr<File, FileHandle> file,
			intptr_t offset, size_t size, bool copyOnWrite, uint32_t nativeFlags);

	async::result<void *> remapFile(void *old_pointer, size_t old_size, size_t new_size);

	async::result<void> protectFile(void *pointer, size_t size, uint32_t protectionFlags);

	void unmapFile(void *pointer, size_t size);

private:
	struct Area {
		bool copyOnWrite;
		size_t areaSize;
		uint32_t nativeFlags;
		helix::UniqueDescriptor fileView;
		helix::UniqueDescriptor copyView;
		smarter::shared_ptr<File, FileHandle> file;
		intptr_t offset;
		intptr_t effectiveOffset = 0;
	};

	std::pair<
		std::map<uintptr_t, Area>::iterator,
		std::map<uintptr_t, Area>::iterator
	> splitAreaOn_(uintptr_t addr, size_t size);

	helix::UniqueDescriptor _space;

	// Non-owning; the owning Process keeps the hierarchy descriptor alive.
	HelHandle _hierarchyHandle = kHelNullHandle;

	std::map<uintptr_t, Area> _areaTree;

public:
	struct AreaAccessor {
		AreaAccessor(std::map<uintptr_t, Area>::iterator iter)
		:_it{iter} {}

		uintptr_t baseAddress() {
			return _it->first;
		}

		size_t size() {
			return _it->second.areaSize;
		}

		bool isPrivate() {
			return _it->second.copyOnWrite;
		}

		bool isReadable() {
			return _it->second.nativeFlags & kHelMapProtRead;
		}

		bool isWritable() {
			return _it->second.nativeFlags & kHelMapProtWrite;
		}

		bool isExecutable() {
			return _it->second.nativeFlags & kHelMapProtExecute;
		}

		smarter::borrowed_ptr<File, FileHandle> backingFile() {
			return _it->second.file;
		}

		intptr_t backingFileOffset() {
			return _it->second.offset;
		}

	private:
		std::map<uintptr_t, Area>::iterator _it;
	};

	struct AreaIterator {
		AreaIterator(std::map<uintptr_t, Area>::iterator iter)
		:_it{iter} {}

		AreaIterator &operator++() {
			_it++;
			return *this;
		}

		AreaAccessor operator*() {
			return AreaAccessor{_it};
		}

		bool operator!=(const AreaIterator &other) {
			return _it != other._it;
		}
	private:
		std::map<uintptr_t, Area>::iterator _it;
	};

	AreaIterator begin() {
		return AreaIterator{_areaTree.begin()};
	}

	AreaIterator end() {
		return AreaIterator{_areaTree.end()};
	}
};

struct FsContext {
	static std::shared_ptr<FsContext> create();
	static std::shared_ptr<FsContext> clone(std::shared_ptr<FsContext> original);

	ViewPath getRoot();
	ViewPath getWorkingDirectory();
	mode_t getUmask();

	std::expected<void, Error> changeRoot(ViewPath root);
	std::expected<void, Error> changeWorkingDirectory(ViewPath path);
	mode_t setUmask(mode_t mask);

private:
	ViewPath _root;
	ViewPath _workDir;
	mode_t _umask = 0022;
};

struct FileDescriptor {
	smarter::shared_ptr<File, FileHandle> file;
	bool closeOnExec;
};

struct FileContext {
public:
	static std::shared_ptr<FileContext> create();
	static std::shared_ptr<FileContext> clone(std::shared_ptr<FileContext> original);

	~FileContext();

	helix::BorrowedDescriptor getUniverse() {
		return _universe;
	}

	helix::BorrowedDescriptor fileTableMemory() {
		return _fileTableMemory;
	}

	std::expected<int, Error> attachFile(smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false, int start_at = 0);

	std::expected<void, Error> attachFile(int fd, smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false);

	std::optional<FileDescriptor> getDescriptor(int fd);

	Error setDescriptor(int fd, bool close_on_exec);

	smarter::shared_ptr<File, FileHandle> getFile(int fd);

	Error closeFile(int fd);

	void closeOnExec();

	HelHandle clientMbusLane() {
		return _clientMbusLane;
	}

	const std::unordered_map<int, FileDescriptor> &fileTable() {
		return _fileTable;
	}

	void setFdLimit(uint64_t limit) {
		// TODO: increase the limit once we allow more than one shared fd -> HelHandle mapping page
		fdLimit_ = std::min(limit, 0x1000 / sizeof(HelHandle));
	}

private:
	HelHandle *fileTableWindow() {
		return reinterpret_cast<HelHandle *>(fileTableWindow_.get());
	}

	helix::UniqueDescriptor _universe;

	// TODO: replace this by a tree that remembers gaps between keys.
	std::unordered_map<int, FileDescriptor> _fileTable;

	helix::UniqueDescriptor _fileTableMemory;
	helix::Mapping fileTableWindow_;

	// TODO: increase the limit once we allow more than one shared fd -> HelHandle mapping page
	uint64_t fdLimit_ = 0x1000 / sizeof(HelHandle);

	HelHandle _clientMbusLane;
};

struct UserSignal {
	int pid = 0;
	int uid = 0;
	int code = SI_USER;
	int err_no = 0;
	sigval val = { .sival_ptr = nullptr };
};

struct TimerSignal {
	int timerId = 0;
};

struct ChildSignal {
	int code = 0;
	int pid = 0;
	int uid = 0;
	int status = 0;
	clock_t utime = 0;
	clock_t stime = 0;
};

struct SegfaultSignal {
	uintptr_t offendingAddress = 0;
	bool accessError = false;
	bool mapError = false;
};

using SignalInfo = std::variant<
	UserSignal,
	TimerSignal,
	ChildSignal,
	SegfaultSignal
>;

using SignalFlags = uint32_t;

inline constexpr SignalFlags signalInfo = (1 << 0);
inline constexpr SignalFlags signalOnce = (1 << 1);
inline constexpr SignalFlags signalReentrant = (1 << 2);
inline constexpr SignalFlags signalOnStack = (1 << 3);
inline constexpr SignalFlags signalNoChildWait = (1 << 4);

struct SignalFlagsView {
	SignalFlags flags;
};

template <>
struct std::formatter<SignalFlagsView> : std::formatter<string_view> {
	auto format(SignalFlagsView f, format_context &ctx) const {
		std::string joined;

		if (f.flags & signalInfo)
			joined += "signalInfo|";
		if (f.flags & signalOnce)
			joined += "signalOnce|";
		if (f.flags & signalReentrant)
			joined += "signalReentrant|";
		if (f.flags & signalOnStack)
			joined += "signalOnStack|";
		if (f.flags & signalNoChildWait)
			joined += "signalNoChildWait|";
		if (joined.empty())
			joined = "none|";

		joined.pop_back(); // Remove trailing '|'
		return std::formatter<string_view>::format(joined, ctx);
	}
};

enum class SignalDisposition {
	none,
	ignore,
	handle
};

template <>
struct std::formatter<SignalDisposition> : std::formatter<string_view> {
	auto format(SignalDisposition d, format_context &ctx) const {
		string_view name = "invalid";
		switch (d) {
			case SignalDisposition::none:
				name = "none";
				break;
			case SignalDisposition::ignore:
				name = "ignore";
				break;
			case SignalDisposition::handle:
				name = "handle";
				break;
		}
		return std::formatter<string_view>::format(name, ctx);
	}
};

struct SignalHandler {
	SignalDisposition disposition;
	SignalFlags flags;
	uint64_t mask;
	uintptr_t handlerIp;
	uintptr_t restorerIp;
};

template <>
struct std::formatter<SignalHandler> : std::formatter<string_view> {
	auto format(const SignalHandler &sh, format_context &ctx) const {
		return std::format_to(
		    ctx.out(),
		    "SignalHandler(disposition={}, flags={}, mask={:#x}, handlerIp={:#x}, "
		    "restorerIp={:#x})",
		    sh.disposition,
		    SignalFlagsView{sh.flags},
		    sh.mask,
		    sh.handlerIp,
		    sh.restorerIp
		);
	}
};

struct SignalItem {
	int signalNumber;
	SignalInfo info;
	frg::default_list_hook<SignalItem> hook_;
};

using PollSignalResult = std::tuple<uint64_t, uint64_t>;
using CheckSignalResult = std::tuple<uint64_t, uint64_t>;

struct CompileSignalInfo {
	void operator() (const UserSignal &info) const;
	void operator() (const TimerSignal &info) const;
	void operator() (const ChildSignal &info) const;
	void operator() (const SegfaultSignal &info) const;

	siginfo_t *si;
};

struct CompileSignalFdInfo {
	void operator() (const UserSignal &info) const;
	void operator() (const TimerSignal &info) const;
	void operator() (const ChildSignal &info) const;
	void operator() (const SegfaultSignal &info) const;

	signalfd_siginfo *si;
};

// Container for a single queue of pending signals; used at both the thread (`struct Process`)
// and process (`struct ThreadGroup`) level.
struct SignalQueue {
	friend struct Process;
	friend struct ThreadGroup;

private:
	struct SignalSlot {
		uint64_t raiseSeq = 0;

		frg::intrusive_list<
			SignalItem,
			frg::locate_member<
				SignalItem,
				frg::default_list_hook<SignalItem>,
				&SignalItem::hook_>
		> asyncQueue;
	};

	void issueSignal(int sn, SignalInfo info, uint64_t seq);

	SignalSlot slots_[64];
	async::recurring_event signalBell_;
	uint64_t activeSet_ = 0;
};

// Context encapsulating the configuration of signal handling for a thread (`struct Process`).
// Stores signal handler configuration and dispatches signals according to that.
struct SignalContext {
	static std::shared_ptr<SignalContext> create();
	static std::shared_ptr<SignalContext> clone(std::shared_ptr<SignalContext> original);

	void resetHandlers();

	SignalHandler getHandler(int sn);
	SignalHandler changeHandler(int sn, SignalHandler handler);

	// ------------------------------------------------------------------------
	// Signal context manipulation.
	// ------------------------------------------------------------------------

	struct SignalHandling {
		bool killed = false;
		bool ignored = false;
		SignalHandler handler;
	};

	// As this function bumps the signal seq number, only call this exactly
	// once per SignalItem!
	SignalHandling determineHandling(SignalItem *item, Process *process);
	async::result<void> raiseContext(SignalItem *item, Process *process,
			SignalHandling handling);

	async::result<void> determineAndRaiseContext(SignalItem *item, Process *process,
			bool &killed);

	async::result<void> restoreContext(helix::BorrowedDescriptor thread, Process *process);

private:
	SignalHandler _handlers[64];
};

template <>
struct std::formatter<SignalContext::SignalHandling> : std::formatter<string_view> {
	auto format(const SignalContext::SignalHandling &sh, format_context &ctx) const {
		return std::format_to(
		    ctx.out(),
		    "SignalHandling(killed={}, ignored={}, handler={})",
		    sh.killed,
		    sh.ignored,
		    sh.handler
		);
	}
};

enum class NotifyType {
	null,
	terminated
};

struct TerminationByExit {
	int code;
};

struct TerminationBySignal {
	int signo;
};

using TerminationState = std::variant<
	std::monostate,
	TerminationByExit,
	TerminationBySignal
>;

using WaitFlags = uint32_t;
inline constexpr WaitFlags waitNonBlocking = 1;
inline constexpr WaitFlags waitLeaveZombie = 2;
inline constexpr WaitFlags waitExited = 4;

struct ResourceUsage {
	uint64_t userTime;
};

// This struct is mainly needed to coordinate the destruction of kernel threads
// and request handelers during exec(). The exec() and terminate() implementations
// use it to wait until all running request handlers are finished.
struct Generation {
	~Generation();

	bool inTermination = false;
	async::cancellation_event cancelServe;
	async::oneshot_event signalsDone;
	async::oneshot_event requestsDone;
};

// --------------------------------------------------------------------------------------
// The 'Process' class.
// --------------------------------------------------------------------------------------

// This struct owns the PID.
// It remains alive until the PID can be recycled.
struct PidHull : std::enable_shared_from_this<PidHull> {
	PidHull(pid_t pid);

	PidHull(const PidHull &) = delete;

	~PidHull();

	PidHull &operator= (const PidHull &) = delete;

	pid_t getPid() {
		return pid_;
	}

	void initializeThreadGroup(ThreadGroup *tg);
	void initializeProcess(Process *process);
	void initializeProcessGroup(ProcessGroup *group);
	void initializeTerminalSession(TerminalSession *session);

	std::shared_ptr<ThreadGroup> getThreadGroup();
	std::shared_ptr<Process> getProcess();
	std::shared_ptr<ProcessGroup> getProcessGroup();
	std::shared_ptr<TerminalSession> getTerminalSession();

private:
	pid_t pid_;
	std::weak_ptr<ThreadGroup> threadGroup_;
	std::weak_ptr<Process> process_;
	std::weak_ptr<ProcessGroup> processGroup_;
	std::weak_ptr<TerminalSession> terminalSession_;
};

struct Process : std::enable_shared_from_this<Process> {
	friend struct ThreadGroup;
	friend struct ProcessGroup;
	friend struct TerminalSession;
	friend struct ControllingTerminalState;

	static std::shared_ptr<Process> findProcess(ProcessId pid);

	static async::result<std::shared_ptr<ThreadGroup>> init(std::string path);

	static async::result<std::shared_ptr<Process>> fork(std::shared_ptr<Process> parent);

	static async::result<std::expected<std::shared_ptr<Process>, Error>>
	clone(std::shared_ptr<Process> parent, void *ip, void *sp, posix::superCloneArgs *args);

	static async::result<Error> exec(std::shared_ptr<Process> process,
			std::string path, std::vector<std::string> args, std::vector<std::string> env);

public:
	Process(ThreadGroup *threadGroup, std::shared_ptr<PidHull> tidHull);

	~Process();

	ThreadGroup *getParent();
	PidHull *getPidHull();
	PidHull *getTidHull();
	int pid();
	int tid();

	std::string path() {
		return _path;
	}

	std::string name() {
		return _name;
	}

	void setName(std::string name) {
		_name = name;
	}

	helix::BorrowedLane posixLane() {
		return _posixLane;
	}

	helix::BorrowedDescriptor threadDescriptor() {
		return _threadDescriptor;
	}

	// As the contexts associated with a process can change (e.g. when unshare() is implemented),
	// those functions return refcounted pointers.
	std::shared_ptr<VmContext> vmContext() { return _vmContext; }
	std::shared_ptr<FsContext> fsContext() { return _fsContext; }
	std::shared_ptr<FileContext> fileContext() { return _fileContext; }
	ThreadGroup *threadGroup() { return tgPointer_.get(); }
	std::shared_ptr<ProcessGroup> pgPointer();

	void setSignalMask(uint64_t mask) {
		_signalMask = mask;
	}

	uint64_t signalMask() {
		return _signalMask;
	}

	void issueThreadSignal(int sn, SignalInfo info);
	CheckSignalResult checkSignal();

	async::result<SignalItem *>
	fetchSignal(uint64_t mask, bool nonBlock, async::cancellation_token ct = {});

	SignalItem * tryFetchSignal(uint64_t mask);

	async::result<PollSignalResult>
	pollSignal(uint64_t in_seq, uint64_t mask, async::cancellation_token cancellation = {});

	HelHandle clientPosixLane() { return _clientPosixLane; }
	posix::ThreadPage *clientThreadPage() { return _clientThreadPage; }
	void *clientFileTable() { return _clientFileTable; }
	void *clientClkTrackerPage() { return _clientClkTrackerPage; }
	HelHandle clientHierarchyHandle() { return _clientHierarchyHandle; }
	void *clientAuxBegin() { return _clientAuxBegin; }
	void *clientAuxEnd() { return _clientAuxEnd; }

	posix::ThreadPage *accessThreadPage() {
		return reinterpret_cast<posix::ThreadPage *>(_threadPageMapping.get());
	}

	async::result<void> cancelEvent(uint64_t cancelId, int fd);

	// Like checkOrRequestSignalRaise() but only check if raising is possible.
	bool checkSignalRaise();

	// Check if signals can currently be raised (via the thread page).
	// If not, request the thread to raise its signals.
	// Preconditon: the thread has to be stopped!
	bool checkOrRequestSignalRaise();

	void dumpRegisters();

	// Called when a process is terminated.
	// This kills the kernel thread that currently corresponds the process
	// and waits for signal and request handling to exit.
	// This MUST only be called from the Process's observation loop.
	// Note that terminateGroup() must be called separately as needed.
	async::result<void> terminate(bool *lastInGroup = nullptr);

	struct WaitResult {
		int pid = 0;
		int uid = 0;
		TerminationState state = std::monostate{};
		ResourceUsage stats = {};
	};

	async::result<frg::expected<Error, WaitResult>>
	wait(int pid, WaitFlags flags, async::cancellation_token ct);

	bool hasChild(int pid);

	bool isOnAltStack(uint64_t sp) {
		return sp >= _altStackSp && sp <= (_altStackSp + _altStackSize);
	}

	uint64_t altStackSp() {
		return _altStackSp;
	}

	size_t altStackSize() {
		return _altStackSize;
	}

	void setAltStackSp(uint64_t ptr, size_t size) {
		_altStackSp = ptr;
		_altStackSize = size;
	}

	bool isAltStackEnabled() {
		return _altStackEnabled;
	}

	void setAltStackEnabled(bool en) {
		_altStackEnabled = en;
	}

	uint64_t enteredSignalSeq() {
		return _enteredSignalSeq;
	}

	void enterSignal() {
		_enteredSignalSeq++;
	}

	async::result<void> coredump(TerminationState state);

	CancelEventRegistry &cancelEventRegistry() {
		return cancelEventRegistry_;
	}

	helix_ng::CredentialsView credentials() const {
		return {credentials_};
	}

	// Forces terminate() to be called on next kHelObserveInterrupt.
	bool forceTermination = false;

	SignalQueue signalQueue;
	SignalItem *delayedSignal = nullptr;
	std::optional<SignalContext::SignalHandling> delayedSignalHandling = std::nullopt;

private:
	std::shared_ptr<PidHull> hull_;
	std::string _path;
	std::string _name;
	helix::UniqueLane _posixLane;
	helix::UniqueDescriptor _threadDescriptor;
	std::shared_ptr<Generation> _currentGeneration;
	std::shared_ptr<VmContext> _vmContext;
	std::shared_ptr<FsContext> _fsContext;
	std::shared_ptr<FileContext> _fileContext;
	helix::UniqueDescriptor _hierarchy;

	std::shared_ptr<procfs::Link> procfsTaskLink_;

	std::shared_ptr<ThreadGroup> tgPointer_;
	frg::default_list_hook<Process> tgHook_;

	helix::UniqueDescriptor _threadPageMemory;
	helix::Mapping _threadPageMapping;

	HelHandle _clientPosixLane = kHelNullHandle;
	HelHandle _clientHierarchyHandle = kHelNullHandle;
	posix::ThreadPage *_clientThreadPage;
	void *_clientFileTable = nullptr;
	void *_clientClkTrackerPage;
	// Pointers to the aux vector in the client.
	void *_clientAuxBegin = nullptr;
	void *_clientAuxEnd = nullptr;

	uint64_t _signalMask;

	bool _altStackEnabled = false;
	uint64_t _altStackSp = 0;
	size_t _altStackSize = 0;

	// Used for tracking signals that happened between sigprocmask and
	// a call that resumes on a signal.
	uint64_t _enteredSignalSeq = 0;

	CancelEventRegistry cancelEventRegistry_;
	std::array<char, 16> credentials_{};
};

std::optional<std::shared_ptr<Process>> findProcessWithCredentials(helix_ng::CredentialsView);

struct ThreadGroup : std::enable_shared_from_this<ThreadGroup> {
	friend struct Process;
	friend struct ProcessGroup;

	ThreadGroup(std::shared_ptr<PidHull> hull, ThreadGroup *parent);
	~ThreadGroup();

	static std::shared_ptr<ThreadGroup> init(std::shared_ptr<PidHull> hull);
	static ThreadGroup *create(std::shared_ptr<PidHull> hull, ThreadGroup *parent);

	PidHull *getHull() const {
		return hull_.get();
	}

	pid_t pid() const {
		return hull_->getPid();
	}

	bool didExecute() {
		return didExecute_;
	}

	ThreadGroup *getParent() {
		return parent_;
	}

	std::shared_ptr<ProcessGroup> pgPointer() { return pgPointer_; }

	void associateProcess(std::shared_ptr<Process> process);

	async::result<void> terminateGroup(TerminationState state);
	static void retire(ThreadGroup *group);

	SignalContext *signalContext() { return _signalContext.get(); }
	SignalQueue &signalQueue() { return signalQueue_; }

	static std::shared_ptr<ThreadGroup> findThreadGroup(ProcessId pid);
	std::shared_ptr<Process> findThread(pid_t tid);

	Error setUid(int uid) {
		if(uid < 0) {
			return Error::illegalArguments;
		}
		if(_uid == 0 || _euid == 0) {
			_uid = uid;
			_euid = uid;
			return Error::success;
		} else if(uid == _uid) {
			_uid = uid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int uid() {
		return _uid;
	}

	Error setEuid(int euid) {
		if(euid < 0) {
			return Error::illegalArguments;
		}
		if(_uid == 0 || _euid == 0 || euid == _uid) {
			_euid = euid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int euid() {
		return _euid;
	}

	Error setGid(int gid) {
		if(gid < 0) {
			return Error::illegalArguments;
		}
		if(_gid == 0 || _egid == 0) {
			_gid = gid;
			_egid = gid;
			return Error::success;
		} else if(gid == _gid) {
			_egid = gid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int gid() {
		return _gid;
	}

	Error setEgid(int egid) {
		if(egid < 0) {
			return Error::illegalArguments;
		}
		if(_gid == 0 || _egid == 0 || _gid == egid || _egid == egid) {
			_egid = egid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int egid() {
		return _egid;
	}

	const std::vector<gid_t> &supplementaryGroups() const {
		return supplementaryGids_;
	}

	Error setSupplementaryGroups(std::vector<gid_t> list) {
		supplementaryGids_ = std::move(list);
		return Error::success;
	}

	ResourceUsage selfUsage() const {
		return _generationUsage;
	}

	ResourceUsage accumulatedUsage() const {
		return _childrenUsage;
	}

	void setProcfsLink(std::shared_ptr<procfs::Link> link) {
		procfsLink_ = link;
	}

	std::shared_ptr<procfs::Link> procfsLink() const {
		return procfsLink_;
	}

	void setDumpable(bool dumpable) {
		dumpable_ = dumpable;
	}

	bool getDumpable() const {
		return dumpable_;
	}

	NotifyType notifyType() const {
		return notifyType_;
	}

	TerminationState terminationState() const {
		return _state;
	}

	void issueThreadGroupSignal(int sn, SignalInfo info);

	void setParentDeathSignal(std::optional<int> sig) {
		parentDeathSignal_ = sig;
	}

	async::result<bool> awaitNotifyTypeChange(async::cancellation_token token = {});

	struct IntervalTimer : posix::IntervalTimer {
		IntervalTimer(std::weak_ptr<Process> process, uint64_t initial, uint64_t interval)
			: posix::IntervalTimer(initial, interval), process_{process} {}

		void raise(bool success) override {
			if(!success)
				return;

			auto proc_lock = process_.lock();
			if(proc_lock)
				proc_lock->threadGroup()->issueThreadGroupSignal(SIGALRM, {});
		}

		void expired() override {
		}

	private:
		std::weak_ptr<Process> process_;
	};

	struct PosixTimer;

	struct PosixTimerContext {
		clockid_t clockid;
		std::shared_ptr<PosixTimer> timer = {};
		std::optional<int> tid = std::nullopt;
		int signo;
	};

	struct PosixTimer : posix::IntervalTimer {
		PosixTimer(std::weak_ptr<Process> proc, std::optional<int> tid,
			int signo, int timerId, uint64_t initial, uint64_t interval)
			: posix::IntervalTimer(initial, interval), process_{proc},
			tid{tid}, signo{signo}, timerId{timerId} {}

		void raise(bool success) override {
			if(!success)
				return;

			if(tid) {
				auto proc = process_.lock();
				if(!proc) {
					cancel();
					expired();
					return;
				}
				proc->threadGroup()->issueThreadGroupSignal(signo, TimerSignal{static_cast<int>(timerId)});
			}
		}

		void expired() override {
			isExpired = true;
		}

	private:
		std::weak_ptr<Process> process_;
		std::optional<int> tid = std::nullopt;
		int signo;
		int timerId;
		bool isExpired = false;
	};

	std::shared_ptr<IntervalTimer> realTimer;
	std::unordered_map<int, std::shared_ptr<PosixTimerContext>> timers;
	id_allocator<int> timerIdAllocator{};

	void raiseNotifyBell() {
		_notifyBell.raise();
	}

private:
	ThreadGroup *parent_;
	// The leading thread is kept alive until the thread group is terminated, as we need to expose
	// it via /proc/[pid]/task/[tid].
	std::shared_ptr<Process> leader_;

	std::shared_ptr<PidHull> hull_;

	std::shared_ptr<SignalContext> _signalContext;

	bool didExecute_ = false;

	std::shared_ptr<ProcessGroup> pgPointer_;
	frg::default_list_hook<ThreadGroup> pgHook_;

	int _uid;
	int _euid;
	int _gid;
	int _egid;

	std::vector<gid_t> supplementaryGids_;

	// Raised by Process::terminate().
	async::recurring_event processTerminationEvent_;

	// Resource usage accumulated from previous generations.
	ResourceUsage _generationUsage = {};
	// Resource usage accumulated from children.
	ResourceUsage _childrenUsage = {};

	// The following intrusive queue stores notifications for wait().
	NotifyType notifyType_ = NotifyType::null;
	async::recurring_event notifyTypeChange_;
	TerminationState _state;

	std::vector<std::shared_ptr<ThreadGroup>> _children;

	frg::default_list_hook<ThreadGroup> notifyHook_;
	frg::intrusive_list<
		ThreadGroup,
		frg::locate_member<ThreadGroup, frg::default_list_hook<ThreadGroup>, &ThreadGroup::notifyHook_>
	> _notifyQueue;
	async::recurring_event _notifyBell;

	std::shared_ptr<procfs::Link> procfsLink_;

	uint64_t currentSignalSeq_ = 1;
	// ThreadGroup-wide signal queue for signals with no thread to accept them.
	SignalQueue signalQueue_;
	std::optional<int> parentDeathSignal_ = std::nullopt;

	// equivalent to PR_[SG]ET_DUMPABLE
	bool dumpable_ = true;

	std::vector<std::shared_ptr<Process>> threads_;
};

// --------------------------------------------------------------------------------------
// Process groups and sessions.
// --------------------------------------------------------------------------------------

struct ProcessGroup : std::enable_shared_from_this<ProcessGroup> {
	friend struct TerminalSession;
	friend struct ControllingTerminalState;

	static std::shared_ptr<ProcessGroup> findProcessGroup(ProcessId pgid);

	ProcessGroup(std::shared_ptr<PidHull> hull);

	~ProcessGroup();

	void reassociateProcess(ThreadGroup *process);

	void dropProcess(ThreadGroup *process);

	void issueSignalToGroup(int sn, SignalInfo info);

	bool isOrphaned();

	PidHull *getHull() {
		return hull_.get();
	}

	TerminalSession *getSession() { return sessionPointer_.get(); }

private:
	std::shared_ptr<PidHull> hull_;

	frg::intrusive_list<
		ThreadGroup,
		frg::locate_member<
			ThreadGroup,
			frg::default_list_hook<ThreadGroup>,
			&ThreadGroup::pgHook_
		>
	> members_;

	std::shared_ptr<TerminalSession> sessionPointer_;
	frg::default_list_hook<ProcessGroup> sessionHook_;
};

struct TerminalSession : std::enable_shared_from_this<TerminalSession> {
	friend struct ControllingTerminalState;

	TerminalSession(std::shared_ptr<PidHull> hull);

	~TerminalSession();

	pid_t getSessionId();

	static std::shared_ptr<TerminalSession> initializeNewSession(ThreadGroup *sessionLeader);

	std::shared_ptr<ProcessGroup> spawnProcessGroup(ThreadGroup *groupLeader);

	std::shared_ptr<ProcessGroup> getProcessGroupById(pid_t id);

	ProcessGroup *getForegroundGroup() { return foregroundGroup_; }

	ControllingTerminalState *getControllingTerminal() { return ctsPointer_; }

	void dropGroup(ProcessGroup *group);

	Error setForegroundGroup(ProcessGroup *group);

private:
	std::shared_ptr<PidHull> hull_;

	frg::intrusive_list<
		ProcessGroup,
		frg::locate_member<
			ProcessGroup,
			frg::default_list_hook<ProcessGroup>,
			&ProcessGroup::sessionHook_
		>
	> groups_;

	ProcessGroup *foregroundGroup_ = nullptr;

	ControllingTerminalState *ctsPointer_ = nullptr;
};

struct ControllingTerminalState {
	Error assignSessionOf(Process *process);

	void dropSession(TerminalSession *session);

	void issueSignalToForegroundGroup(int sn, SignalInfo info);

	TerminalSession *getSession() { return associatedSession_; }

	std::weak_ptr<UnixDevice> controllingTerminal_;
private:
	TerminalSession *associatedSession_ = nullptr;
};
