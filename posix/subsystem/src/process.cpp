
#include <signal.h>
#include <string.h>
#include <print>

#include "common.hpp"
#include <core/clock.hpp>
#include "exec.hpp"
#include "gdbserver.hpp"
#include "process.hpp"

#include <protocols/posix/data.hpp>
#include <helix/ipc.hpp>

#include "debug-options.hpp"

static bool logFileAttach = false;
static bool logCleanup_ = false;

async::result<void> serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation);

std::shared_ptr<ThreadGroup> initThreadGroup = nullptr;

// ----------------------------------------------------------------------------
// VmContext.
// ----------------------------------------------------------------------------

std::shared_ptr<VmContext> VmContext::create() {
	auto context = std::make_shared<VmContext>();

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	return context;
}

async::result<std::shared_ptr<VmContext>> VmContext::clone(std::shared_ptr<VmContext> original) {
	auto context = std::make_shared<VmContext>();

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	for(const auto &entry : original->_areaTree) {
		const auto &[address, area] = entry;

		helix::UniqueDescriptor copyView;
		if(area.copyOnWrite) {
			auto forkResult = co_await helix_ng::forkMemory(area.copyView);
			HEL_CHECK(forkResult.error());
			copyView = forkResult.descriptor();

			void *pointer;
			HelError error = helMapMemory(copyView.getHandle(), context->_space.getHandle(),
					reinterpret_cast<void *>(address),
					area.effectiveOffset, area.areaSize, area.nativeFlags, &pointer);
			if(error != kHelErrNone && error != kHelErrAlreadyExists) {
				HEL_CHECK(error);
			}
		}else{
			void *pointer;
			HEL_CHECK(helMapMemory(area.fileView.getHandle(), context->_space.getHandle(),
					reinterpret_cast<void *>(address),
					area.offset, area.areaSize, area.nativeFlags, &pointer));
		}

		Area copy;
		copy.copyOnWrite = area.copyOnWrite;
		copy.areaSize = area.areaSize;
		copy.nativeFlags = area.nativeFlags;
		copy.fileView = area.fileView.dup();
		copy.copyView = std::move(copyView);
		copy.file = area.file;
		copy.offset = area.offset;
		copy.effectiveOffset = area.effectiveOffset;
		context->_areaTree.emplace(address, std::move(copy));
	}

	co_return context;
}

VmContext::~VmContext() {
	if(logCleanup_)
		std::cout << "\e[33mposix: VmContext is destructed\e[39m" << std::endl;
}

auto VmContext::splitAreaOn_(uintptr_t addr, size_t size) ->
		std::pair<
			std::map<uintptr_t, Area>::iterator,
			std::map<uintptr_t, Area>::iterator
		> {
	// Avoid accessing out of bounds iterators
	if (!_areaTree.size())
		return {_areaTree.end(), _areaTree.end()};

	auto performSingleSplit = [this] (uintptr_t addr) {
		auto it = _areaTree.upper_bound(addr);
		if (it != _areaTree.begin())
			it = std::prev(it);

		auto &[base, area] = *it;

		if (base < addr && (base + area.areaSize) > addr) {
			Area right;
			right.copyOnWrite = area.copyOnWrite;
			right.areaSize = area.areaSize - (addr - base);
			right.nativeFlags = area.nativeFlags;
			right.fileView = area.fileView.dup();
			right.copyView = area.copyView.dup();
			right.file = area.file;
			right.offset = area.offset + (addr - base);
			right.effectiveOffset = area.effectiveOffset + (addr - base);

			_areaTree.emplace(addr, std::move(right));

			area.areaSize = (addr - base);
		}

		return it;
	};

	return {
		performSingleSplit(addr),
		std::next(performSingleSplit(addr + size))
	};
}

async::result<frg::expected<Error, void *>>
VmContext::mapFile(uintptr_t hint, helix::UniqueDescriptor memory,
		smarter::shared_ptr<File, FileHandle> file,
		intptr_t offset, size_t size, bool copyOnWrite, uint32_t nativeFlags) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	helix::UniqueDescriptor copyView;
	void *pointer;
	HelError error;
	if(copyOnWrite) {
		HelHandle handle;
		if(memory) {
			HEL_CHECK(helCopyOnWrite(memory.getHandle(), offset, alignedSize, &handle));
		}else{
			HEL_CHECK(helCopyOnWrite(kHelZeroMemory, offset, alignedSize, &handle));
		}
		copyView = helix::UniqueDescriptor{handle};

		error = helMapMemory(copyView.getHandle(), _space.getHandle(),
				reinterpret_cast<void *>(hint),
				0, alignedSize, nativeFlags, &pointer);
	}else{
		error = helMapMemory(memory.getHandle(), _space.getHandle(),
				reinterpret_cast<void *>(hint),
				offset, alignedSize, nativeFlags, &pointer);
	}

	if(error == kHelErrAlreadyExists) {
		co_return Error::alreadyExists;
	}else if(error == kHelErrNoMemory)
		co_return Error::noMemory;
	HEL_CHECK(error);

	//std::cout << "posix: VM_MAP returns " << pointer
	//		<< " (size: " << (void *)size << ")" << std::endl;

	auto address = reinterpret_cast<uintptr_t>(pointer);

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt;) {
		const auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize))
			it = _areaTree.erase(it);
		else
			++it;
	}

	// Construct the new area.
	Area area;
	area.copyOnWrite = copyOnWrite;
	area.areaSize = alignedSize;
	area.nativeFlags = nativeFlags;
	area.fileView = std::move(memory);
	area.copyView = std::move(copyView);
	area.file = std::move(file);
	area.offset = offset;
	area.effectiveOffset = copyOnWrite ? 0 : offset;
	_areaTree.emplace(address, std::move(area));

	co_return pointer;
}

async::result<void *> VmContext::remapFile(void *oldPointer,
		size_t oldSize, size_t newSize) {
	size_t alignedOldSize = (oldSize + 0xFFF) & ~size_t(0xFFF);
	size_t alignedNewSize = (newSize + 0xFFF) & ~size_t(0xFFF);

//	std::cout << "posix: Remapping " << oldPointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(oldPointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == alignedOldSize);

	assert(!it->second.copyOnWrite);

	auto memory = co_await it->second.file->accessMemory();

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, it->second.offset, alignedNewSize,
			it->second.nativeFlags, &pointer));
//	std::cout << "posix: VM_REMAP returns " << pointer << std::endl;

	// Unmap the old area.
	HEL_CHECK(helUnmapMemory(_space.getHandle(), oldPointer, alignedOldSize));

	// Construct the new area from the old one.
	Area area;
	area.copyOnWrite = it->second.copyOnWrite;
	area.areaSize = alignedNewSize;
	area.nativeFlags = it->second.nativeFlags;
	area.fileView = std::move(it->second.fileView);
	area.copyView = std::move(it->second.copyView);
	area.file = std::move(it->second.file);
	area.offset = it->second.offset;
	// TODO: This needs to be revised when we implement remapFile() for anonymous mappings.
	area.effectiveOffset = it->second.offset;
	_areaTree.erase(it);

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + alignedNewSize);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	_areaTree.insert({address, std::move(area)});

	co_return pointer;
}

async::result<void> VmContext::protectFile(void *pointer, size_t size, uint32_t protectionFlags) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);
	auto address = reinterpret_cast<uintptr_t>(pointer);

	helix::ProtectMemory protect;
	auto &&submit = helix::submitProtectMemory(_space, &protect,
			pointer, alignedSize, protectionFlags, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(protect.error());

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt; ++it) {
		auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize)) {
			area.nativeFlags &= ~(kHelMapProtRead | kHelMapProtWrite | kHelMapProtExecute);
			area.nativeFlags |= protectionFlags;
		}
	}
}

void VmContext::unmapFile(void *pointer, size_t size) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);
	auto address = reinterpret_cast<uintptr_t>(pointer);

	HEL_CHECK(helUnmapMemory(_space.getHandle(), pointer, alignedSize));

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt;) {
		auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize)) {
			it = _areaTree.erase(it);
		} else {
			++it;
		}
	}
}

// ----------------------------------------------------------------------------
// FsContext.
// ----------------------------------------------------------------------------

std::shared_ptr<FsContext> FsContext::create() {
	auto context = std::make_shared<FsContext>();

	context->_root = rootPath();
	context->_workDir = rootPath();

	return context;
}

std::shared_ptr<FsContext> FsContext::clone(std::shared_ptr<FsContext> original) {
	auto context = std::make_shared<FsContext>();

	context->_root = original->_root;
	context->_workDir = original->_workDir;
	context->_umask = original->_umask;

	return context;
}

ViewPath FsContext::getRoot() {
	return _root;
}

ViewPath FsContext::getWorkingDirectory() {
	return _workDir;
}

std::expected<void, Error> FsContext::changeRoot(ViewPath root) {
	if(root.second->getTarget()->getType() != VfsType::directory)
		return std::unexpected(Error::notDirectory);
	_root = std::move(root);
	return {};
}

std::expected<void, Error> FsContext::changeWorkingDirectory(ViewPath workdir) {
	if(workdir.second->getTarget()->getType() != VfsType::directory)
		return std::unexpected(Error::notDirectory);
	_workDir = std::move(workdir);
	return {};
}

mode_t FsContext::getUmask() {
	return _umask;
}

mode_t FsContext::setUmask(mode_t mask) {
	mode_t old = _umask;
	_umask = mask & 0777;
	return old;
}

// ----------------------------------------------------------------------------
// FileContext.
// ----------------------------------------------------------------------------

static HelHandle posixMbusClient = [] {
	posix::ManagarmProcessData data;

	HEL_CHECK(helSyscall1(kHelCallSuper + 1, reinterpret_cast<HelWord>(&data)));

	return data.mbusLane;
}();

std::shared_ptr<FileContext> FileContext::create() {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	HEL_CHECK(helAllocateMemory(fileTableMemorySize, 0, nullptr, &memory));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->fileTableWindow_ = helix::Mapping{context->_fileTableMemory, 0, fileTableMemorySize};

	HEL_CHECK(helTransferDescriptor(
	    posixMbusClient, context->_universe.getHandle(), kHelTransferDescriptorOut, &context->_clientMbusLane
	));

	return context;
}

std::shared_ptr<FileContext> FileContext::clone(std::shared_ptr<FileContext> original) {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	HEL_CHECK(helAllocateMemory(fileTableMemorySize, 0, nullptr, &memory));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->fileTableWindow_ = helix::Mapping{context->_fileTableMemory, 0, fileTableMemorySize};

	for(auto entry : original->_fileTable) {
		//std::cout << "Clone FD " << entry.first << std::endl;

		// the new number of fd in the file context is always <= the number of fd in the original
		auto attachRes = context->attachFile(entry.first, entry.second.file, entry.second.closeOnExec);
		assert(attachRes);
	}

	HEL_CHECK(helTransferDescriptor(
	    posixMbusClient, context->_universe.getHandle(), kHelTransferDescriptorOut, &context->_clientMbusLane
	));

	return context;
}

FileContext::~FileContext() {
	if(logCleanup_)
		std::cout << "\e[33mposix: FileContext is destructed\e[39m" << std::endl;
}

void FileContext::writeFileTableEntry(int fd, HelHandle handle) {
	auto slot = &fileTableWindow()[fd];
	// Bump the sequence to an odd value, update the entry, then bump it back to even.
	auto seq = slot->sequence;
	__atomic_store_n(&slot->sequence, seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	slot->handle = handle;
	slot->apiSignature = handle ? posix::ptApiDefault : 0;
	slot->reserved = 0;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&slot->sequence, seq + 2, __ATOMIC_RELAXED);
}

std::expected<int, Error> FileContext::attachFile(smarter::shared_ptr<File, FileHandle> file,
		bool closeOnExec, int startAt) {
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(
	    file->getPassthroughLane().getHandle(), _universe.getHandle(), kHelTransferDescriptorOut, &handle
	));

	for(int fd = startAt; ; fd++) {
		if(_fileTable.find(fd) != _fileTable.end())
			continue;

		if (static_cast<uint64_t>(fd) >= fdLimit_)
			return std::unexpected{Error::noFileDescriptorsAvailable};

		if(logFileAttach)
			std::cout << "posix: Attaching FD " << fd << std::endl;

		_fileTable.insert({fd, {std::move(file), closeOnExec}});
		writeFileTableEntry(fd, handle);
		return fd;
	}
}

std::expected<void, Error> FileContext::attachFile(int fd, smarter::shared_ptr<File, FileHandle> file,
		bool close_on_exec) {
	if (static_cast<uint64_t>(fd) >= fdLimit_)
		return std::unexpected{Error::noFileDescriptorsAvailable};

	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(
	    file->getPassthroughLane().getHandle(), _universe.getHandle(), kHelTransferDescriptorOut, &handle
	));

	if(logFileAttach)
		std::cout << "posix: Attaching fixed FD " << fd << std::endl;

	auto it = _fileTable.find(fd);
	if(it != _fileTable.end()) {
		it->second = {std::move(file), close_on_exec};
	}else{
		_fileTable.insert({fd, {std::move(file), close_on_exec}});
	}
	writeFileTableEntry(fd, handle);

	return {};
}

std::optional<FileDescriptor> FileContext::getDescriptor(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return std::nullopt;
	return file->second;
}

Error FileContext::setDescriptor(int fd, bool close_on_exec) {
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}
	it->second.closeOnExec = close_on_exec;
	return Error::success;
}

smarter::shared_ptr<File, FileHandle> FileContext::getFile(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return smarter::shared_ptr<File, FileHandle>{};
	return file->second.file;
}

Error FileContext::closeFile(int fd) {
	if(logFileAttach)
		std::cout << "posix: Closing FD " << fd << std::endl;
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}

	HEL_CHECK(helCloseDescriptor(_universe.getHandle(), fileTableWindow()[fd].handle));

	writeFileTableEntry(fd, 0);
	_fileTable.erase(it);
	return Error::success;
}

void FileContext::closeOnExec() {
	auto it = _fileTable.begin();
	while(it != _fileTable.end()) {
		if(it->second.closeOnExec) {
			HEL_CHECK(helCloseDescriptor(_universe.getHandle(), fileTableWindow()[it->first].handle));

			writeFileTableEntry(it->first, 0);
			it = _fileTable.erase(it);
		}else{
			it++;
		}
	}
}

// ----------------------------------------------------------------------------
// SignalContext.
// ----------------------------------------------------------------------------

void CompileSignalInfo::operator() (const UserSignal &info) const {
	si->si_code = info.code;
	si->si_errno = info.err_no;
	si->si_pid = info.pid;
	si->si_uid = info.uid;
	si->si_value = info.val;
}

void CompileSignalFdInfo::operator() (const UserSignal &info) const {
	si->ssi_code = info.code;
	si->ssi_errno = info.err_no;
	si->ssi_pid = info.pid;
	si->ssi_uid = info.uid;
	si->ssi_ptr = reinterpret_cast<uintptr_t>(info.val.sival_ptr);
	si->ssi_int = info.val.sival_int;
}

void CompileSignalInfo::operator() (const TimerSignal &info) const {
	si->si_code = SI_TIMER;
	si->si_timerid = info.timerId;
}

void CompileSignalFdInfo::operator() (const TimerSignal &info) const {
	si->ssi_code = SI_TIMER;
	si->ssi_tid = info.timerId;
}

void CompileSignalInfo::operator() (const ChildSignal &info) const {
	si->si_code = info.code;
	si->si_pid = info.pid;
	si->si_uid = info.uid;
	si->si_status = info.status;
	si->si_utime = info.utime;
	si->si_stime = info.stime;
}

void CompileSignalFdInfo::operator() (const ChildSignal &info) const {
	si->ssi_code = info.code;
	si->ssi_pid = info.pid;
	si->ssi_uid = info.uid;
	si->ssi_status = info.status;
	si->ssi_utime = info.utime;
	si->ssi_stime = info.stime;
}

void CompileSignalInfo::operator() (const SegfaultSignal &info) const {
	si->si_addr = reinterpret_cast<void *>(info.offendingAddress);
	if (info.accessError)
		si->si_code = SEGV_ACCERR;
	else if (info.mapError)
		si->si_code = SEGV_MAPERR;
	else {
		std::println("posix: no SegfaultSignal error code given!");
		si->si_code = 0;
	}
}

void CompileSignalFdInfo::operator() (const SegfaultSignal &info) const {
	si->ssi_addr = info.offendingAddress;
	if (info.accessError)
		si->ssi_code = SEGV_ACCERR;
	else if (info.mapError)
		si->ssi_code = SEGV_MAPERR;
	else {
		std::println("posix: no SegfaultSignal error code given!");
		si->ssi_code = 0;
	}
}

std::shared_ptr<SignalContext> SignalContext::create() {
	auto context = std::make_shared<SignalContext>();

	// All signals use their default disposition.
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1].disposition = SignalDisposition::none;

	return context;
}

std::shared_ptr<SignalContext> SignalContext::clone(std::shared_ptr<SignalContext> original) {
	auto context = std::make_shared<SignalContext>();

	// Copy the current signal handler table.
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1] = original->_handlers[sn - 1];

	return context;
}

void SignalContext::resetHandlers() {
	for(int sn = 1; sn <= 64; sn++)
		if(_handlers[sn - 1].disposition == SignalDisposition::handle)
			_handlers[sn - 1] = SignalHandler{SignalDisposition::none};
}

SignalHandler SignalContext::getHandler(int sn) {
	assert(sn > 0);
	assert(sn - 1 < 64);
	return _handlers[sn - 1];
}

SignalHandler SignalContext::changeHandler(int sn, SignalHandler handler) {
	assert(sn > 0);
	assert(sn - 1 < 64);
	return std::exchange(_handlers[sn - 1], handler);
}

void SignalQueue::issueSignal(int sn, SignalInfo info, uint64_t seq) {
	assert(sn > 0);
	assert(sn - 1 < 64);
	auto item = new SignalItem;
	item->signalNumber = sn;
	item->info = info;

	slots_[sn - 1].raiseSeq = seq;
	slots_[sn - 1].asyncQueue.push_back(item);
	activeSet_ |= (UINT64_C(1) << (sn - 1));
	signalBell_.raise();
}

void ThreadGroup::issueThreadGroupSignal(int sn, SignalInfo info) {
	signalQueue_.issueSignal(sn, info, ++currentSignalSeq_);
}

void Process::issueThreadSignal(int sn, SignalInfo info) {
	signalQueue.issueSignal(sn, info, ++threadGroup()->currentSignalSeq_);
}

async::result<PollSignalResult>
Process::pollSignal(uint64_t in_seq, uint64_t mask, async::cancellation_token cancellation) {
	assert(in_seq <= threadGroup()->currentSignalSeq_);

	auto threadSignalUnavailable = [&]() {
		if (in_seq == threadGroup()->currentSignalSeq_)
			return true;
		if (!(signalQueue.activeSet_ & mask))
			return true;
		return false;
	};

	auto processSignalUnavailable = [&]() {
		if (in_seq == threadGroup()->currentSignalSeq_)
			return true;
		if (!(threadGroup()->signalQueue_.activeSet_ & mask))
			return true;
		return false;
	};

	while (threadSignalUnavailable() && processSignalUnavailable()
	       && !cancellation.is_cancellation_requested()) {
		co_await async::race_and_cancel(
		    [&](auto c) {
			    return async::transform(
			        signalQueue.signalBell_.async_wait_if(threadSignalUnavailable, c), [](auto) {}
			    );
		    },
		    [&](auto c) {
			    return async::transform(
			        threadGroup()->signalQueue_.signalBell_.async_wait_if(
			            processSignalUnavailable, c
			        ),
			        [](auto) {}
			    );
		    },
		    [&](auto c) {
			    return async::suspend_indefinitely(c, cancellation);
		    }
		);
	}

	uint64_t edges = 0;
	for(int sn = 1; sn <= 64; sn++) {
		if (signalQueue.slots_[sn - 1].raiseSeq > in_seq)
			edges |= UINT64_C(1) << (sn - 1);
		else if (threadGroup()->signalQueue().slots_[sn - 1].raiseSeq > in_seq)
			edges |= UINT64_C(1) << (sn - 1);
	}

	co_return PollSignalResult{threadGroup()->currentSignalSeq_, edges & mask};
}

CheckSignalResult Process::checkSignal() {
	return CheckSignalResult(threadGroup()->currentSignalSeq_, signalQueue.activeSet_ | threadGroup()->signalQueue_.activeSet_);
}

SignalItem * Process::tryFetchSignal(uint64_t mask) {
	for(int sn = 1; sn <= 64; sn++) {
		if(!(mask & (UINT64_C(1) << (sn - 1))))
			continue;

		// is there a pending signal on the thread queue?
		if(!signalQueue.slots_[sn - 1].asyncQueue.empty()) {
			auto item = signalQueue.slots_[sn - 1].asyncQueue.front();
			signalQueue.slots_[sn - 1].asyncQueue.pop_front();
			if(signalQueue.slots_[sn - 1].asyncQueue.empty())
				signalQueue.activeSet_ &= ~(UINT64_C(1) << (sn - 1));
			return item;
		}

		// is there a pending signal on the process queue?
		if(!threadGroup()->signalQueue_.slots_[sn - 1].asyncQueue.empty()) {
			auto item = threadGroup()->signalQueue_.slots_[sn - 1].asyncQueue.front();
			threadGroup()->signalQueue_.slots_[sn - 1].asyncQueue.pop_front();
			if(threadGroup()->signalQueue_.slots_[sn - 1].asyncQueue.empty())
				threadGroup()->signalQueue_.activeSet_ &= ~(UINT64_C(1) << (sn - 1));
			return item;
		}
	}

	return nullptr;
}

async::result<SignalItem *> Process::fetchSignal(uint64_t mask, bool nonBlock, async::cancellation_token ct) {
	auto seq = threadGroup()->currentSignalSeq_;

	while(true) {
		auto item = tryFetchSignal(mask);
		if(nonBlock || item)
			co_return item;

		std::tie(seq, std::ignore) = co_await pollSignal(seq, mask, ct);

		if (ct.is_cancellation_requested())
			co_return nullptr;
	}
}

// We follow a similar model as Linux. The linux layout is a follows:
// struct rt_sigframe. Placed at the top of the stack.
//     struct ucontext. Part of struct rt_sigframe.
//         struct sigcontext. Part of struct ucontext.
//             Actually stores the registers.
//             Stores a pointer to the FPU state.
//     siginfo_t. Part of struct rt_sigframe.
// FPU state is store at a higher (undefined) position on the stack.

// This is our signal frame, similar to Linux' struct rt_sigframe.
#if defined(__x86_64__)
struct SignalFrame {
	uintptr_t returnAddress;
	ucontext_t ucontext;
	siginfo_t info;
};
#else
// Return address for 'ret' is stored in X30 and not on the stack
struct SignalFrame {
	ucontext_t ucontext;
	siginfo_t info;
};
#endif

#if defined(__x86_64__)
constexpr size_t redZoneSize = 128;
// Calls misalign the stack by 8 bytes
// We later offset the stack by this amount because the ABI expects
// (rsp + 8) % 16 == 0 at function entry
constexpr size_t stackCallMisalign = 8;
#else
// The AAPCS64 ABI has no red zone
constexpr size_t redZoneSize = 0;
constexpr size_t stackCallMisalign = 0;
#endif

static const auto simdStateSize = [] () -> size_t {
	HelRegisterInfo regInfo;
	HEL_CHECK(helQueryRegisterInfo(kHelRegsSimd, &regInfo));
	return regInfo.setSize;
}();

SignalContext::SignalHandling SignalContext::determineHandling(SignalItem *item, Process *process) {
	SignalHandler handler = _handlers[item->signalNumber - 1];

	process->enterSignal();

	SignalContext::SignalHandling result {
		.handler = handler,
	};

	// Implement SA_RESETHAND by resetting the signal disposition to default.
	if(handler.flags & signalOnce)
		_handlers[item->signalNumber - 1].disposition = SignalDisposition::none;

	// Handle default dispositions properly
	if(handler.disposition == SignalDisposition::none) {
		switch (item->signalNumber) {
			// TODO: Handle SIGTSTP, SIGSTOP and SIGCONT
			case SIGCHLD:
			case SIGURG:
			case SIGWINCH:
				// Ignore the signal.
				result.ignored = true;
				break;
			default:
				result.killed = true;
				break;
		}
	} else if(handler.disposition == SignalDisposition::ignore) {
		// Ignore the signal.
		result.ignored = true;
	} else {
		assert(handler.disposition == SignalDisposition::handle);
	}

	return result;
}

async::result<void> SignalContext::raiseContext(SignalItem *item, Process *process,
		SignalContext::SignalHandling handling) {
	if (handling.ignored) {
		delete item;
		co_return;
	}

	if (handling.handler.disposition == SignalDisposition::none) {
		switch (item->signalNumber) {
			case SIGABRT:
			case SIGILL:
			case SIGSEGV:
				process->dumpRegisters();
				if(dumpCores) {
					co_await process->coredump(TerminationBySignal{item->signalNumber});
				}
				if(debugFaults) {
					std::cout << "posix: Thread " << process->tid() << " killed as the result of signal "
					<< item->signalNumber << std::endl;
					launchGdbServer(process);
					co_await async::suspend_indefinitely(async::cancellation_token{});
				}
				break;
			default:
				std::cout << "posix: Thread " << process->tid() << " killed as the result of signal "
					<< item->signalNumber << std::endl;
				assert(handling.killed);
				break;
		}
	}

	if (handling.killed) {
		co_await process->terminate();
		co_await process->threadGroup()->terminateGroup(TerminationBySignal{item->signalNumber});
		delete item;
		co_return;
	}

	auto thread = process->threadDescriptor();
	SignalFrame sf;
	memset(&sf, 0, sizeof(SignalFrame));

#if defined (__x86_64__)
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));

	sf.returnAddress = handling.handler.restorerIp;
#elif defined(__aarch64__)
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext));
#elif defined(__riscv) && __riscv_xlen == 64
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#else
#error Signal register loading code is missing for architecture
#endif

	auto rawProcessMask = process->signalMask();
	auto rawHandlerMask = rawProcessMask | handling.handler.mask;

	memcpy(&sf.ucontext.uc_sigmask, &rawProcessMask, sizeof(rawProcessMask));

	if (!(handling.handler.flags & signalReentrant))
		rawHandlerMask |= (1 << (item->signalNumber - 1));

	process->setSignalMask(rawHandlerMask);

	std::vector<std::byte> simdState(simdStateSize);
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSimd, simdState.data()));

	// Once compile siginfo_t if that is neccessary (matches Linux behavior).
	if(handling.handler.flags & signalInfo) {
		sf.info.si_signo = item->signalNumber;
		std::visit(CompileSignalInfo{&sf.info}, item->info);
	}

	// Setup the stack frame.
#if defined(__x86_64__)
	uintptr_t threadSp = sf.ucontext.uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
	uintptr_t threadSp = sf.ucontext.uc_mcontext.sp;
#elif defined(__riscv) && __riscv_xlen == 64
	uintptr_t threadSp = sf.ucontext.uc_mcontext.gregs[REG_SP];
#else
#error Code to get stack pointer is missing for architecture
#endif

	if (handling.handler.flags & signalOnStack && process->isAltStackEnabled()) {
		if (!process->isOnAltStack(threadSp)) {
			threadSp = process->altStackSp() + process->altStackSize();
		}
	}

	uintptr_t nsp = threadSp - redZoneSize;

	auto alignFrame = [&] (size_t size) -> uintptr_t {
		nsp = ((nsp - size) & ~uintptr_t(15)) - stackCallMisalign;
		return nsp;
	};

	size_t totalFrameSize = simdStateSize + sizeof(SignalFrame);

	// Store the current register stack on the stack.
	auto frame = alignFrame(totalFrameSize);
	assert((frame & (alignof(SignalFrame) - 1)) == 0);

#if defined(__x86_64__)
	sf.ucontext.uc_mcontext.fpregs = (struct _fpstate*)(frame + sizeof(SignalFrame));
#endif
	// TODO: aarch64

	auto storeFrame = co_await helix_ng::writeMemory(thread, frame,
			sizeof(SignalFrame), &sf);
	auto storeSimd = co_await helix_ng::writeMemory(thread, frame + sizeof(SignalFrame),
			simdStateSize, simdState.data());
	HEL_CHECK(storeFrame.error());
	HEL_CHECK(storeSimd.error());

	if(logSignals) {
		std::cout << "posix: Saving pre-signal stack to " << (void *)frame << std::endl;
		std::cout << "posix: Calling signal handler at " << (void *)handling.handler.handlerIp << std::endl;
	}
	// Setup the new register image and resume.
#if defined(__x86_64__)
	sf.ucontext.uc_mcontext.gregs[REG_RDI] = item->signalNumber;
	sf.ucontext.uc_mcontext.gregs[REG_RSI] = frame + offsetof(SignalFrame, info);
	sf.ucontext.uc_mcontext.gregs[REG_RDX] = frame + offsetof(SignalFrame, ucontext);
	sf.ucontext.uc_mcontext.gregs[REG_RAX] = 0; // Number of variable arguments.

	sf.ucontext.uc_mcontext.gregs[REG_RIP] = handling.handler.handlerIp;
	sf.ucontext.uc_mcontext.gregs[REG_RSP] = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#elif defined(__aarch64__)
	sf.ucontext.uc_mcontext.regs[0] = item->signalNumber;
	sf.ucontext.uc_mcontext.regs[1] = frame + offsetof(SignalFrame, info);
	sf.ucontext.uc_mcontext.regs[2] = frame + offsetof(SignalFrame, ucontext);

	// Return address for the 'ret' instruction
	sf.ucontext.uc_mcontext.regs[30] = handling.handler.restorerIp;

	sf.ucontext.uc_mcontext.pc = handling.handler.handlerIp;
	sf.ucontext.uc_mcontext.sp = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext));
#elif defined(__riscv) && __riscv_xlen == 64
	sf.ucontext.uc_mcontext.gregs[REG_A0 + 0] = item->signalNumber;
	sf.ucontext.uc_mcontext.gregs[REG_A0 + 1] = frame + offsetof(SignalFrame, info);
	sf.ucontext.uc_mcontext.gregs[REG_A0 + 2] = frame + offsetof(SignalFrame, ucontext);

	sf.ucontext.uc_mcontext.gregs[REG_RA] = handling.handler.restorerIp;
	sf.ucontext.uc_mcontext.gregs[REG_PC] = handling.handler.handlerIp;
	sf.ucontext.uc_mcontext.gregs[REG_SP] = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#else
#error Signal frame register setup code is missing for architecture
#endif

	delete item;
}

async::result<void> SignalContext::determineAndRaiseContext(SignalItem *item, Process *process,
		bool &killed) {
	auto handling = determineHandling(item, process);
	killed = handling.killed;
	co_return co_await raiseContext(item, process, handling);
}

async::result<void> SignalContext::restoreContext(helix::BorrowedDescriptor thread, Process *process) {
	uintptr_t pcrs[2];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
	auto frame = pcrs[kHelRegSp] - stackCallMisalign;

	if(logSignals)
		std::cout << "posix: Restoring post-signal stack from " << (void *)frame << std::endl;

	std::vector<std::byte> simdState(simdStateSize);

	SignalFrame sf;
	auto loadFrame = co_await helix_ng::readMemory(thread, frame,
			sizeof(SignalFrame), &sf);
	auto loadSimd = co_await helix_ng::readMemory(thread, frame + sizeof(SignalFrame),
			simdStateSize, simdState.data());
	HEL_CHECK(loadFrame.error());
	HEL_CHECK(loadSimd.error());

	uint64_t rawSignalMask = 0;
	memcpy(&rawSignalMask, &sf.ucontext.uc_sigmask, sizeof(rawSignalMask));
	process->setSignalMask(rawSignalMask);

#if defined(__x86_64__)
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#elif defined(__aarch64__)
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext));
#elif defined(__riscv) && __riscv_xlen == 64
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#else
#error Signal register storing code is missing for architecture
#endif

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSimd, simdState.data()));
}

// ----------------------------------------------------------------------------
// Generation.
// ----------------------------------------------------------------------------

Generation::~Generation() {
	if(logCleanup_)
		std::cout << "\e[33mposix: Generation is destructed\e[39m" << std::endl;
}

// ----------------------------------------------------------------------------
// Process.
// ----------------------------------------------------------------------------

// PID 1 is reserved for the init process, therefore we start at 2.
ProcessId nextPid = 2;
std::map<ProcessId, PidHull *> globalPidMap;

PidHull::PidHull(pid_t pid)
: pid_{pid} {
	auto [it, success] = globalPidMap.insert({pid_, this});
	assert(success);
	(void)it;
}

PidHull::~PidHull() {
	auto it = globalPidMap.find(pid_);
	assert(it != globalPidMap.end());
	globalPidMap.erase(it);
}

void PidHull::initializeThreadGroup(ThreadGroup *tg) {
	threadGroup_ = tg->weak_from_this();
}

void PidHull::initializeProcess(Process *process) {
	process_ = process->weak_from_this();
}

void PidHull::initializeTerminalSession(TerminalSession *session) {
	// TODO: verify that no terminal session is associated with this PidHull.
	terminalSession_ = session->weak_from_this();
}

void PidHull::initializeProcessGroup(ProcessGroup *group) {
	// TODO: verify that no process group is associated with this PidHull.
	processGroup_ = group->weak_from_this();
}

std::shared_ptr<ThreadGroup> PidHull::getThreadGroup() {
	return threadGroup_.lock();
}

std::shared_ptr<Process> PidHull::getProcess() {
	return process_.lock();
}

std::shared_ptr<ProcessGroup> PidHull::getProcessGroup() {
	return processGroup_.lock();
}

std::shared_ptr<TerminalSession> PidHull::getTerminalSession() {
	return terminalSession_.lock();
}

std::shared_ptr<ThreadGroup> ThreadGroup::findThreadGroup(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->getThreadGroup();
}

std::shared_ptr<Process> Process::findProcess(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->getProcess();
}

Process::Process(ThreadGroup *threadGroup, std::shared_ptr<PidHull> tidHull)
: hull_{std::move(tidHull)}, tgPointer_{threadGroup->shared_from_this()} {

}

Process::~Process() {
	std::cout << std::format("\e[33mposix: Process {} is destructed\e[39m", tid()) << std::endl;

	// Unlink /proc/[pid]/task/[tid] here, as we want to keep it around even if we're a zombie.
	if(procfsTaskLink_) {
		procfsTaskLink_->unlinkSelf();
		procfsTaskLink_ = nullptr;
	}
}

ThreadGroup *Process::getParent() {
	return tgPointer_->parent_;
}

PidHull *Process::getPidHull() {
	return tgPointer_->hull_.get();
}

PidHull *Process::getTidHull() {
	return hull_.get();
}

int Process::pid() {
	assert(tgPointer_->hull_);
	return tgPointer_->hull_->getPid();
}

int Process::tid() {
	assert(hull_);
	return hull_->getPid();
}

std::shared_ptr<ProcessGroup> Process::pgPointer() {
	return threadGroup()->pgPointer();
}

async::result<void> Process::cancelEvent(uint64_t cancelId, int fd) {
	if (cancelId != 0) {
		if (fd == -1) {
			cancelEventRegistry_.cancel(helix_ng::CredentialsView{credentials_}, cancelId);
		} else {
			managarm::fs::CancelOperation req;
			req.set_cancellation_id(cancelId);

			auto file = fileContext()->getFile(fd);

			auto [offer, send_req, imbue_creds] =
			co_await helix_ng::exchangeMsgs(
				file->getPassthroughLane(),
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::imbueCredentials(_threadDescriptor.getHandle())
				)
			);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
		}
	}
}

bool Process::checkSignalRaise() {
	auto t = accessThreadPage();
	unsigned int gsf = __atomic_load_n(&t->globalSignalFlag, __ATOMIC_RELAXED);
	if(!gsf)
		return true;
	return false;
}

bool Process::checkOrRequestSignalRaise() {
	auto t = accessThreadPage();
	unsigned int gsf = __atomic_load_n(&t->globalSignalFlag, __ATOMIC_RELAXED);
	if(!gsf)
		return true;
	if(gsf == 1) {
		__atomic_store_n(&t->globalSignalFlag, 2, __ATOMIC_RELAXED);
	}else if(gsf != 2) {
		std::cout << "\e[33m" "posix: Ignoring unexpected value "
				<< gsf << " of global signal flag" "\e[39m" << std::endl;
	}
	return false;
}

void Process::dumpRegisters() {
	printf("\e[35m");
	auto thread = threadDescriptor();

	uintptr_t pcrs[2];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, pcrs));

	uintptr_t gprs[kHelNumGprs];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, gprs));

	auto ip = pcrs[0];
	auto sp = pcrs[1];

#if defined(__x86_64__)
	printf("rax: %.16lx, rbx: %.16lx, rcx: %.16lx\n", gprs[0], gprs[1], gprs[2]);
	printf("rdx: %.16lx, rdi: %.16lx, rsi: %.16lx\n", gprs[3], gprs[4], gprs[5]);
	printf(" r8: %.16lx,  r9: %.16lx, r10: %.16lx\n", gprs[6], gprs[7], gprs[8]);
	printf("r11: %.16lx, r12: %.16lx, r13: %.16lx\n", gprs[9], gprs[10], gprs[11]);
	printf("r14: %.16lx, r15: %.16lx, rbp: %.16lx\n", gprs[12], gprs[13], gprs[14]);
	printf("rip: %.16lx, rsp: %.16lx\n", pcrs[0], pcrs[1]);
#elif defined(__aarch64__)
	// Registers X0-X30 have indices 0-30
	for (int i = 0; i < 31; i += 3) {
		if (i != 30) {
			printf("x%02d: %.16lx, x%02d: %.16lx, x%02d: %.16lx\n", i, gprs[i], i + 1, gprs[i + 1], i + 2, gprs[i + 2]);
		} else {
			printf("x%d: %.16lx,  ip: %.16lx,  sp: %.16lx\n", i, gprs[i], pcrs[kHelRegIp], pcrs[kHelRegSp]);
		}
	}
#endif

	printf("Mappings:\n");
	for (auto mapping : *(vmContext())) {
		uintptr_t start = mapping.baseAddress();
		uintptr_t end = start + mapping.size();

		std::string path;
		if(mapping.backingFile().get()) {
			// TODO: store the ViewPath inside the mapping.
			ViewPath vp{fsContext()->getRoot().first,
					mapping.backingFile()->associatedLink()};

			// TODO: This code is copied from GETCWD, factor it out into a function.
			path = "";
			while(true) {
				if(vp == fsContext()->getRoot())
					break;

				// If we are at the origin of a mount point, traverse that mount point.
				ViewPath traversed;
				if(vp.second == vp.first->getOrigin()) {
					if(!vp.first->getParent())
						break;
					auto anchor = vp.first->getAnchor();
					assert(anchor); // Non-root mounts must have anchors in their parents.
					traversed = ViewPath{vp.first->getParent(), vp.second};
				}else{
					traversed = vp;
				}

				auto owner = traversed.second->getOwner();
				if(!owner) { // We did not reach the root.
					// TODO: Can we get rid of this case?
					path = "?" + path;
					break;
				}

				path = "/" + traversed.second->getName() + path;
				vp = ViewPath{traversed.first, owner->treeLink()};
			}
		}else{
			path = "anon";
		}

		printf("%016lx - %016lx %s %s%s%s %s + 0x%lx\n", start, end,
				mapping.isPrivate() ? "P" : "S",
				mapping.isExecutable() ? "x" : "-",
				mapping.isReadable() ? "r" : "-",
				mapping.isWritable() ? "w" : "-",
				path.c_str(),
				mapping.backingFileOffset());
		if(ip >= start && ip < end)
			printf("               ^ IP is 0x%lx bytes into this mapping\n", ip - start);
		if(sp >= start && sp < end)
			printf("               ^ Stack is 0x%lx bytes into this mapping\n", sp - start);
	}

	printf("\e[39m");
	fflush(stdout);
}

async::result<std::shared_ptr<ThreadGroup>> Process::init(std::string path) {
	auto hull = std::make_shared<PidHull>(1);
	auto threadGroup = ThreadGroup::init(hull);
	threadGroup->_uid = 0;
	threadGroup->_euid = 0;
	threadGroup->_gid = 0;
	threadGroup->_egid = 0;
	threadGroup->_suid = 0;
	threadGroup->_sgid = 0;

	auto process = std::make_shared<Process>(threadGroup.get(), std::move(hull));
	process->threadGroup()->associateProcess(process);
	size_t pos = path.rfind('/');
	assert(pos != std::string::npos);
	process->_path = path;
	process->_name = path.substr(pos + 1);
	process->_vmContext = VmContext::create();
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();
	process->threadGroup()->_signalContext = SignalContext::create();

	TerminalSession::initializeNewSession(threadGroup.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};
	new (process->_threadPageMapping.get()) posix::ThreadPage{};

	// The initial signal mask allows all signals.
	process->_signalMask = 0;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&process->_clientThreadPage)));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, fileTableMemorySize, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->getTidHull()->initializeThreadGroup(threadGroup.get());
	process->getTidHull()->initializeProcess(process.get());

	// TODO: Do not pass an empty argument vector?
	auto execOutcome = co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::vector<std::string>{}, std::vector<std::string>{},
			process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane(), process.get());
	if(!execOutcome)
		throw std::logic_error("Could not execute() init process");
	auto &execResult = execOutcome.value();

	process->_threadDescriptor = std::move(execResult.thread);
	process->_clientAuxBegin = execResult.auxBegin;
	process->_clientAuxEnd = execResult.auxEnd;
	process->_posixLane = std::move(server_lane);
	process->threadGroup()->didExecute_ = true;

	HEL_CHECK(helGetCredentials(process->_threadDescriptor.getHandle(), 0, process->credentials_.data()));

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->procfsTaskLink_ = procfs_root->createProcTaskDirectory(process.get());

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return threadGroup;
}

async::result<std::shared_ptr<Process>> Process::fork(std::shared_ptr<Process> original) {
	auto hull = std::make_shared<PidHull>(nextPid++);
	auto threadGroup = ThreadGroup::create(hull, original->threadGroup());
	auto process = std::make_shared<Process>(threadGroup, std::move(hull));
	process->threadGroup()->associateProcess(process);
	process->_path = original->path();
	process->_name = original->name();
	process->_vmContext = co_await VmContext::clone(original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);
	process->threadGroup()->_signalContext = SignalContext::clone(original->threadGroup()->_signalContext);

	original->pgPointer()->reassociateProcess(threadGroup);

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};
	new (process->_threadPageMapping.get()) posix::ThreadPage{};

	// Signal masks are copied on fork().
	process->_signalMask = original->_signalMask;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&process->_clientThreadPage)));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, fileTableMemorySize, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->_clientAuxBegin = original->_clientAuxBegin;
	process->_clientAuxEnd = original->_clientAuxEnd;
	process->threadGroup()->_uid = original->threadGroup()->_uid;
	process->threadGroup()->_euid = original->threadGroup()->_euid;
	process->threadGroup()->_gid = original->threadGroup()->_gid;
	process->threadGroup()->_egid = original->threadGroup()->_egid;
	process->threadGroup()->_suid = original->threadGroup()->_suid;
	process->threadGroup()->_sgid = original->threadGroup()->_sgid;
	process->getTidHull()->initializeThreadGroup(threadGroup);
	process->threadGroup()->didExecute_ = false;

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->procfsTaskLink_ = procfs_root->createProcTaskDirectory(process.get());

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			nullptr, nullptr, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);
	HEL_CHECK(helGetCredentials(process->_threadDescriptor.getHandle(), 0, process->credentials_.data()));

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	async::detach(serve(process, std::move(generation)));

	co_return process;
}

constexpr uint64_t supportedCloneFlags = (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
	CLONE_THREAD | CLONE_PARENT | CLONE_CLEAR_SIGHAND);

async::result<std::expected<std::shared_ptr<Process>, Error>>
Process::clone(std::shared_ptr<Process> original, void *ip, void *sp, posix::superCloneArgs *args) {
	if (args->flags & ~supportedCloneFlags) {
		std::println("posix: unexpected clone flags {:#x}", args->flags & ~supportedCloneFlags);
		co_return std::unexpected{Error::illegalArguments};
	}

	if ((args->flags & CLONE_PARENT) && original->pid() == 1) {
		std::println("posix: attempted clone with CLONE_PARENT from init!");
		co_return std::unexpected{Error::illegalArguments};
	}

	if ((args->flags & CLONE_SIGHAND) && (args->flags & CLONE_CLEAR_SIGHAND)) {
		co_return std::unexpected{Error::illegalArguments};
	}

	ThreadGroup *parentPtr = original->threadGroup();
	if (args->flags & CLONE_PARENT) {
		parentPtr = original->getParent();
	}

	ThreadGroup *threadGroup;

	if (args->flags & CLONE_THREAD) {
		threadGroup = original->tgPointer_.get();
	} else {
		auto pidHull = std::make_shared<PidHull>(nextPid++);
		threadGroup = ThreadGroup::create(std::move(pidHull), parentPtr);
		original->pgPointer()->reassociateProcess(threadGroup);
		threadGroup->getHull()->initializeThreadGroup(threadGroup);
	}

	auto tidHull = std::make_shared<PidHull>(nextPid++);
	auto process = std::make_shared<Process>(threadGroup, std::move(tidHull));
	process->_path = original->path();
	process->_name = original->name();

	if (args->flags & CLONE_VM)
		process->_vmContext = original->_vmContext;
	else
		process->_vmContext = co_await VmContext::clone(original->_vmContext);

	if (args->flags & CLONE_FS)
		process->_fsContext = original->_fsContext;
	else
		process->_fsContext = FsContext::clone(original->_fsContext);

	if (args->flags & CLONE_FILES)
		process->_fileContext = original->_fileContext;
	else
		process->_fileContext = FileContext::clone(original->_fileContext);

	if (args->flags & CLONE_SIGHAND) {
		process->threadGroup()->_signalContext = original->threadGroup()->_signalContext;
	} else {
		process->threadGroup()->_signalContext = SignalContext::clone(original->threadGroup()->_signalContext);
		if (args->flags & CLONE_CLEAR_SIGHAND)
			process->threadGroup()->_signalContext->resetHandlers();
	}

	if ((args->flags & CLONE_VM) && !(args->flags & CLONE_VFORK)) {
		// if CLONE_VM and !CLONE_VFORK, then sigaltstack is cleared
		process->setAltStackEnabled(false);
	} else {
		process->setAltStackEnabled(original->isAltStackEnabled());
		if (original->isAltStackEnabled())
			process->setAltStackSp(original->altStackSp(), original->altStackSize());
	}

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};
	new (process->_threadPageMapping.get()) posix::ThreadPage{};

	// Signal masks are copied on clone().
	process->_signalMask = original->_signalMask;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&process->_clientThreadPage)));

	process->_clientFileTable = original->_clientFileTable;
	process->_clientClkTrackerPage = original->_clientClkTrackerPage;

	process->_clientAuxBegin = original->_clientAuxBegin;
	process->_clientAuxEnd = original->_clientAuxEnd;

	if (!(args->flags & CLONE_THREAD)) {
		process->threadGroup()->_uid = original->threadGroup()->_uid;
		process->threadGroup()->_euid = original->threadGroup()->_euid;
		process->threadGroup()->_gid = original->threadGroup()->_gid;
		process->threadGroup()->_egid = original->threadGroup()->_egid;
		process->threadGroup()->_suid = original->threadGroup()->_suid;
		process->threadGroup()->_sgid = original->threadGroup()->_sgid;
	}

	process->getTidHull()->initializeProcess(process.get());
	process->threadGroup()->associateProcess(process);

	process->threadGroup()->didExecute_ = false;

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->procfsTaskLink_ = procfs_root->createProcTaskDirectory(process.get());

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			ip, sp, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);
	HEL_CHECK(helGetCredentials(process->_threadDescriptor.getHandle(), 0, process->credentials_.data()));

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	async::detach(serve(process, std::move(generation)));

	co_return process;
}

async::result<Error> Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env) {
	auto exec_vm_context = VmContext::create();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto execResult = FRG_CO_TRY(co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::move(args), std::move(env), exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane(), process.get()));

	// Allocate resources.
	HelHandle exec_posix_lane;
	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &exec_posix_lane
	));
	client_lane.release();

	posix::ThreadPage *exec_thread_page;
	void *exec_clk_tracker_page;
	void *exec_client_table;
	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&exec_thread_page)));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&exec_clk_tracker_page));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, fileTableMemorySize, kHelMapProtRead,
			&exec_client_table));

	// Kill the old thread.
	// After this is done, we cannot roll back the exec() operation.
	HEL_CHECK(helKillThread(process->_threadDescriptor.getHandle()));
	auto previousGeneration = process->_currentGeneration;
	previousGeneration->inTermination = true;
	previousGeneration->cancelServe.cancel();
	co_await previousGeneration->signalsDone.wait();
	co_await previousGeneration->requestsDone.wait();

	// Perform pre-exec() work.
	// From here on, we can now release resources of the old process image.
	process->_fileContext->closeOnExec();

	// "Commit" the exec() operation.
	size_t pos = path.rfind('/');
	assert(pos != std::string::npos);
	process->_name = path.substr(pos + 1);
	process->_path = std::move(path);
	process->_posixLane = std::move(server_lane);
	process->_threadDescriptor = std::move(execResult.thread);
	process->_vmContext = std::move(exec_vm_context);
	process->threadGroup()->_signalContext->resetHandlers();
	process->setAltStackEnabled(false);
	process->setAltStackSp(0, 0);

	// Handle setuid and friends
	process->threadGroup()->_euid = execResult.effectiveUid;
	process->threadGroup()->_egid = execResult.effectiveGid;
	process->threadGroup()->_suid = execResult.savedUid;
	process->threadGroup()->_sgid = execResult.savedGid;

	process->_clientThreadPage = exec_thread_page;
	process->_clientPosixLane = exec_posix_lane;
	process->_clientFileTable = exec_client_table;
	process->_clientClkTrackerPage = exec_clk_tracker_page;
	process->_clientAuxBegin = execResult.auxBegin;
	process->_clientAuxEnd = execResult.auxEnd;
	process->threadGroup()->didExecute_ = true;
	HEL_CHECK(helGetCredentials(process->_threadDescriptor.getHandle(), 0, process->credentials_.data()));

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return Error::success;
}

async::result<void> Process::terminate(bool *lastInGroup) {
	auto parent = getParent();
	assert(parent);

	// Kill the current thread and accumulate stats.
	HEL_CHECK(helKillThread(_threadDescriptor.getHandle()));
	_currentGeneration->inTermination = true;
	_currentGeneration->cancelServe.cancel();
	co_await _currentGeneration->signalsDone.wait();
	co_await _currentGeneration->requestsDone.wait();

	// TODO: Also do this before switching to a new Generation in execve().
	// TODO: Do the accumulation + _currentGeneration reset after the thread has really terminated?
	HelThreadStats stats;
	HEL_CHECK(helQueryThreadStats(_threadDescriptor.getHandle(), &stats));
	threadGroup()->_generationUsage.userTime += stats.userTime;

	_posixLane = {};
	_threadDescriptor = {};
	_vmContext = nullptr;
	_fsContext = nullptr;
	_fileContext = nullptr;
	//_signalContext = nullptr; // TODO: Migrate the notifications to PID 1.
	_currentGeneration = nullptr;

	parent->_childrenUsage.userTime += threadGroup()->_generationUsage.userTime;

	std::erase_if(tgPointer_->threads_, [&](auto e) {
		return e.get() == this;
	});
	if (lastInGroup)
		*lastInGroup = tgPointer_->threads_.empty();
	tgPointer_->processTerminationEvent_.raise();
}

async::result<frg::expected<Error, Process::WaitResult>>
Process::wait(int pid, WaitFlags flags, async::cancellation_token ct) {
	if(!(flags & waitExited)) {
		std::cout << "posix: Unsupported arguments for flags passed to process::wait, waitExited must be set" << std::endl;
		co_return Error::illegalArguments;
	}
	if(flags & ~(waitNonBlocking | waitExited | waitLeaveZombie)) {
		std::cout << "posix: Unsupported arguments for flags passed to process::wait, unknown flags are set" << std::endl;
		co_return Error::illegalArguments;
	}

	// Check if a notification queue entry for `tg` matches the requested set.
	auto notificationMatch = [&](ThreadGroup *tg) -> bool {
		if (pid > 0) {
			// match a PID
			return pid == tg->pid();
		} else if (pid == 0) {
			// match any child process in the same process group
			return pgPointer() == tg->pgPointer();
		} else if (pid == -1) {
			// match any child process
			return true;
		} else {
			// match any child process with the PGID of abs(pid)
			return abs(pid) == tg->pgPointer()->getHull()->getPid();
		}
	};

	// Check if there are still children or notifications left that match the requested set.
	auto hasMatchingChildren = [&]() -> bool {
		if (pid < -1 || pid == 0) {
			int pgid = (pid == 0) ? pgPointer()->getHull()->getPid() : abs(pid);

			// check if there are this Process' children left in the process group `pgid`
			auto matchChildren =
			    std::ranges::any_of(threadGroup()->_children, [pgid](const auto &c) {
				    return c->pgPointer() && c->pgPointer()->getHull()->getPid() == pgid;
			    });

			// check if there are notifications queued for children that are members of
			// the process group `pgid`
			auto matchNotifications =
			    std::ranges::any_of(threadGroup()->_notifyQueue, [pgid](const auto &c) {
				    return c->pgPointer() && c->pgPointer()->getHull()->getPid() == pgid;
			    });

			return matchChildren || matchNotifications;
		} else {
			return !threadGroup()->_children.empty() || !threadGroup()->_notifyQueue.empty();
		}
	};

	if(!hasMatchingChildren() || (pid > 0 && !hasChild(pid)))
		co_return Error::noChildProcesses;

	std::optional<WaitResult> result{};
	while(true) {
		for(auto it = threadGroup()->_notifyQueue.begin(); it != threadGroup()->_notifyQueue.end(); ++it) {
			if(!notificationMatch(*it))
				continue;
			if(std::holds_alternative<TerminationByExit>((*it)->_state) && !(flags & (waitExited)))
				continue;
			if(std::holds_alternative<TerminationBySignal>((*it)->_state) && !(flags & (waitExited)))
				continue;

			result = WaitResult{
				.pid = (*it)->pid(),
				.uid = (*it)->uid(),
				.state = (*it)->terminationState(),
				.stats = (*it)->selfUsage(),
			};

			if(!(flags & waitLeaveZombie)) {
				// erasing from the queue invalidates the iterator, so we take a pointer before
				auto tg = *it;
				threadGroup()->_notifyQueue.erase(it);
				ThreadGroup::retire(tg);
			}

			break;
		}

		if(result)
			co_return result.value();
		else if(flags & waitNonBlocking)
			co_return Error::wouldBlock;

		if (!co_await threadGroup()->_notifyBell.async_wait(ct))
			co_return Error::interrupted;

		if (!hasMatchingChildren())
			co_return Error::noChildProcesses;
	}
}

bool Process::hasChild(int pid) {
	return std::ranges::any_of(threadGroup()->_children, [pid](auto e) {
		return e->pid() == pid;
	});
}

async::result<bool> ThreadGroup::awaitNotifyTypeChange(async::cancellation_token token) {
	co_return co_await notifyTypeChange_.async_wait(token);
}

ThreadGroup::ThreadGroup(std::shared_ptr<PidHull> hull, ThreadGroup *parent)
: parent_{parent}, hull_{std::move(hull)} {

}

ThreadGroup::~ThreadGroup() {
	pgPointer()->dropProcess(this);
}

std::shared_ptr<ThreadGroup> ThreadGroup::init(std::shared_ptr<PidHull> hull) {
	initThreadGroup = std::make_shared<ThreadGroup>(std::move(hull), nullptr);
	return initThreadGroup;
}

ThreadGroup *ThreadGroup::create(std::shared_ptr<PidHull> hull, ThreadGroup *parent) {
	auto tg = std::make_shared<ThreadGroup>(std::move(hull), parent);
	parent->_children.push_back(tg);
	return tg.get();
}

async::result<void> ThreadGroup::terminateGroup(TerminationState state) {
	// Only the first terminate() call goes through.
	if (!std::holds_alternative<std::monostate>(_state))
		co_return;

	_state = std::move(state);

	// Cause all remaining processes to terminate and wait for them.
	// Note that the process that caused the thread group termination has already called terminate() by now,
	// so we do not need to worry about that process deadlocking if it waits for terminateGroup().
	while (true) {
		for (auto &process : threads_) {
			if (process->forceTermination)
				continue;
			process->forceTermination = true;
			HEL_CHECK(helInterruptThread(process->threadDescriptor().getHandle()));
		}

		if (threads_.empty())
			break;
		co_await processTerminationEvent_.async_wait();
	}

	if(realTimer)
		realTimer->cancel();

	// Walk up the chain until we hit a process that has no parent.
	if (!parent_) {
		std::println("posix: terminateGroup() called on init process");
		co_return;
	}
	auto reparentTo = parent_;
	while(reparentTo->parent_)
		reparentTo = reparentTo->parent_;

	for(auto it = _children.begin(); it != _children.end();) {
		(*it)->parent_ = reparentTo;
		reparentTo->_children.push_back((*it));

		// send the signal if it requested one on parent death
		if((*it)->parentDeathSignal_) {
			UserSignal info;
			info.pid = hull_->getPid();
			(*it)->issueThreadGroupSignal((*it)->parentDeathSignal_.value(), info);
		}

		it = _children.erase(it);
	}

	auto reparentSigchldHandling = reparentTo->signalContext()->getHandler(SIGCHLD);
	bool ringReparent = false;
	while (!_notifyQueue.empty()) {
		auto child = _notifyQueue.pop_front();
		ringReparent = true;
		if (reparentSigchldHandling.disposition != SignalDisposition::ignore && !(reparentSigchldHandling.flags & signalNoChildWait))
			reparentTo->_notifyQueue.push_back(child);
	}

	if(ringReparent)
		reparentTo->_notifyBell.raise();

	// Notify the parent of our status change.
	// We need to do that even when not sending SIGCHLD since it wakes up pidfd.
	assert(notifyType_ == NotifyType::null);
	notifyType_ = NotifyType::terminated;
	notifyTypeChange_.raise();

	auto sigchldHandling = parent_->signalContext()->getHandler(SIGCHLD);
	if (sigchldHandling.disposition != SignalDisposition::ignore && !(sigchldHandling.flags & signalNoChildWait)) {
		parent_->_notifyQueue.push_back(this);
		parent_->_notifyBell.raise();

		// Send SIGCHLD to the parent.
		ChildSignal info;
		info.pid = hull_->getPid();
		info.utime = _generationUsage.userTime;

		if(std::get_if<TerminationByExit>(&_state)) {
			info.status = std::get<TerminationByExit>(_state).code;
			info.code = CLD_EXITED;
		} else if(std::get_if<TerminationBySignal>(&_state)) {
			info.status = std::get<TerminationBySignal>(_state).signo;
			info.code = dumpable_ ? CLD_DUMPED : CLD_KILLED;
		} else {
			std::println("posix: unhandled SIGCHLD reason");
		}

		parent_->issueThreadGroupSignal(SIGCHLD, info);
	} else {
		ThreadGroup::retire(this);
	}
}

void ThreadGroup::retire(ThreadGroup *tg) {
	tg->leader_ = nullptr;

	if (tg->procfsLink_) {
		tg->procfsLink_->unlinkSelf();
		tg->procfsLink_ = nullptr;
	}

	tg->parent_->_children.erase(
	    std::ranges::find_if(tg->parent_->_children, [pid = tg->pid()](auto e) {
		    return e->pid() == pid;
	    })
	);

	// We need to ring _notifyBell once the process is retired
	// since waitpid() returns an error once no children are left.
	tg->parent_->_notifyBell.raise();
}

void ThreadGroup::associateProcess(std::shared_ptr<Process> process) {
	assert(process->tgPointer_.get() == this);

	threads_.push_back(process);

	if (!leader_)
		leader_ = process;
}

std::shared_ptr<Process> ThreadGroup::findThread(pid_t tid) {
	for(auto t : threads_) {
		if (t->tid() == tid)
			return t->shared_from_this();
	}

	return {};
}

// --------------------------------------------------------------------------------------
// Process groups and sessions.
// --------------------------------------------------------------------------------------

std::shared_ptr<ProcessGroup> ProcessGroup::findProcessGroup(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->getProcessGroup();
}

ProcessGroup::ProcessGroup(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

ProcessGroup::~ProcessGroup() {
	sessionPointer_->dropGroup(this);
}

void ProcessGroup::reassociateProcess(ThreadGroup *process) {
	if(process->pgPointer()) {
		auto oldGroup = process->pgPointer().get();
		oldGroup->members_.erase(oldGroup->members_.iterator_to(process));
	}
	process->pgPointer_ = shared_from_this();
	members_.push_back(process);
}

void ProcessGroup::dropProcess(ThreadGroup *process) {
	assert(process->pgPointer().get() == this);
	members_.erase(members_.iterator_to(process));
	// Note: this assignment can destruct 'this'.
	process->pgPointer_ = nullptr;
}

void ProcessGroup::issueSignalToGroup(int sn, SignalInfo info) {
	for(auto processRef : members_)
		processRef->issueThreadGroupSignal(sn, info);
}

bool ProcessGroup::isOrphaned() {
	for (auto processRef : members_) {
		// If a member process has a parent that
		// a) is in the same session as the ProcessGroup, and
		// b) is not a member of the process group,
		// then the ProcessGroup is NOT orphaned
		if (processRef->getParent()
		    && processRef->getParent()->pgPointer()->getSession() == getSession()
		    && processRef->pgPointer() != getHull()->getProcessGroup())
			return false;
	}

	// If no parent-child relationships (yuck) can connect this group to the rest of the
	// session, we are orphaned.
	return true;
}

TerminalSession::TerminalSession(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

TerminalSession::~TerminalSession() {
	if(ctsPointer_)
		ctsPointer_->dropSession(this);
}

pid_t TerminalSession::getSessionId() {
	return hull_->getPid();
}

std::shared_ptr<TerminalSession> TerminalSession::initializeNewSession(ThreadGroup *sessionLeader) {
	auto session = std::make_shared<TerminalSession>(sessionLeader->getHull()->shared_from_this());
	auto group = session->spawnProcessGroup(sessionLeader);
	session->foregroundGroup_ = group.get();
	session->hull_->initializeTerminalSession(session.get());
	return session;
}

std::shared_ptr<ProcessGroup> TerminalSession::spawnProcessGroup(ThreadGroup *groupLeader) {
	auto group = std::make_shared<ProcessGroup>(groupLeader->getHull()->shared_from_this());
	group->reassociateProcess(groupLeader);
	group->sessionPointer_ = shared_from_this();
	groups_.push_back(group.get());
	group->hull_->initializeProcessGroup(group.get());
	return group;
}

std::shared_ptr<ProcessGroup> TerminalSession::getProcessGroupById(pid_t id) {
	for(auto i : groups_) {
		if(i->getHull()->getPid() == id)
			return i->getHull()->getProcessGroup()->shared_from_this();
	}
	return nullptr;
}

void TerminalSession::dropGroup(ProcessGroup *group) {
	assert(group->sessionPointer_.get() == this);
	if(foregroundGroup_ == group)
		foregroundGroup_ = nullptr;
	groups_.erase(groups_.iterator_to(group));
	// Note: this assignment can destruct 'this'.
	group->sessionPointer_ = nullptr;
}

Error TerminalSession::setForegroundGroup(ProcessGroup *group) {
	assert(group);
	if(group->sessionPointer_.get() != this)
		return Error::insufficientPermissions;
	foregroundGroup_ = group;
	return Error::success;
}

Error ControllingTerminalState::assignSessionOf(Process *process) {
	auto group = process->pgPointer().get();
	auto session = group->sessionPointer_.get();
	if(process->getPidHull() != session->hull_.get())
		return Error::illegalArguments; // Process is not a session leader.
	if(associatedSession_ == session)
		return Error::success;
	if(associatedSession_)
		return Error::insufficientPermissions;
	if(session->ctsPointer_)
		return Error::insufficientPermissions;
	associatedSession_ = session;
	session->ctsPointer_ = this;
	return Error::success;
}

void ControllingTerminalState::dropSession(TerminalSession *session) {
	assert(associatedSession_ == session);
	associatedSession_ = nullptr;
	session->ctsPointer_ = nullptr;
}

void ControllingTerminalState::issueSignalToForegroundGroup(int sn, SignalInfo info) {
	if(!associatedSession_)
		return;
	if(!associatedSession_->foregroundGroup_)
		return;
	associatedSession_->foregroundGroup_->issueSignalToGroup(sn, info);
}
