
#include "kernel.hpp"
#include "module.hpp"
#include <frigg/elf.hpp>
#include <eir/interface.hpp>

namespace thor {

static constexpr bool logEveryIrq = true;
static constexpr bool logEverySyscall = false;

// TODO: get rid of the rootUniverse/initrdServer global variables.
frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;
frigg::LazyInitializer<frigg::SharedPtr<Endpoint, EndpointRwControl>> initrdServer;

frigg::LazyInitializer<frigg::Vector<Module, KernelAlloc>> allModules;

// TODO: move this declaration to a header file
void runService();

Module *getModule(frigg::StringView filename) {
	for(size_t i = 0; i < allModules->size(); i++)
		if((*allModules)[i].filename == filename)
			return &(*allModules)[i];
	return nullptr;
}

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	frigg::StringView interpreter;
};

ImageInfo loadModuleImage(frigg::SharedPtr<AddressSpace> space,
		VirtualAddr base, PhysicalAddr image_paddr) {
	ImageInfo info;

	void *image_ptr = physicalToVirtual(image_paddr);
	
	// parse the ELf file format
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	assert(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');

	info.entryIp = (void *)(base + ehdr->e_entry);
	info.phdrEntrySize = ehdr->e_phentsize;
	info.phdrCount = ehdr->e_phnum;

	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		
		if(phdr->p_type == PT_LOAD) {
			assert(phdr->p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			auto memory = frigg::makeShared<Memory>(*kernelAlloc,
					AllocatedMemory(virt_length));

			uintptr_t virt_disp = phdr->p_vaddr - virt_address;
			memory->copyFrom(virt_disp, (char *)image_ptr + phdr->p_offset, phdr->p_filesz);

			VirtualAddr actual_address;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				AddressSpace::Guard space_guard(&space->lock);
				space->map(space_guard, memory, base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadWrite,
						&actual_address);
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				AddressSpace::Guard space_guard(&space->lock);
				space->map(space_guard, memory, base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadExecute,
						&actual_address);
			}else{
				frigg::panicLogger() << "Illegal combination of segment permissions"
						<< frigg::endLog;
			}
			thorRtInvalidateSpace();
		}else if(phdr->p_type == PT_INTERP) {
			info.interpreter = frigg::StringView((char *)image_ptr + phdr->p_offset,
					phdr->p_filesz);
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_DYNAMIC
				|| phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			assert(!"Unexpected PHDR");
		}
	}

	return info;
}

template<typename T>
uintptr_t copyToStack(frigg::String<KernelAlloc> &stack_image, const T &data) {
	uintptr_t misalign = stack_image.size() % alignof(data);
	if(misalign)
		stack_image.resize(alignof(data) - misalign);
	uintptr_t offset = stack_image.size();
	stack_image.resize(stack_image.size() + sizeof(data));
	memcpy(&stack_image[offset], &data, sizeof(data));
	return offset;
}

void executeModule(PhysicalAddr image_paddr) {	
	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	space->setupDefaultMappings();

	ImageInfo exec_info = loadModuleImage(space, 0, image_paddr);

	// FIXME: use actual interpreter name here
	Module *interp_module = getModule("ld-init.so");
	assert(interp_module);
	ImageInfo interp_info = loadModuleImage(space, 0x40000000, interp_module->physical);

	// start relevant services.

	// we increment the owning reference count twice here. it is decremented
	// each time one of the EndpointRwControl references is decremented to zero.
	auto pipe = frigg::makeShared<FullPipe>(*kernelAlloc);
	pipe.control().increment();
	pipe.control().increment();
	initrdServer.initialize(frigg::adoptShared,
			&pipe->endpoint(0),
			EndpointRwControl(&pipe->endpoint(0), pipe.control().counter()));
	frigg::SharedPtr<Endpoint, EndpointRwControl> initrd_client(frigg::adoptShared,
			&pipe->endpoint(1),
			EndpointRwControl(&pipe->endpoint(1), pipe.control().counter()));

	Handle initrd_handle;
	{
		Universe::Guard lock(&(*rootUniverse)->lock);
		initrd_handle = (*rootUniverse)->attachDescriptor(lock,
				EndpointDescriptor(frigg::move(initrd_client)));
	}

	runService();

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = frigg::makeShared<Memory>(*kernelAlloc,
			AllocatedMemory(stack_size));

	VirtualAddr stack_base;
	{
		AddressSpace::Guard space_guard(&space->lock);
		space->map(space_guard, stack_memory, 0, 0, stack_size,
				AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite, &stack_base);
	}
	thorRtInvalidateSpace();

	// build the stack data area (containing program arguments,
	// environment strings and related data).
	struct AuxFileData {
		AuxFileData(int fd, HelHandle pipe)
		: fd(fd), pipe(pipe) { }

		int fd;
		HelHandle pipe;
	};

	frigg::String<KernelAlloc> data_area(*kernelAlloc);
//	auto fd0_offset = copyToStack<AuxFileData>(data_area, { 1, stdout_handle });

	uintptr_t data_disp = stack_size - data_area.size();
	stack_memory->copyFrom(data_disp, data_area.data(), data_area.size());

	// build the stack tail area (containing the aux vector).
	enum {
		AT_NULL = 0,
		AT_PHDR = 3,
		AT_PHENT = 4,
		AT_PHNUM = 5,
		AT_ENTRY = 9,

		AT_OPENFILES = 0x1001,
		AT_POSIX_SERVER = 0x1101,
		AT_FS_SERVER = 0x1102
	};

	frigg::String<KernelAlloc> tail_area(*kernelAlloc);
	copyToStack<uintptr_t>(tail_area, AT_ENTRY);
	copyToStack<uintptr_t>(tail_area, (uintptr_t)exec_info.entryIp);
	copyToStack<uintptr_t>(tail_area, AT_PHDR);
	copyToStack<uintptr_t>(tail_area, (uintptr_t)exec_info.phdrPtr);
	copyToStack<uintptr_t>(tail_area, AT_PHENT);
	copyToStack<uintptr_t>(tail_area, exec_info.phdrEntrySize);
	copyToStack<uintptr_t>(tail_area, AT_PHNUM);
	copyToStack<uintptr_t>(tail_area, exec_info.phdrCount);
//	copyToStack<uintptr_t>(tail_area, AT_OPENFILES);
//	copyToStack<uintptr_t>(tail_area, (uintptr_t)stack_base + data_disp + fd0_offset);
	copyToStack<uintptr_t>(tail_area, AT_FS_SERVER);
	copyToStack<uintptr_t>(tail_area, initrd_handle);
	copyToStack<uintptr_t>(tail_area, AT_NULL);
	copyToStack<uintptr_t>(tail_area, 0);

	uintptr_t tail_disp = data_disp - tail_area.size();
	stack_memory->copyFrom(tail_disp, tail_area.data(), tail_area.size());

	// create a thread for the module
	auto thread = frigg::makeShared<Thread>(*kernelAlloc, *rootUniverse,
			frigg::move(space));
	thread->flags |= Thread::kFlagExclusive | Thread::kFlagTrapsAreFatal;
	thread->image.initSystemVAbi((uintptr_t)interp_info.entryIp,
			stack_base + tail_disp, false);

	// see helCreateThread for the reasoning here
	thread.control().increment();
	thread.control().increment();

	ScheduleGuard schedule_guard(scheduleLock.get());
	enqueueInSchedule(schedule_guard, frigg::move(thread));
}

extern "C" void thorMain(PhysicalAddr info_paddr) {
	frigg::infoLogger() << "Starting Thor" << frigg::endLog;

	initializeProcessorEarly();
	
	auto info = accessPhysical<EirInfo>(info_paddr);
	frigg::infoLogger() << "Bootstrap memory at "
			<< (void *)info->bootstrapPhysical
			<< ", length: " << (info->bootstrapLength / 1024) << " KiB" << frigg::endLog;

	physicalAllocator.initialize(info->bootstrapPhysical, info->bootstrapLength);
	physicalAllocator->bootstrap();

	PhysicalAddr pml4_ptr;
	asm volatile ( "mov %%cr3, %%rax" : "=a" (pml4_ptr) );
	kernelSpace.initialize(pml4_ptr);
	
	kernelVirtualAlloc.initialize();
	kernelAlloc.initialize(*kernelVirtualAlloc);

	for(int i = 0; i < 16; i++)
		irqRelays[i].initialize();

	scheduleQueue.initialize(*kernelAlloc);
	scheduleLock.initialize();

	initializeTheSystem();
	initializeThisProcessor();
	
	// create a directory and load the memory regions of all modules into it
	assert(info->numModules >= 1);
	auto modules = accessPhysicalN<EirModule>(info->moduleInfo,
			info->numModules);
	
	allModules.initialize(*kernelAlloc);
	for(size_t i = 1; i < info->numModules; i++) {
		size_t virt_length = modules[i].length + (kPageSize - (modules[i].length % kPageSize));
		assert((virt_length % kPageSize) == 0);

		// TODO: free module memory if it is not used anymore
		auto mod_memory = frigg::makeShared<Memory>(*kernelAlloc,
				HardwareMemory(modules[i].physicalBase, virt_length));
		
		auto name_ptr = accessPhysicalN<char>(modules[i].namePtr,
				modules[i].nameLength);
		frigg::infoLogger() << "Module " << frigg::StringView(name_ptr, modules[i].nameLength)
				<< ", length: " << modules[i].length << frigg::endLog;

		Module module(frigg::StringView(name_ptr, modules[i].nameLength),
				modules[i].physicalBase, modules[i].length);
		allModules->push(module);
	}
	
	// create a root universe and run a kernel thread to communicate with the universe 
	rootUniverse.initialize(frigg::makeShared<Universe>(*kernelAlloc));

	// finally we lauch the user_boot program
	executeModule(modules[0].physicalBase);

	frigg::infoLogger() << "Exiting Thor!" << frigg::endLog;
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(frigg::move(schedule_guard));
}

extern "C" void handleStubInterrupt() {
	frigg::panicLogger() << "Fault or IRQ from stub" << frigg::endLog;
}
extern "C" void handleBadDomain() {
	frigg::panicLogger() << "Fault or IRQ from bad domain" << frigg::endLog;
}

extern "C" void handleDivideByZeroFault(FaultImageAccessor image) {
	frigg::panicLogger() << "Divide by zero" << frigg::endLog;
}

extern "C" void handleDebugFault(FaultImageAccessor image) {
	frigg::infoLogger() << "Debug fault at "
			<< (void *)*image.ip() << frigg::endLog;
}

extern "C" void handleOpcodeFault(FaultImageAccessor image) {
	frigg::panicLogger() << "Invalid opcode" << frigg::endLog;
}

extern "C" void handleNoFpuFault(FaultImageAccessor image) {
	frigg::panicLogger() << "FPU invoked at "
			<< (void *)*image.ip() << frigg::endLog;
}

extern "C" void handleDoubleFault(FaultImageAccessor image) {
	frigg::panicLogger() << "Double fault at "
			<< (void *)*image.ip() << frigg::endLog;
}

extern "C" void handleProtectionFault(FaultImageAccessor image) {
	frigg::panicLogger() << "General protection fault\n"
			<< "    Faulting IP: " << (void *)*image.ip() << "\n"
			<< "    Faulting segment: " << (void *)*image.code() << frigg::endLog;
}

void handlePageFault(FaultImageAccessor image, uintptr_t address) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<AddressSpace> address_space = this_thread->getAddressSpace();

	const Word kPfAccess = 1;
	const Word kPfWrite = 2;
	const Word kPfUser = 4;
	const Word kPfBadTable = 8;
	const Word kPfInstruction = 16;
	assert(!(*image.code() & kPfBadTable));

	uint32_t flags = 0;
	if(*image.code() & kPfWrite)
		flags |= AddressSpace::kFaultWrite;

	AddressSpace::Guard space_guard(&address_space->lock);
	bool handled = address_space->handleFault(space_guard, address, flags);
	space_guard.unlock();
	
	if(!handled) {
		auto msg = frigg::panicLogger();
		msg << "Page fault"
				<< " at " << (void *)address
				<< ", faulting ip: " << (void *)*image.ip() << "\n";
		msg << "Errors:";
		if(*image.code() & kPfUser) {
			msg << " (User)";
		}else{
			msg << " (Supervisor)";
		}
		if(*image.code() & kPfAccess) {
			msg << " (Access violation)";
		}else{
			msg << " (Page not present)";
		}
		if(*image.code() & kPfWrite) {
			msg << " (Write)";
		}else if(*image.code() & kPfInstruction) {
			msg << " (Instruction fetch)";
		}else{
			msg << " (Read)";
		}
		msg << frigg::endLog;
	}
}

void handleOtherFault(FaultImageAccessor image, Fault fault) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();

	const char *name;
	switch(fault) {
	case kFaultBreakpoint: name = "breakpoint"; break;
	default:
		frigg::panicLogger() << "Unexpected fault code" << frigg::endLog;
	}

	if(this_thread->flags & Thread::kFlagTrapsAreFatal) {
		frigg::infoLogger() << "traps-are-fatal thread killed by " << name << " fault.\n"
				<< "Last ip: " << (void *)*image.ip() << frigg::endLog;
	}else{
		this_thread->transitionToFault();
		saveExecutorFromFault(image);
	}

	frigg::infoLogger() << "schedule after fault" << frigg::endLog;
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(frigg::move(schedule_guard));
}

void handleIrq(IrqImageAccessor image, int number) {
	assert(!intsAreEnabled());

	if(logEveryIrq)
		frigg::infoLogger() << "IRQ #" << number << frigg::endLog;
	
	if(number == 2)
		timerInterrupt();
	
	IrqRelay::Guard irq_guard(&irqRelays[number]->lock);
	irqRelays[number]->fire(irq_guard);
	irq_guard.unlock();
}

extern "C" void thorImplementNoThreadIrqs() {
	assert(!"Implement no-thread IRQ stubs");
}

extern "C" void handleSyscall(SyscallImageAccessor image) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	if(logEverySyscall && *image.number() != kHelCallLog)
		frigg::infoLogger() << this_thread.get()
				<< " syscall #" << *image.number() << frigg::endLog;

	Word arg0 = *image.in0();
	Word arg1 = *image.in1();
	Word arg2 = *image.in2();
	Word arg3 = *image.in3();
	Word arg4 = *image.in4();
	Word arg5 = *image.in5();
	Word arg6 = *image.in6();
	Word arg7 = *image.in7();
	Word arg8 = *image.in8();

	switch(*image.number()) {
	case kHelCallLog: {
		*image.error() = helLog((const char *)arg0, (size_t)arg1);
	} break;
	case kHelCallPanic: {
		frigg::infoLogger() << "User space panic:" << frigg::endLog;
		helLog((const char *)arg0, (size_t)arg1);
		
		while(true) { }
	} break;

	case kHelCallCreateUniverse: {
		HelHandle handle;
		*image.error() = helCreateUniverse(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallTransferDescriptor: {
		HelHandle out_handle;
		*image.error() = helTransferDescriptor((HelHandle)arg0, (HelHandle)arg1,
				&out_handle);
		*image.out0() = out_handle;
	} break;
	case kHelCallDescriptorInfo: {
		*image.error() = helDescriptorInfo((HelHandle)arg0, (HelDescriptorInfo *)arg1);
	} break;
	case kHelCallCloseDescriptor: {
//		frigg::infoLogger() << "helCloseDescriptor(" << (HelHandle)arg0 << ")" << frigg::endLog;
		*image.error() = helCloseDescriptor((HelHandle)arg0);
	} break;

	case kHelCallAllocateMemory: {
		HelHandle handle;
		*image.error() = helAllocateMemory((size_t)arg0, (uint32_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateManagedMemory: {
		HelHandle backing_handle, frontal_handle;
		*image.error() = helCreateManagedMemory((size_t)arg0, (uint32_t)arg1,
				&backing_handle, &frontal_handle);
		*image.out0() = backing_handle;
		*image.out1() = frontal_handle;
	} break;
	case kHelCallAccessPhysical: {
		HelHandle handle;
		*image.error() = helAccessPhysical((uintptr_t)arg0, (size_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateSpace: {
		HelHandle handle;
		*image.error() = helCreateSpace(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallForkSpace: {
		HelHandle forked;
		*image.error() = helForkSpace((HelHandle)arg0, &forked);
		*image.out0() = forked;
	} break;
	case kHelCallMapMemory: {
		void *actual_pointer;
		*image.error() = helMapMemory((HelHandle)arg0, (HelHandle)arg1,
				(void *)arg2, (uintptr_t)arg3, (size_t)arg4, (uint32_t)arg5, &actual_pointer);
		*image.out0() = (Word)actual_pointer;
	} break;
	case kHelCallUnmapMemory: {
		*image.error() = helUnmapMemory((HelHandle)arg0, (void *)arg1, (size_t)arg2);
	} break;
	case kHelCallPointerPhysical: {
		uintptr_t physical;
		*image.error() = helPointerPhysical((void *)arg0, &physical);
		*image.out0() = physical;
	} break;
	case kHelCallMemoryInfo: {
		size_t size;
		*image.error() = helMemoryInfo((HelHandle)arg0, &size);
		*image.out0() = size;
	} break;
	case kHelCallSubmitProcessLoad: {
		int64_t async_id;
		*image.error() = helSubmitProcessLoad((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallCompleteLoad: {
		*image.error() = helCompleteLoad((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;
	case kHelCallSubmitLockMemory: {
		int64_t async_id;
		*image.error() = helSubmitLockMemory((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (size_t)arg3,
				(uintptr_t)arg4, (uintptr_t)arg5, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallLoadahead: {
		*image.error() = helLoadahead((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;

	case kHelCallCreateThread: {
//		frigg::infoLogger() << "[" << this_thread->globalThreadId << "]"
//				<< " helCreateThread()"
//				<< frigg::endLog;
		HelHandle handle;
		*image.error() = helCreateThread((HelHandle)arg0, (HelHandle)arg1,
				(int)arg2, (void *)arg3, (void *)arg4, (uint32_t)arg5, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallYield: {
		*image.error() = helYield();
	} break;
	case kHelCallSubmitObserve: {
		int64_t async_id;
		*image.error() = helSubmitObserve((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallResume: {
		*image.error() = helResume((HelHandle)arg0);
	} break;
	case kHelCallExitThisThread: {
		*image.error() = helExitThisThread();
	} break;
	case kHelCallWriteFsBase: {
		*image.error() = helWriteFsBase((void *)arg0);
	} break;
	case kHelCallGetClock: {
		uint64_t counter;
		*image.error() = helGetClock(&counter);
		*image.out0() = counter;
	} break;

	case kHelCallCreateEventHub: {
//			frigg::infoLogger() << "helCreateEventHub" << frigg::endLog;
		HelHandle handle;
		*image.error() = helCreateEventHub(&handle);
//			frigg::infoLogger() << "    -> " << handle << frigg::endLog;
		*image.out0() = handle;
	} break;
	case kHelCallWaitForEvents: {
//			frigg::infoLogger() << "helWaitForEvents(" << (HelHandle)arg0
//					<< ", " << (void *)arg1 << ", " << (HelNanotime)arg2
//					<< ", " << (HelNanotime)arg3 << ")" << frigg::endLog;

		size_t num_items;
		*image.error() = helWaitForEvents((HelHandle)arg0,
				(HelEvent *)arg1, (size_t)arg2, (HelNanotime)arg3,
				&num_items);
		*image.out0() = num_items;
	} break;
	case kHelCallWaitForCertainEvent: {
		*image.error() = helWaitForCertainEvent((HelHandle)arg0,
				(int64_t)arg1, (HelEvent *)arg2, (HelNanotime)arg3);
	} break;
	
	case kHelCallCreateStream: {
		HelHandle lane1;
		HelHandle lane2;
		*image.error() = helCreateStream(&lane1, &lane2);
		*image.out0() = lane1;
		*image.out1() = lane2;
	} break;
	case kHelCallSubmitAsync: {
		*image.error() = helSubmitAsync((HelHandle)arg0, (HelAction *)arg1,
				(size_t)arg2, (HelHandle)arg3, (uint32_t)arg4);
	} break;
	
	case kHelCallCreateRing: {
		HelHandle handle;
		*image.error() = helCreateRing((HelHandle)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallSubmitRing: {
		int64_t async_id;
		*image.error() = helSubmitRing((HelHandle)arg0, (HelHandle)arg1,
				(HelRingBuffer *)arg2, (size_t)arg3,
				(uintptr_t)arg4, (uintptr_t)arg5, &async_id);
		*image.out0() = async_id;
	} break;

	case kHelCallCreateFullPipe: {
		HelHandle first;
		HelHandle second;
		*image.error() = helCreateFullPipe(&first, &second);
		*image.out0() = first;
		*image.out1() = second;
	} break;
	case kHelCallSubmitSendString: {
		int64_t async_id;
		*image.error() = helSubmitSendString((HelHandle)arg0,
				(HelHandle)arg1, (const void *)arg2, (size_t)arg3,
				(int64_t)arg4, (int64_t)arg5,
				(uintptr_t)arg6, (uintptr_t)arg7, (uint32_t)arg8, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitSendDescriptor: {
		int64_t async_id;
		*image.error() = helSubmitSendDescriptor((HelHandle)arg0,
				(HelHandle)arg1, (HelHandle)arg2,
				(int64_t)arg3, (int64_t)arg4,
				(uintptr_t)arg5, (uintptr_t)arg6, (uint32_t)arg7, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitRecvDescriptor: {
		int64_t async_id;
		*image.error() = helSubmitRecvDescriptor((HelHandle)arg0, (HelHandle)arg1,
				(int64_t)arg2, (int64_t)arg3,
				(uintptr_t)arg4, (uintptr_t)arg5, (uint32_t)arg6, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitRecvString: {
		int64_t async_id;
		*image.error() = helSubmitRecvString((HelHandle)arg0,
				(HelHandle)arg1, (void *)arg2, (size_t)arg3,
				(int64_t)arg4, (int64_t)arg5,
				(uintptr_t)arg6, (uintptr_t)arg7, (uint32_t)arg8, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitRecvStringToRing: {
		int64_t async_id;
		*image.error() = helSubmitRecvStringToRing((HelHandle)arg0,
				(HelHandle)arg1, (HelHandle)arg2,
				(int64_t)arg3, (int64_t)arg4,
				(uintptr_t)arg5, (uintptr_t)arg6, (uint32_t)arg7, &async_id);
		*image.out0() = async_id;
	} break;

	case kHelCallAccessIrq: {
		HelHandle handle;
		*image.error() = helAccessIrq((int)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallSetupIrq: {
		*image.error() = helSetupIrq((HelHandle)arg0, (uint32_t)arg1);
	} break;
	case kHelCallAcknowledgeIrq: {
		*image.error() = helAcknowledgeIrq((HelHandle)arg0);
	} break;
	case kHelCallSubmitWaitForIrq: {
		int64_t async_id;
		*image.error() = helSubmitWaitForIrq((HelHandle)arg0,
				(HelHandle)arg1, (uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;

	case kHelCallAccessIo: {
		HelHandle handle;
		*image.error() = helAccessIo((uintptr_t *)arg0, (size_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallEnableIo: {
		*image.error() = helEnableIo((HelHandle)arg0);
	} break;
	case kHelCallEnableFullIo: {
		*image.error() = helEnableFullIo();
	} break;
	
	case kHelCallControlKernel: {
		int subsystem = (int)arg0;
		int interface = (int)arg1;
		const void *user_input = (const void *)arg2;
		void *user_output = (void *)arg3;

		if(subsystem == kThorSubArch) {
			controlArch(interface, user_input, user_output);
			*image.error() = kHelErrNone;
		}else if(subsystem == kThorSubDebug) {
			if(interface == kThorIfDebugMemory) {
				frigg::infoLogger() << "Memory info:\n"
						<< "    Physical pages: Used: " << physicalAllocator->numUsedPages()
						<< ", free: " << physicalAllocator->numFreePages() << "\n"
						<< "    kernelAlloc: Used " << kernelAlloc->numUsedPages()
						<< frigg::endLog;
				*image.error() = kHelErrNone;
			}else{
				assert(!"Illegal debug interface");
			}
		}else{
			assert(!"Illegal subsystem");
		}
	} break;
	default:
		*image.error() = kHelErrIllegalSyscall;
	}

	if(this_thread->pendingSignal() == Thread::kSigKill) {
		frigg::infoLogger() << "Fix thread collection" << frigg::endLog;
//		this_thread.control().decrement();

		ScheduleGuard schedule_guard(scheduleLock.get());
		doSchedule(frigg::move(schedule_guard));
	}
	assert(!this_thread->pendingSignal());

//	frigg::infoLogger() << "exit syscall" << frigg::endLog;
}

} // namespace thor
