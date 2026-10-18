
#include <signal.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <limits>
#include <mutex>
#include <print>

#include <async/scope.hpp>
#include <frg/mutex.hpp>
#include <frg/std_compat.hpp>

#include "common.hpp"
#include <core/clock.hpp>
#include "exec.hpp"
#include "gdbserver.hpp"
#include "process.hpp"
#include "requests.hpp"
#include "swap.hpp"

#include <core/process-data.hpp>
#include <protocols/posix/data.hpp>
#include <helix/ipc.hpp>

#include "debug-options.hpp"

static bool logFileAttach = false;
static bool logCleanup_ = false;

async::result<void> serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation);

std::shared_ptr<ThreadGroup> initThreadGroup = nullptr;

namespace {

template<typename Id>
bool isNoChangeSetIdRequest(uint64_t requested) {
	return requested == static_cast<uint64_t>(-1)
		|| requested == static_cast<uint64_t>(std::numeric_limits<Id>::max());
}

template<typename Id>
Error resolveSetIdRequest(uint64_t requested, Id current, Id &result) {
	if(isNoChangeSetIdRequest<Id>(requested)) {
		result = current;
		return Error::success;
	}
	if(requested > static_cast<uint64_t>(std::numeric_limits<Id>::max()))
		return Error::illegalArguments;
	result = static_cast<Id>(requested);
	return Error::success;
}

} // namespace

helix::BorrowedDescriptor getZeroMemory() {
	static helix::UniqueDescriptor memory = [] {
		HelHandle handle;
		HEL_CHECK(helObtainHandle(kHelObtainZeroMemory, &handle));
		return helix::UniqueDescriptor{handle};
	}();

	return memory;
}

// ----------------------------------------------------------------------------
// Hierarchy helpers.
// ----------------------------------------------------------------------------

HelHandle rootHierarchy() {
	static HelHandle handle = [] {
		HelHierarchyParameters params{};
		strncpy(params.tag, "posix-root", sizeof(params.tag));
		// POSIX processes stop allocating before servers do, such that servers
		// (e.g., the ones that write back or swap out POSIX memory) can still make progress.
		params.controllers = kHelHierarchyMemoryController;
		params.memoryFloor = std::clamp(getTotalMemory() / 64,
				size_t{4} << 20, size_t{64} << 20);

		HelHandle child;
		HEL_CHECK(helExtendHierarchy(core::getProcessHierarchy(), &params, &child));
		return child;
	}();
	return handle;
}

HelHandle sharedHierarchy() {
	static HelHandle handle = [] {
		HelHierarchyParameters params{};
		strncpy(params.tag, "shared-memory", sizeof(params.tag));

		HelHandle child;
		HEL_CHECK(helExtendHierarchy(rootHierarchy(), &params, &child));
		return child;
	}();
	return handle;
}

namespace {
	helix::UniqueDescriptor extendHierarchyForProcess(ProcessId pid,
			std::string_view path) {
		std::string tag = "pid" + std::to_string(pid) + ":" + std::string{path};

		HelHierarchyParameters params{};
		strncpy(params.tag, tag.data(), sizeof(params.tag));

		HelHandle child;
		HEL_CHECK(helExtendHierarchy(rootHierarchy(), &params, &child));
		return helix::UniqueDescriptor{child};
	}
}

// ----------------------------------------------------------------------------
// VmContext.
// ----------------------------------------------------------------------------

std::shared_ptr<VmContext> VmContext::create(helix::UniqueDescriptor hierarchy) {
	auto context = std::make_shared<VmContext>();
	context->_hierarchy = std::move(hierarchy);

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	return context;
}

async::result<std::shared_ptr<VmContext>> VmContext::clone(
	helix::UniqueDescriptor hierarchy, std::shared_ptr<VmContext> original
) {
	auto context = std::make_shared<VmContext>();
	context->_hierarchy = std::move(hierarchy);

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	// The forks borrow the copy views of the original areas.
	co_await original->_mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, original->_mutex};

	co_await async::dynamic_scope<async::result<void>>(frg::stl_allocator{}, [&] (auto &spawn) {
		for(const auto &entry : original->_areaTree) {
			const auto &[address, area] = entry;

			Area copy;
			copy.copyOnWrite = area.copyOnWrite;
			copy.areaSize = area.areaSize;
			copy.nativeFlags = area.nativeFlags;
			copy.fileView = area.fileView.dup();
			copy.file = area.file;
			copy.offset = area.offset;
			copy.effectiveOffset = area.effectiveOffset;
			auto [it, inserted] = context->_areaTree.emplace(address, std::move(copy));
			assert(inserted);

			// Areas are independent, so fork and map all of them concurrently.
			spawn([] (VmContext *context, uintptr_t address,
					helix::BorrowedDescriptor originalCopyView, Area *copy) -> async::result<void> {
				if(copy->copyOnWrite) {
					auto forkResult = co_await helix_ng::forkMemory(context->_hierarchy, originalCopyView);
					HEL_CHECK(forkResult.error());
					copy->copyView = forkResult.descriptor();

					auto mapResult = co_await helix_ng::mapMemory(copy->copyView, context->_space,
							reinterpret_cast<void *>(address),
							copy->effectiveOffset, copy->areaSize, copy->nativeFlags);
					if(mapResult.error() != kHelErrNone && mapResult.error() != kHelErrAlreadyExists) {
						HEL_CHECK(mapResult.error());
					}
				}else{
					auto mapResult = co_await helix_ng::mapMemory(copy->fileView, context->_space,
							reinterpret_cast<void *>(address),
							copy->offset, copy->areaSize, copy->nativeFlags);
					HEL_CHECK(mapResult.error());
				}
			}(context.get(), address, area.copyView, &it->second));
		}
	});

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

	// Files of replaced areas are dropped after unlocking since that can close them.
	std::vector<smarter::shared_ptr<File, FileHandle>> dropped;
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	helix::UniqueDescriptor copyView;
	void *pointer = nullptr;
	HelError error;
	if(copyOnWrite) {
		helix::BorrowedDescriptor memoryDesc;
		if(memory) {
			memoryDesc = memory;
		} else {
			memoryDesc = getZeroMemory();
		}

		HelHandle handle;
		HEL_CHECK(helCopyOnWrite(_hierarchy.getHandle(),
				getSwapSpaceHandle().getHandle(),
				memoryDesc.getHandle(),
				offset, alignedSize, &handle));
		copyView = helix::UniqueDescriptor{handle};

		auto mapResult = co_await helix_ng::mapMemory(copyView, _space,
				reinterpret_cast<void *>(hint),
				0, alignedSize, nativeFlags);
		error = mapResult.error();
		if(!error)
			pointer = mapResult.pointer();
	}else{
		auto mapResult = co_await helix_ng::mapMemory(memory, _space,
				reinterpret_cast<void *>(hint),
				offset, alignedSize, nativeFlags);
		error = mapResult.error();
		if(!error)
			pointer = mapResult.pointer();
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
		auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize)) {
			dropped.push_back(std::move(area.file));
			it = _areaTree.erase(it);
		} else {
			++it;
		}
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

	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

//	std::cout << "posix: Remapping " << oldPointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(oldPointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == alignedOldSize);

	assert(!it->second.copyOnWrite);

	auto memory = co_await it->second.file->accessMemory();

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	auto mapResult = co_await helix_ng::mapMemory(memory, _space,
			nullptr, it->second.offset, alignedNewSize,
			it->second.nativeFlags);
	HEL_CHECK(mapResult.error());
	auto pointer = mapResult.pointer();
//	std::cout << "posix: VM_REMAP returns " << pointer << std::endl;

	// Unmap the old area.
	auto unmapResult = co_await helix_ng::unmapMemory(_space, oldPointer, alignedOldSize);
	HEL_CHECK(unmapResult.error());

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

	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

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

async::result<void> VmContext::unmapFile(void *pointer, size_t size) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);
	auto address = reinterpret_cast<uintptr_t>(pointer);

	// Files of unmapped areas are dropped after unlocking since that can close them.
	std::vector<smarter::shared_ptr<File, FileHandle>> dropped;
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	auto unmapResult = co_await helix_ng::unmapMemory(_space, pointer, alignedSize);
	HEL_CHECK(unmapResult.error());

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt;) {
		auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize)) {
			dropped.push_back(std::move(area.file));
			it = _areaTree.erase(it);
		} else {
			++it;
		}
	}
}

async::result<std::vector<VmContext::AreaSnapshot>> VmContext::snapshotAreas() {
	co_await _mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, _mutex};
	co_return snapshotAreasLocked_();
}

std::optional<std::vector<VmContext::AreaSnapshot>> VmContext::trySnapshotAreas() {
	if(!_mutex.try_lock_shared())
		return std::nullopt;
	frg::shared_lock lock{frg::adopt_lock, _mutex};
	return snapshotAreasLocked_();
}

std::vector<VmContext::AreaSnapshot> VmContext::snapshotAreasLocked_() {
	std::vector<AreaSnapshot> areas;
	for(const auto &[address, area] : _areaTree) {
		areas.push_back(AreaSnapshot{
			.baseAddress_ = address,
			.size_ = area.areaSize,
			.copyOnWrite_ = area.copyOnWrite,
			.nativeFlags_ = area.nativeFlags,
			.file_ = area.file,
			.offset_ = area.offset,
		});
	}
	return areas;
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

	std::lock_guard lock{original->_mutex};
	context->_root = original->_root;
	context->_workDir = original->_workDir;
	context->_umask = original->_umask;

	return context;
}

ViewPath FsContext::getRoot() {
	std::lock_guard lock{_mutex};
	return _root;
}

ViewPath FsContext::getWorkingDirectory() {
	std::lock_guard lock{_mutex};
	return _workDir;
}

std::expected<void, Error> FsContext::changeRoot(ViewPath root) {
	if(root.second->getTarget()->getType() != VfsType::directory)
		return std::unexpected(Error::notDirectory);
	std::lock_guard lock{_mutex};
	std::swap(_root, root);
	return {};
}

std::expected<void, Error> FsContext::changeWorkingDirectory(ViewPath workdir) {
	if(workdir.second->getTarget()->getType() != VfsType::directory)
		return std::unexpected(Error::notDirectory);
	std::lock_guard lock{_mutex};
	std::swap(_workDir, workdir);
	return {};
}

mode_t FsContext::getUmask() {
	std::lock_guard lock{_mutex};
	return _umask;
}

mode_t FsContext::setUmask(mode_t mask) {
	std::lock_guard lock{_mutex};
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
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x1000, 0, nullptr, &memory));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->fileTableWindow_ = helix::Mapping{context->_fileTableMemory, 0, 0x1000};

	HEL_CHECK(helTransferDescriptor(
	    posixMbusClient,
		context->_universe.getHandle(),
		kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
		&context->_clientMbusLane
	));

	return context;
}

std::shared_ptr<FileContext> FileContext::clone(std::shared_ptr<FileContext> original) {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x1000, 0, nullptr, &memory));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->fileTableWindow_ = helix::Mapping{context->_fileTableMemory, 0, 0x1000};

	for(auto entry : original->snapshotFileTable()) {
		//std::cout << "Clone FD " << entry.first << std::endl;

		// the new number of fd in the file context is always <= the number of fd in the original
		auto attachRes = context->attachFile(entry.first, entry.second.file, entry.second.closeOnExec);
		assert(attachRes);
	}

	HEL_CHECK(helTransferDescriptor(
	    posixMbusClient,
		context->_universe.getHandle(),
		kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
		&context->_clientMbusLane
	));

	return context;
}

FileContext::~FileContext() {
	if(logCleanup_)
		std::cout << "\e[33mposix: FileContext is destructed\e[39m" << std::endl;
}

std::expected<int, Error> FileContext::attachFile(smarter::shared_ptr<File, FileHandle> file,
		bool closeOnExec, int startAt) {
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(
	    file->getPassthroughLane().getHandle(),
		_universe.getHandle(),
		kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
		&handle
	));

	std::lock_guard lock{_mutex};
	for(int fd = startAt; ; fd++) {
		if(_fileTable.find(fd) != _fileTable.end())
			continue;

		if (static_cast<uint64_t>(fd) >= fdLimit_)
			return std::unexpected{Error::noFileDescriptorsAvailable};

		if(logFileAttach)
			std::cout << "posix: Attaching FD " << fd << std::endl;

		_fileTable.insert({fd, {std::move(file), closeOnExec}});
		fileTableWindow()[fd] = handle;
		return fd;
	}
}

std::expected<void, Error> FileContext::attachFile(int fd, smarter::shared_ptr<File, FileHandle> file,
		bool close_on_exec) {
	{
		std::lock_guard lock{_mutex};
		if (static_cast<uint64_t>(fd) >= fdLimit_)
			return std::unexpected{Error::noFileDescriptorsAvailable};
	}

	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(
	    file->getPassthroughLane().getHandle(),
		_universe.getHandle(),
		kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
		&handle
	));

	if(logFileAttach)
		std::cout << "posix: Attaching fixed FD " << fd << std::endl;

	FileDescriptor displaced;
	std::lock_guard lock{_mutex};
	auto it = _fileTable.find(fd);
	if(it != _fileTable.end()) {
		displaced = std::exchange(it->second, {std::move(file), close_on_exec});
	}else{
		_fileTable.insert({fd, {std::move(file), close_on_exec}});
	}
	fileTableWindow()[fd] = handle;

	return {};
}

std::optional<FileDescriptor> FileContext::getDescriptor(int fd) {
	std::lock_guard lock{_mutex};
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return std::nullopt;
	return file->second;
}

Error FileContext::setDescriptor(int fd, bool close_on_exec) {
	std::lock_guard lock{_mutex};
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}
	it->second.closeOnExec = close_on_exec;
	return Error::success;
}

smarter::shared_ptr<File, FileHandle> FileContext::getFile(int fd) {
	std::lock_guard lock{_mutex};
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return smarter::shared_ptr<File, FileHandle>{};
	return file->second.file;
}

Error FileContext::closeFile(int fd) {
	if(logFileAttach)
		std::cout << "posix: Closing FD " << fd << std::endl;
	FileDescriptor closed;
	std::lock_guard lock{_mutex};
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}

	HEL_CHECK(helCloseDescriptor(_universe.getHandle(), fileTableWindow()[fd]));

	fileTableWindow()[fd] = 0;
	closed = std::move(it->second);
	_fileTable.erase(it);
	return Error::success;
}

void FileContext::closeOnExec() {
	std::vector<FileDescriptor> closed;
	std::lock_guard lock{_mutex};
	auto it = _fileTable.begin();
	while(it != _fileTable.end()) {
		if(it->second.closeOnExec) {
			HEL_CHECK(helCloseDescriptor(_universe.getHandle(), fileTableWindow()[it->first]));

			fileTableWindow()[it->first] = 0;
			closed.push_back(std::move(it->second));
			it = _fileTable.erase(it);
		}else{
			it++;
		}
	}
}

std::unordered_map<int, FileDescriptor> FileContext::snapshotFileTable() {
	std::lock_guard lock{_mutex};
	return _fileTable;
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
	std::lock_guard lock{original->_mutex};
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1] = original->_handlers[sn - 1];

	return context;
}

void SignalContext::resetHandlers() {
	std::lock_guard lock{_mutex};
	for(int sn = 1; sn <= 64; sn++)
		if(_handlers[sn - 1].disposition == SignalDisposition::handle)
			_handlers[sn - 1] = SignalHandler{SignalDisposition::none};
}

SignalHandler SignalContext::getHandler(int sn) {
	assert(sn > 0);
	assert(sn - 1 < 64);
	std::lock_guard lock{_mutex};
	return _handlers[sn - 1];
}

SignalHandler SignalContext::changeHandler(int sn, SignalHandler handler) {
	assert(sn > 0);
	assert(sn - 1 < 64);
	std::lock_guard lock{_mutex};
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
}

void ThreadGroup::issueThreadGroupSignal(int sn, SignalInfo info) {
	{
		std::lock_guard lock{signalMutex_};
		signalQueue_.issueSignal(sn, info, ++currentSignalSeq_);
	}
	signalQueue_.signalBell_.raise();
}

void Process::issueThreadSignal(int sn, SignalInfo info) {
	auto tg = threadGroup();
	{
		std::lock_guard lock{tg->signalMutex_};
		signalQueue.issueSignal(sn, info, ++tg->currentSignalSeq_);
	}
	signalQueue.signalBell_.raise();
}

async::result<PollSignalResult>
Process::pollSignal(uint64_t in_seq, uint64_t mask, async::cancellation_token cancellation) {
	auto tg = threadGroup();
	{
		std::lock_guard lock{tg->signalMutex_};
		assert(in_seq <= tg->currentSignalSeq_);
	}

	auto threadSignalUnavailable = [&]() {
		std::lock_guard lock{tg->signalMutex_};
		if (in_seq == tg->currentSignalSeq_)
			return true;
		if (!(signalQueue.activeSet_ & mask))
			return true;
		return false;
	};

	auto processSignalUnavailable = [&]() {
		std::lock_guard lock{tg->signalMutex_};
		if (in_seq == tg->currentSignalSeq_)
			return true;
		if (!(tg->signalQueue_.activeSet_ & mask))
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

	std::lock_guard lock{tg->signalMutex_};
	uint64_t edges = 0;
	for(int sn = 1; sn <= 64; sn++) {
		if (signalQueue.slots_[sn - 1].raiseSeq > in_seq)
			edges |= UINT64_C(1) << (sn - 1);
		else if (tg->signalQueue().slots_[sn - 1].raiseSeq > in_seq)
			edges |= UINT64_C(1) << (sn - 1);
	}

	co_return PollSignalResult{tg->currentSignalSeq_, edges & mask};
}

CheckSignalResult Process::checkSignal() {
	std::lock_guard lock{threadGroup()->signalMutex_};
	return CheckSignalResult(threadGroup()->currentSignalSeq_, signalQueue.activeSet_ | threadGroup()->signalQueue_.activeSet_);
}

SignalItem * Process::tryFetchSignal(uint64_t mask) {
	std::lock_guard lock{threadGroup()->signalMutex_};
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
	uint64_t seq;
	{
		std::lock_guard lock{threadGroup()->signalMutex_};
		seq = threadGroup()->currentSignalSeq_;
	}

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

SignalContext::SignalHandling SignalContext::acceptSignal(SignalItem *item, Process *process) {
	SignalHandler handler;
	{
		std::lock_guard lock{_mutex};
		handler = _handlers[item->signalNumber - 1];

		// Implement SA_RESETHAND by resetting the signal disposition to default.
		if(handler.flags & signalOnce)
			_handlers[item->signalNumber - 1].disposition = SignalDisposition::none;
	}

	process->enterSignal();

	SignalContext::SignalHandling result {
		.handler = handler,
	};

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

async::result<void> SignalContext::acceptSignalAndRaiseContext(SignalItem *item, Process *process,
		bool &killed) {
	auto handling = acceptSignal(item, process);
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
std::atomic<ProcessId> nextPid = 2;

std::shared_mutex processGraphMutex;

namespace {

// Leaf lock that protects globalPidMap and the weak pointers in all PidHulls.
// ~PidHull erases its entry under this lock, so mapped hulls stay alive while it is held.
std::mutex pidMapMutex;
std::map<ProcessId, PidHull *> globalPidMap;

} // anonymous namespace

PidHull::PidHull(pid_t pid)
: pid_{pid} {
	std::lock_guard lock{pidMapMutex};
	auto [it, success] = globalPidMap.insert({pid_, this});
	assert(success);
	(void)it;
}

PidHull::~PidHull() {
	std::lock_guard lock{pidMapMutex};
	auto it = globalPidMap.find(pid_);
	assert(it != globalPidMap.end());
	globalPidMap.erase(it);
}

void PidHull::initializeThreadGroup(ThreadGroup *tg) {
	std::lock_guard lock{pidMapMutex};
	threadGroup_ = tg->weak_from_this();
}

void PidHull::initializeProcess(Process *process) {
	std::lock_guard lock{pidMapMutex};
	process_ = process->weak_from_this();
}

void PidHull::initializeTerminalSession(TerminalSession *session) {
	std::lock_guard lock{pidMapMutex};
	// TODO: verify that no terminal session is associated with this PidHull.
	terminalSession_ = session->weak_from_this();
}

void PidHull::initializeProcessGroup(ProcessGroup *group) {
	std::lock_guard lock{pidMapMutex};
	// TODO: verify that no process group is associated with this PidHull.
	processGroup_ = group->weak_from_this();
}

std::shared_ptr<ThreadGroup> PidHull::getThreadGroup() {
	std::lock_guard lock{pidMapMutex};
	return threadGroup_.lock();
}

std::shared_ptr<Process> PidHull::getProcess() {
	std::lock_guard lock{pidMapMutex};
	return process_.lock();
}

std::shared_ptr<ProcessGroup> PidHull::getProcessGroup() {
	std::lock_guard lock{pidMapMutex};
	return processGroup_.lock();
}

std::shared_ptr<TerminalSession> PidHull::getTerminalSession() {
	std::lock_guard lock{pidMapMutex};
	return terminalSession_.lock();
}

std::shared_ptr<ThreadGroup> ThreadGroup::findThreadGroup(ProcessId pid) {
	std::lock_guard lock{pidMapMutex};
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->threadGroup_.lock();
}

std::shared_ptr<Process> Process::findProcess(ProcessId pid) {
	std::lock_guard lock{pidMapMutex};
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->process_.lock();
}

Process::Process(ThreadGroup *threadGroup, std::shared_ptr<PidHull> tidHull)
: hull_{std::move(tidHull)}, tgPointer_{threadGroup->shared_from_this()} {

}

// Processes are unlinked from procfs and the process graph by terminate() and
// ThreadGroup::retireLocked_(), such that the last reference can be dropped on any thread.
Process::~Process() {
	std::cout << std::format("\e[33mposix: Process {} is destructed\e[39m", tid()) << std::endl;
	assert(!procfsTaskLink_);
}

void Process::unlinkProcfsTask_() {
	if(procfsTaskLink_) {
		procfsTaskLink_->unlinkSelf();
		procfsTaskLink_ = nullptr;
	}
}

std::shared_ptr<ThreadGroup> Process::getParent() {
	return tgPointer_->getParent();
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

void Process::interruptThread() {
	std::lock_guard lock{taskMutex_};
	if(_threadDescriptor)
		HEL_CHECK(helInterruptThread(_threadDescriptor.getHandle()));
}

HelError Process::setAffinity(uint8_t *mask, size_t size) {
	std::lock_guard lock{taskMutex_};
	if(!_threadDescriptor)
		return kHelErrThreadTerminated;
	return helSetAffinity(_threadDescriptor.getHandle(), mask, size);
}

HelError Process::getAffinity(uint8_t *mask, size_t size, size_t *actualSize) {
	std::lock_guard lock{taskMutex_};
	if(!_threadDescriptor)
		return kHelErrThreadTerminated;
	return helGetAffinity(_threadDescriptor.getHandle(), mask, size, actualSize);
}

void Process::startServe() {
	async::detach(serve(shared_from_this(), _currentGeneration));
}

void Process::cancelPosixRequest(uint64_t cancelId) {
	if (cancelId != 0)
		cancelEventRegistry_.cancel(helix_ng::CredentialsView{credentials_}, cancelId);
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
	auto areas = vmContext()->trySnapshotAreas();
	if(!areas)
		printf("(unavailable while the address space is being modified)\n");
	for (auto &mapping : areas.value_or(std::vector<VmContext::AreaSnapshot>{})) {
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
				if(vp.second.get() == vp.first->getOrigin().get()) {
					if(!vp.first->getParent())
						break;
					auto anchor = vp.first->getAnchor();
					assert(anchor); // Non-root mounts must have anchors in their parents.
					traversed = ViewPath{vp.first->getParent(), vp.second};
				}else{
					traversed = vp;
				}

				auto parentLink = traversed.second->getParent();
				if(!parentLink) { // We did not reach the root.
					// TODO: Can we get rid of this case?
					path = "?" + path;
					break;
				}

				path = "/" + traversed.second->getName() + path;
				vp = ViewPath{traversed.first, std::move(parentLink)};
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
	{
		std::lock_guard lock{threadGroup->credentialsMutex_};
		threadGroup->_uid = 0;
		threadGroup->_euid = 0;
		threadGroup->_gid = 0;
		threadGroup->_egid = 0;
		threadGroup->_suid = 0;
		threadGroup->_sgid = 0;
	}

	auto hierarchy = extendHierarchyForProcess(hull->getPid(), path);

	auto process = std::make_shared<Process>(threadGroup.get(), std::move(hull));
	{
		std::unique_lock lock{processGraphMutex};
		process->threadGroup()->associateProcessLocked(process);
	}
	size_t pos = path.rfind('/');
	assert(pos != std::string::npos);
	process->_path = path;
	process->_name = path.substr(pos + 1);
	process->_vmContext = VmContext::create(std::move(hierarchy));
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();
	process->threadGroup()->_signalContext = SignalContext::create();

	{
		std::unique_lock lock{processGraphMutex};
		TerminalSession::initializeNewSessionLocked(threadGroup.get());
	}

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};
	new (process->_threadPageMapping.get()) posix::ThreadPage{};

	// The initial signal mask allows all signals.
	process->setSignalMask(0);

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helTransferDescriptor(
		process->_vmContext->getHierarchy().getHandle(),
		process->_fileContext->getUniverse().getHandle(),
		kHelTransferDescriptorOut,
		kHelRightDerive | kHelRightProvision,
		kHelRightDerive | kHelRightProvision,
		&process->_clientHierarchyHandle
	));

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&process->_clientThreadPage)));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
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

	auto procfs_root = smarter::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->procfsTaskLink_ = procfs_root->createProcTaskDirectory(getProcfs().get(), process.get());

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return threadGroup;
}

bool Process::publishChild_(Process *original, std::shared_ptr<Process> process,
		std::shared_ptr<ThreadGroup> newGroup, bool cloneParent) {
	auto originalGroup = original->threadGroup();
	std::unique_lock lock{processGraphMutex};
	// Refuse to add threads or children to a group that is exiting: terminateGroup() would
	// miss the thread, or it might already have reparented the group's children.
	if (!std::holds_alternative<std::monostate>(originalGroup->_state))
		return false;

	if (newGroup) {
		auto parent = cloneParent ? originalGroup->parent_ : originalGroup;
		newGroup->parent_ = parent;
		parent->_children.push_back(newGroup);
		originalGroup->pgPointer_->reassociateProcessLocked(newGroup.get());
		newGroup->getHull()->initializeThreadGroup(newGroup.get());
	}
	process->threadGroup()->associateProcessLocked(std::move(process));
	return true;
}

async::result<std::expected<std::shared_ptr<Process>, Error>>
Process::fork(std::shared_ptr<Process> original) {
	auto hull = std::make_shared<PidHull>(nextPid++);
	auto threadGroup = ThreadGroup::create(hull);
	auto hierarchy = extendHierarchyForProcess(hull->getPid(), original->path());
	auto process = std::make_shared<Process>(threadGroup.get(), std::move(hull));
	process->_path = original->path();
	process->_name = original->name();
	process->_vmContext = co_await VmContext::clone(std::move(hierarchy), original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);
	process->threadGroup()->_signalContext = SignalContext::clone(original->threadGroup()->_signalContext);

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};
	new (process->_threadPageMapping.get()) posix::ThreadPage{};

	// Signal masks are copied on fork().
	process->setSignalMask(original->signalMask());

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helTransferDescriptor(
		process->_vmContext->getHierarchy().getHandle(),
		process->_fileContext->getUniverse().getHandle(),
		kHelTransferDescriptorOut,
		kHelRightDerive | kHelRightProvision,
		kHelRightDerive | kHelRightProvision,
		&process->_clientHierarchyHandle
	));

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&process->_clientThreadPage)));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->_clientAuxBegin = original->_clientAuxBegin;
	process->_clientAuxEnd = original->_clientAuxEnd;
	process->threadGroup()->inheritCredentials(original->threadGroup());
	process->threadGroup()->didExecute_ = false;

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->vmContext()->getHierarchy().getHandle(),
			process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(),
			nullptr, nullptr, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);
	HEL_CHECK(helGetCredentials(process->_threadDescriptor.getHandle(), 0, process->credentials_.data()));

	process->_currentGeneration = std::make_shared<Generation>();

	// The thread exists before it is published, such that terminateGroup() can interrupt it.
	if(!publishChild_(original.get(), process, threadGroup, false))
		co_return std::unexpected{Error::interrupted};

	auto procfs_root = smarter::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->procfsTaskLink_ = procfs_root->createProcTaskDirectory(getProcfs().get(), process.get());

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

	// The signal handlers belong to the thread group, like on Linux.
	if ((args->flags & CLONE_THREAD) && !(args->flags & CLONE_SIGHAND)) {
		co_return std::unexpected{Error::illegalArguments};
	}

	ThreadGroup *threadGroup;
	std::shared_ptr<ThreadGroup> newGroup;

	if (args->flags & CLONE_THREAD) {
		threadGroup = original->tgPointer_.get();
	} else {
		auto pidHull = std::make_shared<PidHull>(nextPid++);
		newGroup = ThreadGroup::create(std::move(pidHull));
		threadGroup = newGroup.get();
	}

	auto tidHull = std::make_shared<PidHull>(nextPid++);
	auto process = std::make_shared<Process>(threadGroup, std::move(tidHull));
	process->_path = original->path();
	process->_name = original->name();

	if (args->flags & CLONE_VM) {
		process->_vmContext = original->_vmContext;
	} else {
		auto hierarchy = extendHierarchyForProcess(process->pid(), process->_path);
		process->_vmContext = co_await VmContext::clone(std::move(hierarchy), original->_vmContext);
	}

	if (args->flags & CLONE_FS)
		process->_fsContext = original->_fsContext;
	else
		process->_fsContext = FsContext::clone(original->_fsContext);

	if (args->flags & CLONE_FILES)
		process->_fileContext = original->_fileContext;
	else
		process->_fileContext = FileContext::clone(original->_fileContext);

	// Under CLONE_THREAD, the group is already published; only a new group is initialized here.
	if (!(args->flags & CLONE_THREAD)) {
		if (args->flags & CLONE_SIGHAND) {
			process->threadGroup()->_signalContext = original->threadGroup()->_signalContext;
		} else {
			process->threadGroup()->_signalContext = SignalContext::clone(original->threadGroup()->_signalContext);
			if (args->flags & CLONE_CLEAR_SIGHAND)
				process->threadGroup()->_signalContext->resetHandlers();
		}
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
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};
	new (process->_threadPageMapping.get()) posix::ThreadPage{};

	// Signal masks are copied on clone().
	process->setSignalMask(original->signalMask());

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
		kHelRightInvoke,
		kHelRightInvoke,
	    &process->_clientPosixLane
	));
	client_lane.release();

	// The hierarchy is owned by the VmContext but the universe is owned by the FileContext.
	// Share the hierarchy handle only if both contexts are shared.
	if ((args->flags & CLONE_VM) && (args->flags & CLONE_FILES)) {
		process->_clientHierarchyHandle = original->_clientHierarchyHandle;
	} else {
		HEL_CHECK(helTransferDescriptor(
			process->_vmContext->getHierarchy().getHandle(),
			process->_fileContext->getUniverse().getHandle(),
			kHelTransferDescriptorOut,
			kHelRightDerive | kHelRightProvision,
			kHelRightDerive | kHelRightProvision,
			&process->_clientHierarchyHandle
		));
	}

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			reinterpret_cast<void **>(&process->_clientThreadPage)));

	process->_clientFileTable = original->_clientFileTable;
	process->_clientClkTrackerPage = original->_clientClkTrackerPage;

	process->_clientAuxBegin = original->_clientAuxBegin;
	process->_clientAuxEnd = original->_clientAuxEnd;

	if (!(args->flags & CLONE_THREAD)) {
		process->threadGroup()->inheritCredentials(original->threadGroup());
		process->threadGroup()->didExecute_ = false;
	}

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->vmContext()->getHierarchy().getHandle(),
			process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(),
			ip, sp, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};

#if defined(__riscv) && __riscv_xlen == 64
	// gp points to the executable's small data area and is only ever computed by crt0.
	// New threads start out with all registers but ip and sp cleared, hence it is inherited here.
	uintptr_t originalGprs[kHelNumGprs];
	HEL_CHECK(helLoadRegisters(original->threadDescriptor().getHandle(),
			kHelRegsGeneral, &originalGprs));
	uintptr_t gprs[kHelNumGprs] = {};
	gprs[kHelRegGp] = originalGprs[kHelRegGp];
	HEL_CHECK(helStoreRegisters(new_thread, kHelRegsGeneral, &gprs));
#endif

	process->_posixLane = std::move(server_lane);
	HEL_CHECK(helGetCredentials(process->_threadDescriptor.getHandle(), 0, process->credentials_.data()));

	process->_currentGeneration = std::make_shared<Generation>();

	// The thread exists before it is published, such that terminateGroup() can interrupt it.
	if(!publishChild_(original.get(), process, newGroup, args->flags & CLONE_PARENT))
		co_return std::unexpected{Error::interrupted};
	process->getTidHull()->initializeProcess(process.get());

	auto procfs_root = smarter::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->procfsTaskLink_ = procfs_root->createProcTaskDirectory(getProcfs().get(), process.get());

	co_return process;
}

async::result<Error> Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env) {
	auto exec_hierarchy = extendHierarchyForProcess(process->pid(), path);
	auto exec_vm_context = VmContext::create(std::move(exec_hierarchy));

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
		kHelRightInvoke,
		kHelRightInvoke,
	    &exec_posix_lane
	));
	client_lane.release();

	HelHandle exec_client_hierarchy;
	HEL_CHECK(helTransferDescriptor(
		exec_vm_context->getHierarchy().getHandle(),
		process->_fileContext->getUniverse().getHandle(),
		kHelTransferDescriptorOut,
		kHelRightDerive | kHelRightProvision,
		kHelRightDerive | kHelRightProvision,
		&exec_client_hierarchy
	));

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
			nullptr, 0, 0x1000, kHelMapProtRead,
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
	{
		std::lock_guard lock{process->taskMutex_};
		process->_name = path.substr(pos + 1);
		process->_path = std::move(path);
	}
	process->_posixLane = std::move(server_lane);
	{
		std::lock_guard lock{process->taskMutex_};
		process->_threadDescriptor = std::move(execResult.thread);
		std::swap(process->_vmContext, exec_vm_context);
	}
	exec_vm_context = nullptr;
	process->threadGroup()->_signalContext->resetHandlers();
	process->setAltStackEnabled(false);
	process->setAltStackSp(0, 0);

	// Handle setuid and friends
	process->threadGroup()->setExecCredentials(execResult.effectiveUid, execResult.effectiveGid,
			execResult.savedUid, execResult.savedGid);

	process->_clientThreadPage = exec_thread_page;
	process->_clientPosixLane = exec_posix_lane;
	process->_clientHierarchyHandle = exec_client_hierarchy;
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
	uint64_t groupUserTime;
	{
		std::lock_guard lock{threadGroup()->usageMutex_};
		threadGroup()->_generationUsage.userTime += stats.userTime;
		groupUserTime = threadGroup()->_generationUsage.userTime;
	}

	_posixLane = {};
	// Dropping the contexts closes files, hence do it after releasing the lock.
	helix::UniqueDescriptor threadDescriptor;
	std::shared_ptr<VmContext> vmContext;
	std::shared_ptr<FsContext> fsContext;
	std::shared_ptr<FileContext> fileContext;
	{
		std::lock_guard lock{taskMutex_};
		threadDescriptor = std::move(_threadDescriptor);
		vmContext = std::move(_vmContext);
		fsContext = std::move(_fsContext);
		fileContext = std::move(_fileContext);
	}
	threadDescriptor = {};
	vmContext = nullptr;
	fsContext = nullptr;
	fileContext = nullptr;
	//_signalContext = nullptr; // TODO: Migrate the notifications to PID 1.
	_currentGeneration = nullptr;

	{
		std::lock_guard lock{parent->usageMutex_};
		parent->_childrenUsage.userTime += groupUserTime;
	}

	bool isLeader;
	bool isLast;
	{
		std::unique_lock lock{processGraphMutex};
		std::erase_if(tgPointer_->threads_, [&](auto e) {
			return e.get() == this;
		});
		isLeader = tgPointer_->leader_.get() == this;
		isLast = tgPointer_->threads_.empty();
	}
	// The leader's /proc/[pid]/task/[tid] stays until the group is reaped, like a zombie.
	if (!isLeader)
		unlinkProcfsTask_();
	if (lastInGroup)
		*lastInGroup = isLast;
	tgPointer_->processTerminationSeq_.fetch_add(1, std::memory_order_release);
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
	// Called with processGraphMutex held.
	auto notificationMatch = [&](ThreadGroup *tg) -> bool {
		if (pid > 0) {
			// match a PID
			return pid == tg->pid();
		} else if (pid == 0) {
			// match any child process in the same process group
			return threadGroup()->pgPointer_ == tg->pgPointer_;
		} else if (pid == -1) {
			// match any child process
			return true;
		} else {
			// match any child process with the PGID of abs(pid)
			return abs(pid) == tg->pgPointer_->getHull()->getPid();
		}
	};

	// Check if there are still children or notifications left that match the requested set.
	// Called with processGraphMutex held.
	auto hasMatchingChildren = [&]() -> bool {
		if (pid < -1 || pid == 0) {
			int pgid = (pid == 0) ? threadGroup()->pgPointer_->getHull()->getPid() : abs(pid);

			// check if there are this Process' children left in the process group `pgid`
			auto matchChildren =
			    std::ranges::any_of(threadGroup()->_children, [pgid](const auto &c) {
				    return c->pgPointer_ && c->pgPointer_->getHull()->getPid() == pgid;
			    });

			// check if there are notifications queued for children that are members of
			// the process group `pgid`
			auto matchNotifications =
			    std::ranges::any_of(threadGroup()->_notifyQueue, [pgid](const auto &c) {
				    return c->pgPointer_ && c->pgPointer_->getHull()->getPid() == pgid;
			    });

			return matchChildren || matchNotifications;
		} else {
			return !threadGroup()->_children.empty() || !threadGroup()->_notifyQueue.empty();
		}
	};

	while(true) {
		auto seq = threadGroup()->notifySeq_.load(std::memory_order_acquire);
		// Checked after reading the sequence number, such that a concurrent reap wakes us up.
		bool noChildren;
		{
			std::shared_lock lock{processGraphMutex};
			noChildren = !hasMatchingChildren() || (pid > 0 && !hasChildLocked(pid));
		}
		if(noChildren)
			co_return Error::noChildProcesses;

		std::optional<WaitResult> result{};
		std::optional<ThreadGroup::Retired> retired;
		{
			std::unique_lock lock{processGraphMutex};
			auto &queue = threadGroup()->_notifyQueue;
			for(auto it = queue.begin(); it != queue.end(); ++it) {
				if(!notificationMatch(*it))
					continue;
				if(std::holds_alternative<TerminationByExit>((*it)->_state) && !(flags & (waitExited)))
					continue;
				if(std::holds_alternative<TerminationBySignal>((*it)->_state) && !(flags & (waitExited)))
					continue;

				result = WaitResult{
					.pid = (*it)->pid(),
					.uid = (*it)->uid(),
					.state = (*it)->_state,
					.stats = (*it)->selfUsage(),
				};

				if(!(flags & waitLeaveZombie)) {
					// erasing from the queue invalidates the iterator, so we take a pointer before
					auto tg = *it;
					queue.erase(it);
					retired = ThreadGroup::retireLocked_(tg);
				}

				break;
			}
		}
		if(retired)
			ThreadGroup::finishRetire_(std::move(*retired));

		if(result)
			co_return result.value();
		else if(flags & waitNonBlocking)
			co_return Error::wouldBlock;

		auto waitResult = co_await threadGroup()->_notifyBell.async_wait_if([&] {
			return threadGroup()->notifySeq_.load(std::memory_order_acquire) == seq;
		}, ct);
		if (!waitResult)
			co_return Error::interrupted;
	}
}

bool Process::hasChildLocked(int pid) {
	return std::ranges::any_of(threadGroup()->_children, [pid](auto e) {
		return e->pid() == pid;
	});
}

async::result<bool> ThreadGroup::awaitNotifyTypeChange(async::cancellation_token token) {
	auto result = co_await notifyTypeChange_.async_wait_if([&] {
		return notifyType_.load() == NotifyType::null;
	}, token);
	co_return bool(result);
}

ThreadGroup::ThreadGroup(std::shared_ptr<PidHull> hull, ThreadGroup *parent)
: parent_{parent}, hull_{std::move(hull)} {

}

// ThreadGroups are unlinked from the process graph by retireLocked_(), such that the last
// reference can be dropped on any thread.
ThreadGroup::~ThreadGroup() {
	assert(!pgPointer_);
}

std::shared_ptr<ThreadGroup> ThreadGroup::init(std::shared_ptr<PidHull> hull) {
	initThreadGroup = std::make_shared<ThreadGroup>(std::move(hull), nullptr);
	return initThreadGroup;
}

std::shared_ptr<ThreadGroup> ThreadGroup::create(std::shared_ptr<PidHull> hull) {
	return std::make_shared<ThreadGroup>(std::move(hull), nullptr);
}

void ThreadGroup::inheritCredentials(ThreadGroup *other) {
	uid_t uid, euid, suid;
	gid_t gid, egid, sgid;
	{
		std::lock_guard lock{other->credentialsMutex_};
		uid = other->_uid;
		euid = other->_euid;
		suid = other->_suid;
		gid = other->_gid;
		egid = other->_egid;
		sgid = other->_sgid;
	}
	std::lock_guard lock{credentialsMutex_};
	_uid = uid;
	_euid = euid;
	_suid = suid;
	_gid = gid;
	_egid = egid;
	_sgid = sgid;
}

void ThreadGroup::setExecCredentials(uid_t euid, gid_t egid, uid_t suid, gid_t sgid) {
	std::lock_guard lock{credentialsMutex_};
	_euid = euid;
	_egid = egid;
	_suid = suid;
	_sgid = sgid;
}

Error ThreadGroup::setUid(uid_t uid) {
	if(uid < 0)
		return Error::illegalArguments;

	std::lock_guard lock{credentialsMutex_};
	if(isRootLocked_()) {
		_uid = uid;
		_euid = uid;
		_suid = uid;
		return Error::success;
	}
	if(uid == _uid || uid == _suid) {
		_euid = uid;
		return Error::success;
	}
	return Error::accessDenied;
}

Error ThreadGroup::setGid(gid_t gid) {
	if(gid < 0)
		return Error::illegalArguments;

	std::lock_guard lock{credentialsMutex_};
	if(isRootLocked_()) {
		_gid = gid;
		_egid = gid;
		_sgid = gid;
		return Error::success;
	}
	if(gid == _gid || gid == _sgid) {
		_egid = gid;
		return Error::success;
	}
	return Error::accessDenied;
}

Error ThreadGroup::setResuid(uint64_t ruid, uint64_t euid, uint64_t suid) {
	std::lock_guard lock{credentialsMutex_};
	const auto oldUid = _uid;
	const auto oldEuid = _euid;
	const auto oldSuid = _suid;

	uid_t newUid = oldUid;
	uid_t newEuid = oldEuid;
	uid_t newSuid = oldSuid;

	if(auto error = resolveSetIdRequest(ruid, oldUid, newUid); error != Error::success)
		return error;
	if(auto error = resolveSetIdRequest(euid, oldEuid, newEuid); error != Error::success)
		return error;
	if(auto error = resolveSetIdRequest(suid, oldSuid, newSuid); error != Error::success)
		return error;

	if(!isRootLocked_()) {
		auto permitted = [&](uid_t id) {
			return id == oldUid || id == oldEuid || id == oldSuid;
		};
		if((!isNoChangeSetIdRequest<uid_t>(ruid) && !permitted(newUid))
				|| (!isNoChangeSetIdRequest<uid_t>(euid) && !permitted(newEuid))
				|| (!isNoChangeSetIdRequest<uid_t>(suid) && !permitted(newSuid)))
			return Error::insufficientPermissions;
	}

	_uid = newUid;
	_euid = newEuid;
	_suid = newSuid;
	return Error::success;
}

Error ThreadGroup::setResgid(uint64_t rgid, uint64_t egid, uint64_t sgid) {
	std::lock_guard lock{credentialsMutex_};
	const auto oldGid = _gid;
	const auto oldEgid = _egid;
	const auto oldSgid = _sgid;

	gid_t newGid = oldGid;
	gid_t newEgid = oldEgid;
	gid_t newSgid = oldSgid;

	if(auto error = resolveSetIdRequest(rgid, oldGid, newGid); error != Error::success)
		return error;
	if(auto error = resolveSetIdRequest(egid, oldEgid, newEgid); error != Error::success)
		return error;
	if(auto error = resolveSetIdRequest(sgid, oldSgid, newSgid); error != Error::success)
		return error;

	if(!isRootLocked_()) {
		auto permitted = [&](gid_t id) {
			return id == oldGid || id == oldEgid || id == oldSgid;
		};
		if((!isNoChangeSetIdRequest<gid_t>(rgid) && !permitted(newGid))
				|| (!isNoChangeSetIdRequest<gid_t>(egid) && !permitted(newEgid))
				|| (!isNoChangeSetIdRequest<gid_t>(sgid) && !permitted(newSgid)))
			return Error::insufficientPermissions;
	}

	_gid = newGid;
	_egid = newEgid;
	_sgid = newSgid;
	return Error::success;
}

Error ThreadGroup::setReuid(uint64_t ruid, uint64_t euid) {
	std::lock_guard lock{credentialsMutex_};
	const auto oldUid = _uid;
	const auto oldEuid = _euid;
	const auto oldSuid = _suid;
	const bool changeUid = !isNoChangeSetIdRequest<uid_t>(ruid);
	const bool changeEuid = !isNoChangeSetIdRequest<uid_t>(euid);

	uid_t newUid = oldUid;
	uid_t newEuid = oldEuid;
	if(auto error = resolveSetIdRequest(ruid, oldUid, newUid); error != Error::success)
		return error;
	if(auto error = resolveSetIdRequest(euid, oldEuid, newEuid); error != Error::success)
		return error;

	if(!isRootLocked_()) {
		// Linux permits an unprivileged real-ID change to the old real or
		// effective ID; POSIX leaves this case unspecified.
		if(changeUid && newUid != oldUid && newUid != oldEuid)
			return Error::insufficientPermissions;
		if(changeEuid && newEuid != oldUid && newEuid != oldEuid
				&& newEuid != oldSuid)
			return Error::insufficientPermissions;
	}

	uid_t newSuid = oldSuid;
	if(changeUid || (changeEuid && newEuid != oldUid))
		newSuid = newEuid;

	_uid = newUid;
	_euid = newEuid;
	_suid = newSuid;
	return Error::success;
}

Error ThreadGroup::setRegid(uint64_t rgid, uint64_t egid) {
	std::lock_guard lock{credentialsMutex_};
	const auto oldGid = _gid;
	const auto oldEgid = _egid;
	const auto oldSgid = _sgid;
	const bool changeGid = !isNoChangeSetIdRequest<gid_t>(rgid);
	const bool changeEgid = !isNoChangeSetIdRequest<gid_t>(egid);

	gid_t newGid = oldGid;
	gid_t newEgid = oldEgid;
	if(auto error = resolveSetIdRequest(rgid, oldGid, newGid); error != Error::success)
		return error;
	if(auto error = resolveSetIdRequest(egid, oldEgid, newEgid); error != Error::success)
		return error;

	if(!isRootLocked_()) {
		// Linux permits the same real-ID choices as setreuid; POSIX leaves
		// this real-ID case unspecified.
		if(changeGid && newGid != oldGid && newGid != oldEgid)
			return Error::insufficientPermissions;
		if(changeEgid && newEgid != oldGid && newEgid != oldEgid
				&& newEgid != oldSgid)
			return Error::insufficientPermissions;
	}

	gid_t newSgid = oldSgid;
	if(changeGid || (changeEgid && newEgid != oldGid))
		newSgid = newEgid;

	_gid = newGid;
	_egid = newEgid;
	_sgid = newSgid;
	return Error::success;
}

async::result<void> ThreadGroup::terminateGroup(TerminationState state) {
	bool firstCall;
	{
		std::unique_lock lock{processGraphMutex};
		// Only the first terminate() call goes through.
		firstCall = std::holds_alternative<std::monostate>(_state);
		if (firstCall)
			_state = std::move(state);
	}
	if (!firstCall)
		co_return;

	// Cause all remaining processes to terminate and wait for them.
	// Note that the process that caused the thread group termination has already called terminate() by now,
	// so we do not need to worry about that process deadlocking if it waits for terminateGroup().
	// No threads are added from now on since fork() and clone() check _state.
	while (true) {
		auto seq = processTerminationSeq_.load(std::memory_order_acquire);
		auto remaining = threads();
		for (auto &process : remaining) {
			if (process->forceTermination.exchange(true))
				continue;
			process->interruptThread();
		}

		if (remaining.empty())
			break;
		co_await processTerminationEvent_.async_wait_if([&] {
			return processTerminationSeq_.load(std::memory_order_acquire) == seq;
		});
	}

	std::shared_ptr<IntervalTimer> timer;
	{
		std::lock_guard lock{timerMutex};
		timer = realTimer;
	}
	if(timer)
		timer->cancel();

	// Signals and wake-ups are recorded under processGraphMutex and delivered after releasing it.
	std::vector<std::pair<std::shared_ptr<ThreadGroup>, int>> deathSignals;
	std::shared_ptr<ThreadGroup> reparentTo;
	bool ringReparent = false;
	std::shared_ptr<ThreadGroup> parent;
	std::optional<Retired> retired;
	{
		std::unique_lock lock{processGraphMutex};

		if (parent_) {
			// Walk up the chain until we hit a process that has no parent.
			auto reparentTarget = parent_;
			while(reparentTarget->parent_)
				reparentTarget = reparentTarget->parent_;
			reparentTo = reparentTarget->shared_from_this();

			for(auto it = _children.begin(); it != _children.end();) {
				(*it)->parent_ = reparentTarget;
				reparentTarget->_children.push_back((*it));

				// send the signal if it requested one on parent death
				if(int sig = (*it)->parentDeathSignal_; sig)
					deathSignals.push_back({*it, sig});

				it = _children.erase(it);
			}

			auto reparentSigchldHandling = reparentTarget->signalContext()->getHandler(SIGCHLD);
			while (!_notifyQueue.empty()) {
				auto child = _notifyQueue.pop_front();
				ringReparent = true;
				if (reparentSigchldHandling.disposition != SignalDisposition::ignore && !(reparentSigchldHandling.flags & signalNoChildWait))
					reparentTarget->_notifyQueue.push_back(child);
			}

			// Notify the parent of our status change.
			// We need to do that even when not sending SIGCHLD since it wakes up pidfd.
			assert(notifyType_ == NotifyType::null);
			notifyType_ = NotifyType::terminated;

			parent = parent_->shared_from_this();
			auto sigchldHandling = parent_->signalContext()->getHandler(SIGCHLD);
			if (sigchldHandling.disposition != SignalDisposition::ignore && !(sigchldHandling.flags & signalNoChildWait)) {
				parent_->_notifyQueue.push_back(this);

				// Queue SIGCHLD before the parent can reap us; the bell is raised after unlocking.
				ChildSignal info;
				info.pid = hull_->getPid();
				info.utime = selfUsage().userTime;

				if(std::get_if<TerminationByExit>(&_state)) {
					info.status = std::get<TerminationByExit>(_state).code;
					info.code = CLD_EXITED;
				} else if(std::get_if<TerminationBySignal>(&_state)) {
					info.status = std::get<TerminationBySignal>(_state).signo;
					info.code = dumpable_ ? CLD_DUMPED : CLD_KILLED;
				} else {
					std::println("posix: unhandled SIGCHLD reason");
				}

				std::lock_guard signalLock{parent_->signalMutex_};
				parent_->signalQueue_.issueSignal(SIGCHLD, info, ++parent_->currentSignalSeq_);
			} else {
				retired = retireLocked_(this);
			}
		}
	}

	if (!parent) {
		std::println("posix: terminateGroup() called on init process");
		co_return;
	}

	for (auto &[child, sig] : deathSignals) {
		UserSignal info;
		info.pid = hull_->getPid();
		child->issueThreadGroupSignal(sig, info);
	}

	if(ringReparent)
		reparentTo->raiseNotifyBell();

	notifyTypeChange_.raise();

	if (!retired) {
		parent->raiseNotifyBell();
		parent->signalQueue_.signalBell_.raise();
	} else {
		finishRetire_(std::move(*retired));
	}
}

ThreadGroup::Retired ThreadGroup::retireLocked_(ThreadGroup *tg) {
	Retired retired;
	auto parent = tg->parent_;

	// The parent's _children holds the last graph reference; it is dropped by the caller.
	auto it = std::ranges::find(parent->_children, tg, &std::shared_ptr<ThreadGroup>::get);
	assert(it != parent->_children.end());
	retired.group = std::move(*it);
	parent->_children.erase(it);
	retired.parent = parent->shared_from_this();
	tg->parent_ = nullptr;

	retired.leader = std::move(tg->leader_);

	if (tg->pgPointer_)
		tg->pgPointer_->dropProcessLocked(tg);

	retired.procfsLink = std::move(tg->procfsLink_);
	return retired;
}

void ThreadGroup::finishRetire_(Retired retired) {
	if (retired.leader)
		retired.leader->unlinkProcfsTask_();
	if (retired.procfsLink)
		retired.procfsLink->unlinkSelf();

	// We need to ring _notifyBell once the process is retired
	// since waitpid() returns an error once no children are left.
	retired.parent->raiseNotifyBell();
}

void ThreadGroup::associateProcessLocked(std::shared_ptr<Process> process) {
	assert(process->tgPointer_.get() == this);

	threads_.push_back(process);

	if (!leader_)
		leader_ = process;
}

std::shared_ptr<ThreadGroup> ThreadGroup::getParent() {
	std::shared_lock lock{processGraphMutex};
	if (!parent_)
		return nullptr;
	return parent_->shared_from_this();
}

std::shared_ptr<ProcessGroup> ThreadGroup::pgPointer() {
	std::shared_lock lock{processGraphMutex};
	return pgPointer_;
}

std::vector<std::shared_ptr<Process>> ThreadGroup::threads() {
	std::shared_lock lock{processGraphMutex};
	return threads_;
}

Error ThreadGroup::setProcessGroupOf(pid_t pid, pid_t pgid) {
	// POSIX: reject negative `pgid` (or implementation-unsupported) values with EINVAL
	if (pgid < 0)
		return Error::illegalArguments;

	std::shared_ptr<ThreadGroup> target;
	std::shared_ptr<ThreadGroup> targetParent;
	{
		std::unique_lock lock{processGraphMutex};
		if (pid > 0) {
			target = findThreadGroup(pid);
			if (!target || !target->pgPointer_)
				return Error::noSuchProcess;

			auto isSelf = pid == this->pid();
			auto isChild = !isSelf && target->parent_ == this;

			// POSIX: if `pid` is not the PID of the calling process or its children, ESRCH.
			if (!isSelf && !isChild)
				return Error::noSuchProcess;

			// POSIX: if target process is not in the same session, EPERM.
			if (target->pgPointer_->getSession() != pgPointer_->getSession())
				return Error::insufficientPermissions;

			// POSIX: if `pid` matches the process ID of a child and the child has successfully
			// executed one of the `exec*` functions, return EACCES.
			if (isChild && target->didExecute())
				return Error::accessDenied;
		} else {
			target = shared_from_this();
		}

		auto session = target->pgPointer_->getSession();

		// POSIX: We can't change the process group ID of the session leader, EPERM
		if (target->pid() == session->getSessionId())
			return Error::insufficientPermissions;

		// If pgid is 0, we're going to set it to the calling process's pid, use the target group.
		auto resolvedPgid = pgid ? pgid : target->pid();
		auto group = session->getProcessGroupByIdLocked(resolvedPgid);

		if (group) {
			// Found, do permission checking and join
			group->reassociateProcessLocked(target.get());
		} else {
			// Not found, making it if pgid and pid match, or if pgid is 0, indicating that we should make one
			if (target->pid() == pgid || !pgid) {
				session->spawnProcessGroupLocked(target.get());
			} else {
				// POSIX: invalid `pgid` supplied, return EINVAL.
				return Error::illegalArguments;
			}
		}

		if (target->parent_)
			targetParent = target->parent_->shared_from_this();
	}

	// Wake any waiters (e.g. waitpid); this is necessary as that may need to return ECHLD if we
	// moved out the last child.
	if (targetParent)
		targetParent->raiseNotifyBell();
	return Error::success;
}

std::expected<pid_t, Error> ThreadGroup::createSession() {
	pid_t sid;
	std::shared_ptr<ThreadGroup> parent;
	{
		std::unique_lock lock{processGraphMutex};

		// POSIX: if the calling process is already a group leader, EPERM.
		if (pgPointer_->getSession()->getSessionId() == pid())
			return std::unexpected{Error::insufficientPermissions};

		auto session = TerminalSession::initializeNewSessionLocked(this);
		sid = session->getSessionId();

		if (parent_)
			parent = parent_->shared_from_this();
	}

	// Wake any waiters (e.g. waitpid); this is necessary as that may need to return ECHLD if we
	// moved out the last child.
	if (parent)
		parent->raiseNotifyBell();
	return sid;
}


std::shared_ptr<Process> ThreadGroup::findThread(pid_t tid) {
	std::shared_lock lock{processGraphMutex};
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
	std::lock_guard lock{pidMapMutex};
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->processGroup_.lock();
}

ProcessGroup::ProcessGroup(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

// Groups are unlinked from their session once their last member leaves, such that the last
// reference can be dropped on any thread.
ProcessGroup::~ProcessGroup() {
	assert(!sessionHook_.in_list);
}

void ProcessGroup::reassociateProcessLocked(ThreadGroup *process) {
	assert(sessionHook_.in_list);
	if(process->pgPointer_.get() == this)
		return;
	if(process->pgPointer_) {
		auto oldGroup = process->pgPointer_.get();
		oldGroup->members_.erase(oldGroup->members_.iterator_to(process));
		if(oldGroup->members_.empty())
			oldGroup->sessionPointer_->dropGroupLocked(oldGroup);
	}
	process->pgPointer_ = shared_from_this();
	members_.push_back(process);
}

void ProcessGroup::dropProcessLocked(ThreadGroup *process) {
	assert(process->pgPointer_.get() == this);
	members_.erase(members_.iterator_to(process));
	if(members_.empty())
		sessionPointer_->dropGroupLocked(this);
	// Note: this assignment can destruct 'this'.
	process->pgPointer_ = nullptr;
}

void ProcessGroup::collectMembersLocked_(std::vector<std::shared_ptr<ThreadGroup>> &members) {
	for(auto processRef : members_) {
		if(auto member = processRef->weak_from_this().lock(); member)
			members.push_back(std::move(member));
	}
}

void ProcessGroup::issueSignalToGroup(int sn, SignalInfo info) {
	std::vector<std::shared_ptr<ThreadGroup>> members;
	{
		std::shared_lock lock{processGraphMutex};
		collectMembersLocked_(members);
	}
	for(auto &member : members)
		member->issueThreadGroupSignal(sn, info);
}

bool ProcessGroup::isOrphanedLocked() {
	for (auto processRef : members_) {
		// If a member process has a parent that
		// a) is in the same session as the ProcessGroup, and
		// b) is not a member of the process group,
		// then the ProcessGroup is NOT orphaned
		if (processRef->parent_
		    && processRef->parent_->pgPointer_->getSession() == getSession()
		    && processRef->pgPointer_ != getHull()->getProcessGroup())
			return false;
	}

	// If no parent-child relationships (yuck) can connect this group to the rest of the
	// session, we are orphaned.
	return true;
}

TerminalSession::TerminalSession(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

// Sessions drop their controlling terminal once their last group is unlinked, such that the
// last reference can be dropped on any thread.
TerminalSession::~TerminalSession() {
	assert(!ctsPointer_);
}

pid_t TerminalSession::getSessionId() {
	return hull_->getPid();
}

std::shared_ptr<TerminalSession> TerminalSession::initializeNewSessionLocked(ThreadGroup *sessionLeader) {
	auto session = std::make_shared<TerminalSession>(sessionLeader->getHull()->shared_from_this());
	auto group = session->spawnProcessGroupLocked(sessionLeader);
	session->foregroundGroup_ = group.get();
	session->hull_->initializeTerminalSession(session.get());
	return session;
}

std::shared_ptr<ProcessGroup> TerminalSession::spawnProcessGroupLocked(ThreadGroup *groupLeader) {
	auto group = std::make_shared<ProcessGroup>(groupLeader->getHull()->shared_from_this());
	group->sessionPointer_ = shared_from_this();
	groups_.push_back(group.get());
	group->reassociateProcessLocked(groupLeader);
	group->hull_->initializeProcessGroup(group.get());
	return group;
}

std::shared_ptr<ProcessGroup> TerminalSession::getProcessGroupByIdLocked(pid_t id) {
	for(auto i : groups_) {
		if(i->getHull()->getPid() == id)
			return i->shared_from_this();
	}
	return nullptr;
}

void TerminalSession::dropGroupLocked(ProcessGroup *group) {
	assert(group->sessionPointer_.get() == this);
	if(foregroundGroup_ == group)
		foregroundGroup_ = nullptr;
	groups_.erase(groups_.iterator_to(group));
	if(groups_.empty() && ctsPointer_)
		ctsPointer_->dropSessionLocked(this);
}

Error TerminalSession::setForegroundGroupLocked(ProcessGroup *group) {
	assert(group);
	if(group->sessionPointer_.get() != this)
		return Error::insufficientPermissions;
	foregroundGroup_ = group;
	return Error::success;
}

bool TerminalSession::hasControllingTerminal() {
	std::shared_lock lock{processGraphMutex};
	return ctsPointer_;
}

TerminalSession *ControllingTerminalState::sessionOfLocked_(Process *process) {
	auto group = process->threadGroup()->pgPointer_.get();
	if(!group)
		return nullptr;
	auto session = group->sessionPointer_.get();
	if(session->ctsPointer_ != this)
		return nullptr;
	return session;
}

Error ControllingTerminalState::assignSessionOf(Process *process, bool makeForeground) {
	std::unique_lock lock{processGraphMutex};
	auto group = process->threadGroup()->pgPointer_.get();
	if(!group)
		return Error::noSuchProcess;
	auto session = group->sessionPointer_.get();
	if(process->getPidHull() != session->hull_.get())
		return Error::illegalArguments; // Process is not a session leader.
	if(associatedSession_ != session) {
		if(associatedSession_)
			return Error::insufficientPermissions;
		if(session->ctsPointer_)
			return Error::insufficientPermissions;
		associatedSession_ = session;
		session->ctsPointer_ = this;
	}
	if(makeForeground)
		return session->setForegroundGroupLocked(group);
	return Error::success;
}

Error ControllingTerminalState::releaseSessionOf(Process *process) {
	std::vector<std::shared_ptr<ThreadGroup>> hangupMembers;
	{
		std::unique_lock lock{processGraphMutex};
		auto session = sessionOfLocked_(process);
		if(!session)
			return Error::notTerminal;

		auto group = session->foregroundGroup_;
		if(group && session->getSessionId() == process->pid())
			group->collectMembersLocked_(hangupMembers);

		dropSessionLocked(session);
	}

	for(auto &member : hangupMembers)
		member->issueThreadGroupSignal(SIGHUP, {});
	return Error::success;
}

std::expected<pid_t, Error> ControllingTerminalState::getForegroundGroupOf(Process *process) {
	std::shared_lock lock{processGraphMutex};
	auto session = sessionOfLocked_(process);
	if(!session)
		return std::unexpected{Error::notTerminal};

	auto group = session->foregroundGroup_;
	if(!group) {
		// If there is no foreground process group, tcgetpgrp() shall return
		// a value greater than 1 that does not match the process group ID of any
		// existing process group.
		return INT_MAX;
	}
	return group->getHull()->getPid();
}

Error ControllingTerminalState::setForegroundGroupOf(Process *process, pid_t pgid) {
	std::unique_lock lock{processGraphMutex};
	auto session = sessionOfLocked_(process);
	if(!session)
		return Error::notTerminal;

	auto group = session->getProcessGroupByIdLocked(pgid);
	if(!group)
		return Error::insufficientPermissions;

	auto ownGroup = process->threadGroup()->pgPointer_.get();
	auto sigttouHandler = process->threadGroup()->signalContext()->getHandler(SIGTTOU);
	if (ownGroup == session->foregroundGroup_
			|| sigttouHandler.disposition == SignalDisposition::ignore
			|| (process->signalMask() & (1 << (SIGTTOU - 1)))) {
		// POSIX: If the calling thread is blocking SIGTTOU signals or the process is
		// ignoring SIGTTOU signals, the process shall be allowed to perform
		// the operation.
		return session->setForegroundGroupLocked(group.get());
	} else if (ownGroup->isOrphanedLocked()) {
		return Error::ioError;
	}

	// TODO: if process is in a background PG, send SIGTTOU
	return Error::success;
}

std::expected<pid_t, Error> ControllingTerminalState::getSessionIdOf(Process *process) {
	std::shared_lock lock{processGraphMutex};
	auto session = sessionOfLocked_(process);
	if(!session)
		return std::unexpected{Error::notTerminal};
	return session->getSessionId();
}

void ControllingTerminalState::dropSessionLocked(TerminalSession *session) {
	assert(associatedSession_ == session);
	associatedSession_ = nullptr;
	session->ctsPointer_ = nullptr;
}

void ControllingTerminalState::issueSignalToForegroundGroup(int sn, SignalInfo info) {
	std::vector<std::shared_ptr<ThreadGroup>> members;
	{
		std::shared_lock lock{processGraphMutex};
		if(!associatedSession_)
			return;
		if(!associatedSession_->foregroundGroup_)
			return;
		associatedSession_->foregroundGroup_->collectMembersLocked_(members);
	}
	for(auto &member : members)
		member->issueThreadGroupSignal(sn, info);
}

void ControllingTerminalState::setDevice(std::weak_ptr<UnixDevice> device) {
	std::unique_lock lock{processGraphMutex};
	device_ = std::move(device);
}

std::shared_ptr<UnixDevice> ControllingTerminalState::getDeviceOf(Process *process) {
	std::shared_lock lock{processGraphMutex};
	auto group = process->threadGroup()->pgPointer_.get();
	if(!group)
		return nullptr;
	auto cts = group->sessionPointer_->ctsPointer_;
	if(!cts)
		return nullptr;
	return cts->device_.lock();
}
