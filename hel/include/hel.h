
//! @file hel.h

#ifndef HEL_H
#define HEL_H

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "hel-types.h"

#ifdef __cplusplus
#define HEL_C_LINKAGE extern "C"
#else
#define HEL_C_LINKAGE
#endif

enum {
	// largest system call number plus 1
	kHelNumCalls = 113,

	kHelCallLog = 1,
	kHelCallPanic = 10,

	kHelCallNop = 2,
	kHelCallSubmitAsyncNop = 3,

	kHelCallCreateUniverse = 62,
	kHelCallTransferDescriptor = 66,
	kHelCallDescriptorInfo = 32,
	kHelCallGetCredentials = 84,
	kHelCallCloseDescriptor = 21,
	kHelCallObtainHandle = 4,

	kHelCallCreateQueue = 89,
	kHelCallDriveQueue = 105,
	kHelCallAlertQueue = 106,

	kHelCallAllocateMemory = 51,
	kHelCallResizeMemory = 83,
	kHelCallCreateManagedMemory = 64,
	kHelCallCopyOnWrite = 39,
	kHelCallAccessPhysical = 30,
	kHelCallCreateSliceView = 88,
	kHelCallForkMemory = 40,
	kHelCallCreateSpace = 27,
	kHelCallCreateDmaSpace = 108,
	kHelCallConfigureIrq = 109,
	kHelCallCreateIndirectMemory = 45,
	kHelCallAlterMemoryIndirection = 52,
	kHelCallMapMemory = 44,
	kHelCallSubmitProtectMemory = 99,
	kHelCallSubmitSynchronizeSpace = 53,
	kHelCallUnmapMemory = 36,
	kHelCallPointerPhysical = 43,
	kHelCallSubmitReadMemory = 77,
	kHelCallSubmitWriteMemory = 78,
	kHelCallMemoryInfo = 26,
	kHelCallSubmitManageMemory = 46,
	kHelCallUpdateMemory = 47,
	kHelCallSubmitLockMemoryView = 48,
	kHelCallLoadahead = 49,
	kHelCallCreateVirtualizedSpace = 50,
	kHelCallCreateSwapSpace = 110,
	kHelCallAllocateSwappableMemory = 111,
	kHelCallSetSwapBudget = 112,

	kHelCallAccessIommu = 13,

	kHelCallCreateThread = 67,
	kHelCallQueryThreadStats = 95,
	kHelCallSetPriority = 85,
	kHelCallYield = 34,
	kHelCallSubmitObserve = 74,
	kHelCallKillThread = 87,
	kHelCallInterruptThread = 86,
	kHelCallResume = 61,
	kHelCallLoadRegisters = 75,
	kHelCallStoreRegisters = 76,
	kHelCallQueryRegisterInfo = 102,
	kHelCallWriteFsBase = 41,
	kHelCallGetClock = 42,
	kHelCallSubmitAwaitClock = 80,
	kHelCallCreateVirtualizedCpu = 37,
	kHelCallRunVirtualizedCpu = 38,
	kHelCallAssertVirtualizedIrq = 107,
	kHelCallGetRandomBytes = 101,
	kHelCallWriteGsBase = 54,
	kHelCallReadFsBase = 55,
	kHelCallReadGsBase = 56,
	kHelCallGetCurrentCpu = 57,

	kHelCallCreateStream = 68,
	kHelCallSubmitAsync = 79,
	kHelCallShutdownLane = 91,

	kHelCallFutexWait = 73,
	kHelCallFutexWake = 71,

	kHelCallCreateOneshotEvent = 96,
	kHelCallCreateBitsetEvent = 97,
	kHelCallRaiseEvent = 98,
	kHelCallAccessIrq = 14,
	kHelCallHandleIrq = 5,
	kHelCallAcknowledgeIrq = 81,
	kHelCallSubmitAwaitEvent = 82,
	kHelCallAutomateIrq = 94,
	kHelCallQueryMsiInfo = 6,

	kHelCallAccessIo = 11,
	kHelCallEnableIo = 12,

	kHelCallBindKernlet = 93,

	kHelCallGetAffinity = 103,
	kHelCallSetAffinity = 100,

	kHelCallCreateToken = 104,

	kHelCallExtendHierarchy = 7,

	kHelCallSuper = 0x80000000
};

enum {
	kHelErrNone = 0,
	kHelErrBufferTooSmall = 1,
	kHelErrBadDescriptor = 2,
	kHelErrTimeout = 3,
	kHelErrNoDescriptor = 4,
	kHelErrIllegalSyscall = 5,
	kHelErrIllegalObject = 6,
	kHelErrIllegalArgs = 7,
	kHelErrLaneShutdown = 8,
	kHelErrEndOfLane = 9,
	kHelErrFault = 10,
	kHelErrThreadTerminated = 11,
	kHelErrCancelled = 12,
	kHelErrTransmissionMismatch = 13,
	kHelErrQueueTooSmall = 14,
	kHelErrIllegalState = 15,
	kHelErrNoHardwareSupport = 16,
	kHelErrNoMemory = 17,
	kHelErrUnsupportedOperation = 18,
	kHelErrOutOfBounds = 19,
	kHelErrDismissed = 20,
	kHelErrRemoteFault = 21,
	kHelErrAlreadyExists = 22,
	kHelErrBadPermissions = 23,
	kHelErrOther = 24,
	kHelErrFutexRace = 25,
	kHelErrBadRights = 26,
};

typedef uint32_t HelRights;

// No right but easier to recognize in code than a zero literal.
// - General: descriptor properties can be queried without any rights.
// - Memory view: memory size can be queried without any rights.
// - Thread: statistics can be queried without any rights.
static const HelRights kHelRightNull = 0;
// Right to map into an object.
// - Universe: required to transfer into the universe.
// - Universe: required to close descriptors in the universe.
// - Universe: required to create a thread in the universe.
// - Address space: required to map into the space.
// - Address space: required to change memory protection.
// - Virtualized space: required to map into the space.
// - DMA space: required to map into the space.
static const HelRights kHelRightGrant = UINT32_C(1) << 0;
// Right to map from an object.
// - Universe: required to transfer out of the universe.
// - Universe: required to create a thread in the universe.
static const HelRights kHelRightTake = UINT32_C(1) << 1;
// Right to read data.
// - IPC queue: required to map readable.
// - Memory view: required to read.
// - Memory view: required to map readable.
// - Memory view: required to create copy-on-write view.
// - Memory view: required to map into indirect memory.
// - Memory slice: required to map readable.
// - Memory slice: required to map into indirect memory.
// - Address space: required to read from it.
// - Address space: required to create thread.
// - Virtualized space: required to read from it.
// - I/O objects: required to enable userspace I/O.
// - Thread: required to read registers.
// - Thread: required to read memory in its address space.
// - Thread: required to get the affinity.
// - Virtualized CPUs: required to read registers.
static const HelRights kHelRightRead = UINT32_C(1) << 2;
// Right to write data.
// - IPC queue: required to map writeable.
// - Memory view: required to write.
// - Memory view: required to map writeable.
// - Memory view: required to map into indirect memory.
// - Memory slice: required to map writeable.
// - Memory slice: required to map into indirect memory.
// - Address space: required to write to it.
// - Address space: required to create thread.
// - Virtualized space: required to write to it.
// - I/O objects: required to enable userspace I/O.
// - Thread: required to write registers.
// - Thread: required to write memory in its address space.
// - Thread: required to set the affinity.
// - Virtualized CPUs: required to write registers.
static const HelRights kHelRightWrite = UINT32_C(1) << 3;
// Right to execute code.
// - Memory view: required to map executable.
// - Memory slice: required to map executable.
static const HelRights kHelRightExecute = UINT32_C(1) << 4;
// Right to execute operations.
// - IPC queue: required to submit.
// - Lane: required to do exchange messages.
// - Virtualized CPUs: required to run.
static const HelRights kHelRightInvoke = UINT32_C(1) << 5;
// Right to build new objects on top of existing ones.
// - IPC queue: required to map.
// - Universe: required to create thread.
// - Address space: required to create thread.
// - Memory view: required to map.
// - Memory view: required to create copy-on-write view.
// - Memory view: required to map into indirect memory.
// - Memory view: required to create slice.
// - Memory view: required to bind to kernlet.
// - IRQ pin: required to install an IRQ handler.
// - Memory slice: required to map.
// - Memory slice: required to map into indirect memory.
// - Swap space: required to allocate memory backed by it.
// - Virtualized space: required to create virtualized CPU.
// - I/O objects: required to enable userspace I/O.
// - Kernlets: required to create a bound kernlet from it.
// - Bound kernlets: required to attach to an IRQ.
static const HelRights kHelRightAssign = UINT32_C(1) << 6;
// Right to derive new objects that affect the original one.
// - Memory view: required to fork.
// - Hierarchy: required to extend it by a child hierarchy node.
static const HelRights kHelRightDerive = UINT32_C(1) << 7;
// Right to add, remove or manipulate components.
// - Memory views: required to perform loadahead.
// - Address space: required to synchronize.
// - Address space: required to resolve physical addresses.
// - DMA space: required to populate.
// - DMA space: required to resolve physical addresses.
// - Hierarchy: required to charge resources to it.
static const HelRights kHelRightProvision = UINT32_C(1) << 8;
// Right to pin memory pages.
// - Memory view: required to pin pages.
static const HelRights kHelRightPin = UINT32_C(1) << 9;
// Right to perform memory fences.
// - Memory view: required for fences.
static const HelRights kHelRightFence = UINT32_C(1) << 10;
// Right to wait on an object.
// - IPC queue: required to drive.
// - Thread: required to observe.
// - Event: required to wait.
// - IRQ: required to wait.
// - IRQ: required to attach a kernlet.
static const HelRights kHelRightWait = UINT32_C(1) << 11;
// Right to signal an object.
// - IPC queue: required to alert.
// - Thread: required to interrupt.
// - Event: required to raise.
// - Event: required to bind to kernlet.
// - IRQ: required to acknowledge.
// - IRQ: required to attach a kernlet.
// - Virtualized CPUs: required to raise an interrupt.
static const HelRights kHelRightSignal = UINT32_C(1) << 12;
// Right to manage an object (includes potentially destructive operations).
// - Lane: required to shut down.
// - Memory view: required to resize.
// - Memory view: required to manage.
// - Memory view: required to invalidate.
// - Swap space: required to set the swap budget.
// - Thread: required to set priority.
// - Thread: required to resume.
// - Thread: required to kill.
// - IRQ pin: required to configure the IRQ.
// - IRQ pin: required to obtain MSI address and data words.
static const HelRights kHelRightManage = UINT32_C(1) << 13;
// All rights (even unspecified ones). Easier to recognize in code than a literal.
static const HelRights kHelRightsMax = ~UINT32_C(0);

struct HelX86SegmentRegister {
	uint64_t base;
	uint32_t limit;
	uint16_t selector;
	uint8_t type, present, dpl, db, s, l, g, avl;
};

struct HelX86DescriptorTable {
	uint64_t base;
	uint16_t limit;
};

struct HelX86VirtualizationRegs {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t rbp;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;

	uint64_t rsp;
	uint64_t rip;
	uint64_t rflags;

	struct HelX86SegmentRegister cs, ds, es, fs, gs, ss;
	struct HelX86SegmentRegister tr, ldt;
	struct HelX86DescriptorTable gdt, idt;

	uint64_t cr0, cr2, cr3, cr4, cr8;
	uint64_t efer;
	uint64_t apic_base;
};

struct HelRiscv64VirtualizationRegs {
	union {
		uint64_t x[32];

		struct {
			uint64_t zero;
			uint64_t ra;
			uint64_t sp;
			uint64_t gp;
			uint64_t tp;
			uint64_t t0;
			uint64_t t1;
			uint64_t t2;
			uint64_t s0;
			uint64_t s1;
			uint64_t a0;
			uint64_t a1;
			uint64_t a2;
			uint64_t a3;
			uint64_t a4;
			uint64_t a5;
			uint64_t a6;
			uint64_t a7;
			uint64_t s2;
			uint64_t s3;
			uint64_t s4;
			uint64_t s5;
			uint64_t s6;
			uint64_t s7;
			uint64_t s8;
			uint64_t s9;
			uint64_t s10;
			uint64_t s11;
			uint64_t t3;
			uint64_t t4;
			uint64_t t5;
			uint64_t t6;
		};
	};

	uint64_t pc;
	uint8_t kernelMode;

	// CSRs
	uint64_t sstatus;
	uint64_t sie;
	uint64_t stvec;
	uint64_t sscratch;
	uint64_t sepc;
	uint64_t scause;
	uint64_t stval;
	uint64_t sip;
	uint64_t satp;
	uint64_t stimecmp;
};

#if defined(__riscv) && __riscv_xlen == 64
typedef struct HelRiscv64VirtualizationRegs HelVirtualizationRegs;
#else
typedef struct HelX86VirtualizationRegs HelVirtualizationRegs;
#endif

enum {
	kHelNullHandle = 0,
	kHelThisUniverse = -1,
	kHelThisThread = -2
};

enum {
	kHelWaitInfinite = -1
};

enum {
	kHelAbiSystemV = 1
};

enum {
	kHelActionNone = 0,
	kHelActionDismiss = 11,
	kHelActionOffer = 5,
	kHelActionAccept = 6,
	kHelActionImbueCredentials = 8,
	kHelActionExtractCredentials = 9,
	kHelActionSendFromBuffer = 1,
	kHelActionSendFromBufferSg = 10,
	kHelActionRecvInline = 7,
	kHelActionRecvToBuffer = 3,
	kHelActionPushDescriptor = 2,
	kHelActionPullDescriptor = 4
};

enum {
	kHelItemChain = 1,
	kHelItemAncillary = 2,
	kHelItemWantLane = (1 << 16),
};

static const uint32_t kHelTransferDescriptorOut = UINT32_C(1) << 0;
static const uint32_t kHelTransferDescriptorIn = UINT32_C(1) << 1;

enum {
	kHelObtainZeroMemory = 1,
	kHelObtainClockPage = 2
};

//! Clocks that userspace can read without a syscall (see HelClockPage).
enum {
	//! No clock that is accessible to userspace (= helGetClock() has to be used).
	kHelClockNone = 0,
	//! x86: TSC.
	kHelClockTsc = 1,
	//! ARM: physical counter (CNTPCT_EL0).
	kHelClockCntpct = 2,
	//! ARM: virtual counter (CNTVCT_EL0).
	kHelClockCntvct = 3,
	//! RISC-V: time CSR.
	kHelClockTime = 4
};

//! Parameters that allow userspace to compute the current time on the monotone clock
//! (i.e., the clock read by helGetClock()) without performing syscalls.
//!
//! Obtained as a read-only memory object via helObtainHandle() with kHelObtainClockPage.
//! All fields are protected by a seqlock:
//! readers repeat the read while seqlock is odd or while it changes during the read.
//!
//! The conversion from clock ticks to nanoseconds is (tickFactor * ticks) >> tickShift.
//! The multiplication is performed in 128-bit arithmetic.
struct HelClockPage {
	uint64_t seqlock;
	//! One of the kHelClock* constants.
	uint32_t clockType;
	//! Scaling exponent of the tick -> nanosecond conversion.
	int32_t tickShift;
	//! Factor of the tick -> nanosecond conversion.
	uint64_t tickFactor;
};

struct HelSgItem {
	void *buffer;
	size_t length;
};

struct HelAction {
	int type;
	uint32_t flags;
	// TODO: It may be worth to restructure this to a union of structs
	//       (e.g., a HelActionDataBufferSize, HelActionDataDescriptorRights, ...),
	//       but that requires simultaneous mlibc and Managarm changes.
	union {
		uintptr_t word0;
		void *buffer;
	};
	union {
		uintptr_t word1;
		size_t length;
		// For kHelPushDescriptor, kHelPullDescriptor.
		uint32_t rights;
	};
	union {
		uintptr_t word2;
		// For kHelActionImbueCredentials, kHelPushDescriptor, kHelPullDescriptor.
		HelHandle handle;
	};
};

struct HelDescriptorInfo {
	int type;
};

enum HelAllocFlags {
	kHelAllocContinuous = 4,
	kHelAllocOnDemand = 1,
};

struct HelAllocRestrictions {
	int addressBits;
};

enum HelManagedFlags {
	kHelManagedReadahead = 1
};

enum HelManageRequests {
	kHelManageInitialize = 1,
	kHelManageWriteback = 2
};

//! Additional @p type values of helUpdateMemory(); these are never issued as manage requests.
enum HelUpdateMemoryTypes {
	//! Marks a range as dirty, as if it had been written through a dirty-tracked mapping.
	//! Useful when used with kHelMapNoDirtyTracking mappings.
	//! Note that clean pages may be discarded at any time, so users need to pin them as appropriate.
	kHelUpdateMarkDirty = 3
};

enum HelMapFlags {
	// Additional flags that may be set.
	kHelMapProtRead = UINT32_C(1) << 8,
	kHelMapProtWrite = UINT32_C(1) << 9,
	kHelMapProtExecute = UINT32_C(1) << 10,
	kHelMapDontRequireBacking = UINT32_C(1) << 7,
	kHelMapFixed = UINT32_C(1) << 11,
	kHelMapFixedNoReplace = UINT32_C(1) << 12,
	kHelMapPreferBottom = UINT32_C(1) << 13,
	//! Discards the mapping's dirty bits instead of scheduling the pages for writeback.
	//! Requires kHelRightManage on the memory object.
	kHelMapNoDirtyTracking = UINT32_C(1) << 14
};

enum HelSliceFlags {
	kHelSliceCacheWriteCombine = 1,
};

enum HelCachingMode {
	kHelCachingDefault = 0,
	kHelCachingUncached = 1,
	kHelCachingWriteCombine = 2,
	kHelCachingWriteThrough = 3,
	kHelCachingWriteBack = 4,
	kHelCachingMmio = 5,
	kHelCachingMmioNonPosted = 6
};

enum HelThreadFlags {
	kHelThreadStopped = 1
};

enum HelObservation {
	kHelObserveNull = 0,
	kHelObserveInterrupt = 4,
	kHelObservePanic = 3,
	kHelObserveBreakpoint = 1,
	kHelObservePageFault = 2,
	kHelObserveGeneralFault = 5,
	kHelObserveIllegalInstruction = 6,
	kHelObserveDivByZero = 7,
	kHelObserveSuperCall = 0x80000000
};

enum HelRegisterSets {
	kHelRegsProgram = 1,
	kHelRegsGeneral = 2,
	kHelRegsThread = 3,
	kHelRegsDebug = 4,
	kHelRegsVirtualization = 5,
	kHelRegsSimd = 6,
	kHelRegsSignal = 7,
	kHelRegsPageFault = 8
};

enum HelPageFaultFlags {
	kHelPageFaultMapError = 1,
	kHelPageFaultAccessError = 2
};

//! Register-related information returned by helQueryRegisterInfo
struct HelRegisterInfo {
	//! Size of the selected register set
	int setSize;
};

#if defined(__x86_64__)
enum HelRegisterIndex {
	kHelRegRax = 0,
	kHelRegRbx = 1,
	kHelRegRcx = 2,
	kHelRegRdx = 3,
	kHelRegRdi = 4,
	kHelRegRsi = 5,
	kHelRegR8 = 6,
	kHelRegR9 = 7,
	kHelRegR10 = 8,
	kHelRegR11 = 9,
	kHelRegR12 = 10,
	kHelRegR13 = 11,
	kHelRegR14 = 12,
	kHelRegR15 = 13,
	kHelRegRbp = 14,

	kHelNumGprs = 15,

	kHelRegIp = 0,
	kHelRegSp = 1
};

enum HelSyscallArgs {
	kHelRegNumber = kHelRegRdi,
	kHelRegError = kHelRegRdi,

	kHelRegArg0 = kHelRegRsi,
	kHelRegArg1 = kHelRegRdx,
	kHelRegArg2 = kHelRegRax,
	kHelRegArg3 = kHelRegR8,
	kHelRegArg4 = kHelRegR9,
	kHelRegArg5 = kHelRegR10,
	kHelRegArg6 = kHelRegR12,
	kHelRegArg7 = kHelRegR13,
	kHelRegArg8 = kHelRegR14,

	kHelRegOut0 = kHelRegRsi,
	kHelRegOut1 = kHelRegRdx,
	kHelRegOut2 = kHelRegRax
};

#elif defined(__aarch64__)
enum HelRegisterIndex {
	kHelRegX0 = 0,
	kHelRegX1,
	kHelRegX2,
	kHelRegX3,
	kHelRegX4,
	kHelRegX5,
	kHelRegX6,
	kHelRegX7,
	kHelRegX8,
	kHelRegX9,
	kHelRegX10,
	kHelRegX11,
	kHelRegX12,
	kHelRegX13,
	kHelRegX14,
	kHelRegX15,
	kHelRegX16,
	kHelRegX17,
	kHelRegX18,
	kHelRegX19,
	kHelRegX20,
	kHelRegX21,
	kHelRegX22,
	kHelRegX23,
	kHelRegX24,
	kHelRegX25,
	kHelRegX26,
	kHelRegX27,
	kHelRegX28,
	kHelRegX29,
	kHelRegX30,

	kHelNumGprs = 31,

	kHelRegIp = 0,
	kHelRegSp = 1
};

enum HelSyscallArgs {
	kHelRegNumber = kHelRegX0,
	kHelRegError = kHelRegX0,

	kHelRegArg0 = kHelRegX1,
	kHelRegArg1 = kHelRegX2,
	kHelRegArg2 = kHelRegX3,
	kHelRegArg3 = kHelRegX4,
	kHelRegArg4 = kHelRegX5,
	kHelRegArg5 = kHelRegX6,
	kHelRegArg6 = kHelRegX7,
	kHelRegArg7 = kHelRegX8,
	kHelRegArg8 = kHelRegX9,

	kHelRegOut0 = kHelRegX1,
	kHelRegOut1 = kHelRegX2,
	kHelRegOut2 = kHelRegX3
};

#elif defined(__riscv) && __riscv_xlen == 64
enum HelRegisterIndex {
	kHelRegRa = 0,
	kHelRegGp,
	kHelRegTp,
	kHelRegT0,
	kHelRegT1,
	kHelRegT2,
	kHelRegS0,
	kHelRegS1,
	kHelRegA0,
	kHelRegA1,
	kHelRegA2,
	kHelRegA3,
	kHelRegA4,
	kHelRegA5,
	kHelRegA6,
	kHelRegA7,
	kHelRegS2,
	kHelRegS3,
	kHelRegS4,
	kHelRegS5,
	kHelRegS6,
	kHelRegS7,
	kHelRegS8,
	kHelRegS9,
	kHelRegS10,
	kHelRegS11,
	kHelRegT3,
	kHelRegT4,
	kHelRegT5,
	kHelRegT6,
	kHelNumGprs,
	kHelRegIp = 0,
	kHelRegSp = 1
};
enum HelSyscallArgs {
	kHelRegNumber = kHelRegA0,
	kHelRegError = kHelRegA0,
	kHelRegArg0 = kHelRegA1,
	kHelRegArg1 = kHelRegA2,
	kHelRegArg2 = kHelRegA3,
	kHelRegArg3 = kHelRegA4,
	kHelRegArg4 = kHelRegA5,
	kHelRegArg5 = kHelRegA6,
	kHelRegArg6 = kHelRegA7,
	kHelRegArg7 = kHelRegS2,
	kHelRegArg8 = kHelRegS3,

	kHelRegOut0 = kHelRegA1,
	kHelRegOut1 = kHelRegA2,
	kHelRegOut2 = kHelRegA3
};

#endif

struct HelQueueParameters {
	uint32_t flags;
	unsigned int numChunks;
	size_t chunkSize;
	unsigned int numSqChunks;
};

struct HelHierarchyParameters {
	// Optional tag to identify the hierarchy in kernel messages.
	// Null-terminated unless it fills the entire array.
	char tag[128];
};

//! Set in userNotify after kernel has written progress.
static const int kHelUserNotifyCqProgress = (1 << 0);
//! Set in userNotify after kernel has supplied new SQ chunks.
static const int kHelUserNotifySupplySqChunks = (1 << 1);
//! Set in userNotify if the queue encounters a contract violation.
static const int kHelUserNotifyError = (1 << 14);
//! Set in userNotify when the queue is alerted.
static const int kHelUserNotifyAlert = (1 << 15);

//! Set in kernelNotify after userspace has added SQ elements.
static const int kHelKernelNotifySqProgress = (1 << 0);
//! Set in kernelNotify after userspace has supplied new chunks.
static const int kHelKernelNotifySupplyCqChunks = (1 << 1);

//! Flag for helDriveQueue: wait until userNotify has any bits not in notifyMask set.
static const uint32_t kHelDriveWait = (1 << 0);

enum {
	kHelAccessIommuIntelVtd = 1,
	kHelAccessIommuAmdVi = 2,
};

enum {
	kHelDmaRegionRead = 1,
	kHelDmaRegionWrite = 2,
};

//! Range of physical memory that a DMA space identity-maps (see ::helCreateDmaSpace).
struct HelDmaReservedRegion {
	uint64_t base;
	uint64_t size;
	uint32_t flags;
};

//! Requester ID that a device issues DMA requests as (see ::kHelSubmitBindDmaDevice).
struct HelDmaDeviceId {
	uint32_t segment;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
};

//! SQ opcode: cancel an asynchronous operation.
static const uint32_t kHelSubmitCancel = 256;
//! SQ opcode: asynchronous no-op (for testing/profiling).
static const uint32_t kHelSubmitAsyncNop = 1;
//! SQ opcode: exchange messages on a stream.
static const uint32_t kHelSubmitExchangeMsgs = 2;
//! SQ opcode: wait for time to pass.
static const uint32_t kHelSubmitAwaitClock = 3;
//! SQ opcode: wait for an event.
static const uint32_t kHelSubmitAwaitEvent = 4;
//! SQ opcode: protect memory.
static const uint32_t kHelSubmitProtectMemory = 5;
//! SQ opcode: synchronize space.
static const uint32_t kHelSubmitSynchronizeSpace = 6;
//! SQ opcode: read memory.
static const uint32_t kHelSubmitReadMemory = 7;
//! SQ opcode: write memory.
static const uint32_t kHelSubmitWriteMemory = 8;
//! SQ opcode: manage memory.
static const uint32_t kHelSubmitManageMemory = 9;
//! SQ opcode: lock memory view.
static const uint32_t kHelSubmitLockMemoryView = 10;
//! SQ opcode: observe thread.
static const uint32_t kHelSubmitObserve = 11;
//! SQ opcode: resize memory.
static const uint32_t kHelSubmitResizeMemory = 12;
//! SQ opcode: fork memory.
static const uint32_t kHelSubmitForkMemory = 13;
//! SQ opcode: writeback fence.
static const uint32_t kHelSubmitWritebackFence = 14;
//! SQ opcode: invalidate memory.
static const uint32_t kHelSubmitInvalidateMemory = 15;
//! SQ opcode: populate a space.
static const uint32_t kHelSubmitPopulateSpace = 16;
//! SQ opcode: map memory.
static const uint32_t kHelSubmitMapMemory = 17;
//! SQ opcode: unmap memory.
static const uint32_t kHelSubmitUnmapMemory = 18;
//! SQ opcode: bind a device to a DMA space.
static const uint32_t kHelSubmitBindDmaDevice = 19;
//! SQ opcode: block the DMA requests of a device.
static const uint32_t kHelSubmitUnbindDmaDevice = 20;
//! SQ opcode: activate translation on an IOMMU.
static const uint32_t kHelSubmitActivateIommu = 21;

//! In-memory kernel/user-space queue.
struct HelQueue {
	//! Futex that is used to wake userspace.
	//! Kernel sets bits using atomic OR.
	//! Userspace clears bits using atomic AND.
	int userNotify;

	//! Futex that is used to wake the kernel.
	//! Userspace sets bits using atomic OR.
	//! Kernel clears bits using atomic AND.
	int kernelNotify;

	//! Index of the first chunk of the completion queue.
	//! Written by userspace and read by the kernel.
	int cqFirst;

	//! Index of the first chunk of the submission queue.
	//! Written by the kernel and read by userspace.
	int sqFirst;
};

//! Marks the next field as present.
static const int kHelNextPresent = (1 << 24);

//! Mask to extract the number of valid bytes in the chunk.
static const int kHelProgressMask = 0xFFFFFF;
//! Set by the producer once it retires the chunk.
static const int kHelProgressDone = (1 << 25);
//! Set by the producer once the chunk is full (i.e., no more bytes will be written).
static const int kHelProgressFull = (1 << 26);

struct HelChunk {
	//! Index of the next chunk.
	//! Written by consumer, read by producer.
	int next;

	//! Futex for kernel/user-space progress synchronization.
	int progressFutex;

	//! Actual contents of the chunk.
	char buffer[];
};

//! A single element of a HelQueue.
struct HelElement {
	//! Length of the element in bytes.
	unsigned int length;
	//! Operation code (for SQ elements).
	unsigned int opcode;
	//! User-defined value.
	void *context;
};

//! SQ data for kHelSubmitCancel.
struct HelSqCancel {
	uint64_t cancellationTag;
};

//! SQ data for kHelSubmitExchangeMsgs.
//! Followed by count HelAction structures.
struct HelSqExchangeMsgs {
	//! Handle to the lane.
	HelHandle lane;
	//! Number of actions.
	size_t count;
	//! Flags.
	uint32_t flags;
};

//! SQ data for kHelSubmitAwaitClock.
struct HelSqAwaitClock {
	//! Deadline in nanoseconds since boot.
	uint64_t counter;
	//! Tag to cancel this operation.
	uint64_t cancellationTag;
};

//! SQ data for kHelSubmitAwaitEvent.
struct HelSqAwaitEvent {
	//! Handle to the event descriptor.
	HelHandle handle;
	//! Previous sequence number.
	uint64_t sequence;
	//! Tag to cancel this operation.
	uint64_t cancellationTag;
};

//! SQ data for kHelSubmitProtectMemory.
struct HelSqProtectMemory {
	//! Handle to the address space.
	HelHandle spaceHandle;
	//! Pointer to the mapping.
	void *pointer;
	//! Size of the mapping.
	size_t size;
	//! Protection flags.
	uint32_t flags;
};

//! SQ data for kHelSubmitSynchronizeSpace.
struct HelSqSynchronizeSpace {
	//! Handle to the address space.
	HelHandle spaceHandle;
	//! Pointer to the mapping.
	void *pointer;
	//! Size of the mapping.
	size_t size;
};

//! SQ data for kHelSubmitReadMemory.
struct HelSqReadMemory {
	//! Handle to the memory object.
	HelHandle handle;
	//! Address within the memory object.
	uintptr_t address;
	//! Length in bytes.
	size_t length;
	//! Buffer to read into.
	void *buffer;
};

//! SQ data for kHelSubmitWriteMemory.
struct HelSqWriteMemory {
	//! Handle to the memory object.
	HelHandle handle;
	//! Address within the memory object.
	uintptr_t address;
	//! Length in bytes.
	size_t length;
	//! Buffer to write from.
	const void *buffer;
};

//! SQ data for kHelSubmitManageMemory.
struct HelSqManageMemory {
	//! Handle to the memory object.
	HelHandle handle;
};

//! SQ data for kHelSubmitLockMemoryView.
struct HelSqLockMemoryView {
	//! Handle to the memory object.
	HelHandle handle;
	//! Offset within the memory object.
	uintptr_t offset;
	//! Size to lock.
	size_t size;
};

//! SQ data for kHelSubmitObserve.
struct HelSqObserve {
	//! Handle to the thread.
	HelHandle handle;
};

//! SQ data for kHelSubmitResizeMemory.
struct HelSqResizeMemory {
	//! Handle to the memory object.
	HelHandle handle;
	//! New size in bytes.
	size_t newSize;
};

//! SQ data for kHelSubmitForkMemory.
struct HelSqForkMemory {
	//! Handle to the hierarchy object that owns the memory object.
	HelHandle hierarchyHandle;
	//! Handle to the memory object.
	HelHandle handle;
};

//! SQ data for kHelSubmitWritebackFence.
//! Collects dirty pages from mappings of the range and waits for writeback to complete.
struct HelSqWritebackFence {
	//! Handle to the memory object.
	HelHandle handle;
	//! Offset within the memory object.
	uintptr_t offset;
	//! Size of the range.
	size_t size;
};

//! Flag for kHelSubmitInvalidateMemory: discard dirty pages instead of writing them back.
static const uint32_t kHelInvalidateNoWriteback = 1;

//! SQ data for kHelSubmitInvalidateMemory.
struct HelSqInvalidateMemory {
	//! Handle to the memory object.
	HelHandle handle;
	//! Offset within the memory object.
	uintptr_t offset;
	//! Size of the range.
	size_t size;
	//! Flags (kHelInvalidate*).
	uint32_t flags;
};

//! SQ data for kHelSubmitPopulateSpace.
struct HelSqPopulateSpace {
	//! Handle to the memory space.
	HelHandle handle;
	//! Address within the memory space.
	uintptr_t address;
	//! Length of the range.
	size_t length;
};

//! SQ data for kHelSubmitMapMemory.
struct HelSqMapMemory {
	//! Handle to the memory object.
	HelHandle memoryHandle;
	//! Handle to the space that the memory is mapped into.
	HelHandle spaceHandle;
	//! Address to map at (depending on kHelMap*).
	void *pointer;
	//! Offset within the memory object.
	uintptr_t offset;
	//! Size of the mapping.
	size_t size;
	//! Flags (kHelMap*).
	uint32_t flags;
};

//! SQ data for kHelSubmitUnmapMemory.
struct HelSqUnmapMemory {
	//! Handle to the space that the mapping is removed from.
	HelHandle spaceHandle;
	//! Pointer to the mapping.
	void *pointer;
	//! Size of the mapping.
	size_t size;
};

//! SQ data for kHelSubmitBindDmaDevice.
//!
//! Programs an IOMMU to translate the DMA requests of a device through a DMA space.
struct HelSqBindDmaDevice {
	//! Handle to the IOMMU (see ::helAccessIommu).
	HelHandle iommuHandle;
	//! Handle to a DMA space of that IOMMU (see ::helCreateDmaSpace).
	//! kHelNullHandle binds the device in passthrough mode.
	HelHandle dmaSpaceHandle;
	//! Requester ID of the device.
	struct HelDmaDeviceId id;
};

//! SQ data for kHelSubmitUnbindDmaDevice.
//!
//! Programs an IOMMU to block the DMA requests of a device.
struct HelSqUnbindDmaDevice {
	//! Handle to the IOMMU (see ::helAccessIommu).
	HelHandle iommuHandle;
	//! Requester ID of the device.
	struct HelDmaDeviceId id;
};

//! SQ data for kHelSubmitActivateIommu.
//!
//! Until an IOMMU is activated, the unit is transparent and every requester DMAs untranslated.
//! Succeeds if the IOMMU is already active.
struct HelSqActivateIommu {
	//! Handle to the IOMMU (see ::helAccessIommu).
	HelHandle iommuHandle;
};

struct HelSimpleResult {
	HelError error;
	int reserved;
};

struct HelCredentialsResult {
	HelError error;
	int reserved;
	char credentials[16];
};

struct HelManageResult {
	HelError error;
	int type;
	uintptr_t offset;
	size_t length;
};

struct HelObserveResult {
	HelError error;
	unsigned int observation;
};

struct HelInlineResult {
	HelError error;
	int reserved;
	size_t length;
	char data[];
};

struct HelInlineResultNoFlex {
	HelError error;
	int reserved;
	size_t length;
};

struct HelLengthResult {
	HelError error;
	int reserved;
	size_t length;
};

struct HelHandleResult {
	HelError error;
	int reserved;
	HelHandle handle;
};

struct HelPointerResult {
	HelError error;
	int reserved;
	void *pointer;
};

struct HelEventResult {
	HelError error;
	uint32_t bitset;
	uint64_t sequence;
};

enum HelAckFlags {
	kHelAckAcknowledge = 2,
	kHelAckNack = 3,
	kHelAckKick = 1,
	kHelAckClear = 0x100,
};

union HelKernletData {
	HelHandle handle;
};

struct HelThreadStats {
	uint64_t userTime;
};

enum {
  kHelVmexitHlt = 0,
  kHelVmexitTranslationFault = 1,
  kHelVmexitHyperCall = 2,
  kHelVmexitInstructionTrap = 3,
  kHelVmexitError = -1,
  kHelVmexitUnknownPlatformSpecificExitCode = -2,
};

enum {
  kHelVmFaultRead = 1 << 0,
  kHelVmFaultWrite = 1 << 1,
  kHelVmFaultExecute = 1 << 2,
};

#if defined(__riscv) && __riscv_xlen == 64

struct HelVmexitReason {
	uint32_t exitReason;
	// If bit 63 is set then stores a compressed unexpanded instruction,
	// otherwise stores a normal 32-bit instruction (possibly originally compressed in which case bits 1:0 aren't 0b11).
	uint64_t instruction;
	size_t address;
	size_t flags;
};

#else

struct HelVmexitReason {
	uint32_t exitReason;
	uint32_t code;
	size_t address;
	size_t flags;
};

#endif

// see RFC 5424
enum HelLogSeverity {
	kHelLogSeverityEmergency,
	kHelLogSeverityAlert,
	kHelLogSeverityCritical,
	kHelLogSeverityError,
	kHelLogSeverityWarning,
	kHelLogSeverityNotice,
	kHelLogSeverityInfo,
	kHelLogSeverityDebug,
};

enum {
	kHelAccessIrqByGsi = 1,
	kHelAccessIrqByPhandle = 2,
	kHelAccessIrqAllocateMsi = 3,
};

//! Size of the buffer that ::helAccessIrq reads an IRQ name from.
enum {
	kHelIrqNameSize = 32,
};

enum {
	kHelIrqTriggerNull = 0,
	kHelIrqTriggerEdge = 1,
	kHelIrqTriggerLevel = 2
};

enum {
	kHelIrqPolarityNull = 0,
	kHelIrqPolarityHigh = 1,
	kHelIrqPolarityLow = 2
};

//! Message address / data pair that raises an MSI (see ::helQueryMsiInfo).
struct HelMsiInfo {
	uint64_t address;
	uint32_t data;
};

//! @name Logging
//! @{

//! Writes a text message (e.g., a line of text) to the kernel's log.
//! @param[in] severity
//!    	The RFC 5424 priority of the log message.
//! @param[in] string
//!    	Text to be written.
//! @param[in] length
//! 	Size of the text in bytes.
HEL_C_LINKAGE HelError helLog(const enum HelLogSeverity severity, const char *string, size_t length);

//! Kills the current thread and writes an error message to the kernel's log.
//! @param[in] string
//!    	Text to be written.
//! @param[in] length
//! 	Size of the text in bytes.
HEL_C_LINKAGE void helPanic(const char *string, size_t length)
		__attribute__ (( noreturn ));

//! @}
//! @name Debugging
//! @{

//! Does nothing (useful only for profiling).
HEL_C_LINKAGE HelError helNop();

//! Does nothing, asynchronously (useful only for profiling).
//!
//! This is an asynchronous operation.
HEL_C_LINKAGE HelError helSubmitAsyncNop(HelHandle queueHandle, uintptr_t context);

//! Creates a child hierarchy capability.
//! @param[in] hierarchyHandle
//!     Handle to the parent hierarchy capability.
//! @param[in] params
//!     Parameters for the new hierarchy node.
//! @param[out] handle
//!     Handle to the new child hierarchy capability.
HEL_C_LINKAGE HelError helExtendHierarchy(HelHandle hierarchyHandle,
		const struct HelHierarchyParameters *params, HelHandle *handle);

//! @}
//! @name Management of Descriptors and Universes
//! @{

//! Creates a new universe descriptor.
//! @param[out] handle
//!    	Handle to the new universe descriptor.
HEL_C_LINKAGE HelError helCreateUniverse(HelHandle *handle);

//! Copies descriptors from the current universe to another universe.
//! @param[in] handle
//!    	Handle the descriptor to transfer.
//! @param[in] universeHandle
//!    	Handle to the destination universe.
//! @param[out] outHandle
//!    	Handle to the copied descriptor (valid in the universe specified by @p universeHandle).
HEL_C_LINKAGE HelError
helTransferDescriptor(HelHandle handle, HelHandle universeHandle,
		uint32_t flags, uint32_t exposedRights, uint32_t requiredRights, HelHandle *outHandle);

HEL_C_LINKAGE HelError helDescriptorInfo(HelHandle handle, struct HelDescriptorInfo *info);

//! Returns the credentials associated with a given descriptor.
//! @param[in] handle
//!    	Handle to the descriptor.
//!    @param[out] credentials
//!    	Credentials, i.e., a 16-byte binary string.
HEL_C_LINKAGE HelError helGetCredentials(HelHandle handle, uint32_t flags,
		char *credentials);

//! Closes a descriptor.
//! @param[in] universeHandle
//!    	Handle to the universe containing @p handle.
//! @param[in] handle
//!    	Handle to be closed.
HEL_C_LINKAGE HelError helCloseDescriptor(HelHandle universeHandle, HelHandle handle);

//! Obtains a descriptor to one of the kernel's global objects.
//! @param[in] kind
//!    	Kind of the object, e.g., ::kHelObtainZeroMemory.
//! @param[out] handle
//!    	Handle to the object.
HEL_C_LINKAGE HelError helObtainHandle(int kind, HelHandle *handle);

//! @}
//! @name Management of IPC Queues
//! @{

//! Creates an IPC queue.
//! @param[in] params
//!    	Parameters for the queue.
//! @param[out] handle
//!    	Handle to the newly created queue.
HEL_C_LINKAGE HelError helCreateQueue(const struct HelQueueParameters *params,
		HelHandle *handle);

//! Drives an IPC queue.
//!
//! This function signals the kernel that new chunks have been supplied
//! and optionally waits for userNotify to have any bits not in notifyMask set.
//! @param[in] queueHandle
//!    	Handle to the queue.
//! @param[in] flags
//!    	Flags controlling the behavior.
//!    	If kHelDriveWait is set, the call blocks until (userNotify & ~notifyMask) != 0.
//! @param[in] notifyMask
//!    	Bits to ignore when checking userNotify (only relevant when kHelDriveWait is set).
HEL_C_LINKAGE HelError helDriveQueue(HelHandle queueHandle, uint32_t flags, uint32_t notifyMask);

//! Alerts an IPC queue.
//!
//! This function sets the kHelUserNotifyAlert flag in the queue's userNotify
//! and causes helDriveQueue() to return from waiting.
//! @param[in] queueHandle
//!    	Handle to the queue.
HEL_C_LINKAGE HelError helAlertQueue(HelHandle queueHandle);

//! @}
//! @name Memory Management
//! @{

//! Creates a memory object consisting of unmanaged RAM.
//! @param[in] hierarchy
//!    	Handle to the hierarchy that owns the new memory object.
//!    	The allocated physical memory is accounted to this hierarchy node.
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[in] restrictions
//!    	Specifies restrictions for the kernel's memory allocator.
//!    	May be @p NULL if there are no restrictions.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helAllocateMemory(HelHandle hierarchy, size_t size, uint32_t flags,
		const struct HelAllocRestrictions *restrictions, HelHandle *handle);

//! Resizes a memory object.
//! @param[in] handle
//!    	Handle to the memory object.
//!    	Must be aligned to the system's page size.
//! @param[in] newSize
//!    	New size in bytes.
HEL_C_LINKAGE HelError helResizeMemory(HelHandle handle, size_t newSize);

//! Creates a memory object that is managed by userspace.
//!
//!    The @p backingHandle is used to manage the memory object, while
//! the @p frontalHandle provides a view on the memory object for consumers.
//! @param[in] hierarchy
//!    	Handle to the hierarchy that owns the new memory object.
//!    	The allocated physical memory is accounted to this hierarchy node.
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] backingHandle
//!    	Handle to the new memory object (for management)
//! @param[out] frontalHandle
//!    	Handle to the new memory object (for consumers).
HEL_C_LINKAGE HelError helCreateManagedMemory(HelHandle hierarchy, size_t size, uint32_t flags,
		HelHandle *backingHandle, HelHandle *frontalHandle);

//! Creates a swap space, the backing store for swappable anonymous
//! memory (see helAllocateSwappableMemory()).
//!
//!    The @p backingHandle works like the backing handle of
//! helCreateManagedMemory(), addressed by kernel-allocated swap offsets: the
//! swap daemon maps it to perform disk I/O and services initialize ("read
//! these offsets from disk") and writeback ("write them out") requests
//! through helSubmitManageMemory()/helUpdateMemory(). The kernel only issues
//! writeback once a swap budget is set through helSetSwapBudget().
//! @param[in] hierarchy
//!    	Handle to the hierarchy that owns the swap space.
//!    	The resident frames of all memory backed by the swap space are accounted to this hierarchy node.
//! @param[out] backingHandle
//!    	Handle to the swap space's memory object (for the swap daemon).
//! @param[out] swapHandle
//!    	Handle identifying the swap space (for helAllocateSwappableMemory()
//!    	and helSetSwapLimit()).
HEL_C_LINKAGE HelError helCreateSwapSpace(HelHandle hierarchy, uint32_t flags,
		HelHandle *backingHandle, HelHandle *swapHandle);

//! Allocates memory that behaves like helAllocateMemory() but is backed by a
//! swap space: under memory pressure, dirty pages are written out through the
//! swap space's manage protocol and their frames are reclaimed.
//! @param[in] hierarchy
//!    	Handle to the hierarchy that owns the new memory object.
//!    	The swap slots held by the memory object are accounted to this hierarchy node as swap
//!    	(their resident frames are accounted to the swap space's hierarchy).
//! @param[in] swapSpace
//!    	Handle to the swap space (from helCreateSwapSpace()).
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helAllocateSwappableMemory(HelHandle hierarchy, HelHandle swapSpace,
		size_t size, uint32_t flags, HelHandle *handle);

//! Sets a swap space's budget, which is the number of pages that may be swapped out.
//! @param[in] swapSpace
//!    	Handle to the swap space (from helCreateSwapSpace()).
//! @param[in] numPages
//!    	Number of pages available for swap out.
HEL_C_LINKAGE HelError helSetSwapBudget(HelHandle swapSpace, size_t numPages);

//! Creates memory object that obtains its memory by copy-on-write from another memory object.
//! @param[in] hierarchy
//!    	Handle to the hierarchy that owns the new memory object.
//!    	The private copies are accounted to this hierarchy node, as swap if @p swapSpace is given
//!    	(their resident frames are then accounted to the swap space's hierarchy).
//! @param[in] swapSpace
//!    	Handle to a swap space (from helCreateSwapSpace()) that backs the private copies.
//!    	Forked copies (helForkMemory()) inherit the swap space.
//!    	kHelNullHandle makes the private copies unswappable.
//! @param[in] memory
//!    	Handle to the source memory object.
//! @param[in] offset
//!    	Offset in byte relative to @p memory.
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helCopyOnWrite(HelHandle hierarchy, HelHandle swapSpace, HelHandle memory,
		uintptr_t offset, size_t size, HelHandle *handle);

HEL_C_LINKAGE HelError helAccessPhysical(
	HelHandle accessToken, uintptr_t physical, size_t size, uint32_t cachingMode, HelHandle *handle
);

//! Creates a memory object that obtains its memory by delegating to other memory objects.
//! @param[in] numSlots
//! 	Number of slots, i.e., other memory objects that the indirect memory object refers to.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle);

//! Modifies indirect memory objects.
//!
//! @param[in] indirectHandle
//!    	Handle to the indirect memory object to be modified.
//!    	Must refer to a memory object created by ::helCreateIndirectMemory.
//! @param[in] slotIndex
//!    	Index of the slot to be modified. Must be a non-negative integer smaller than
//!    	@p numSlots (see ::helCreateIndirectMemory).
//!    	The slot must not be in use yet.
//! @param[in] memoryHandle
//!    	Handle to the memory object that @p indirectHandle should delegate to.
//! @param[in] offset
//!    	Offset in bytes, relative to @p memoryHandle.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the indirection in bytes.
//!    	Must be non-zero and aligned to the system's page size.
HEL_C_LINKAGE HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slotIndex,
		HelHandle memoryHandle, uintptr_t offset, size_t size);

HEL_C_LINKAGE HelError helCreateSliceView(HelHandle bundle, uintptr_t offset, size_t size,
		uint32_t flags, HelHandle *handle);

//! Forks memory objects, i.e., copies them using copy-on-write.
//!
//! @param[in] indirectHandle
//!    	Handle to the memory object to be forked.
//!    	Must refer to a memory object created by ::helCopyOnWrite.
//! @param[out] handle
//!    	Handle to the new (i.e., forked) memory object.
HEL_C_LINKAGE HelError helForkMemory(HelHandle handle, HelHandle *forkedHandle);

//! Creates a virtual address space that threads can run in.
//! @param[out] handle
//!     Handle to the new address space.
HEL_C_LINKAGE HelError helCreateSpace(HelHandle *handle);

//! Access an IOMMU that the kernel discovered from firmware.
//! @param[in] accessHandle
//!     Handle to the hardware access token.
//! @param[in] mode
//!     Determines how the IOMMU is identified (e.g., ::kHelAccessIommuIntelVtd).
//! @param[in] base
//!     Identifies the unit within that mode, i.e., its register base.
//! @param[out] handle
//!     Handle to the IOMMU.
HEL_C_LINKAGE HelError helAccessIommu(HelHandle accessHandle, uint32_t mode, uint64_t base,
		HelHandle *handle);

//! Creates a DMA space that memory can be mapped into for device DMA.
//!
//! With an IOMMU, the space is one IOMMU domain that the unit translates. The reserved
//! regions are identity-mapped and removed from the range that the space allocates addresses
//! from, such that the space is never observable without them.
//!
//! Without one, the space does not translate: it hands out the physical addresses of the
//! memory that is mapped into it, and reserved regions are rejected.
//! @param[in] iommuHandle
//!     Handle to the IOMMU (see ::helAccessIommu), or ::kHelNullHandle for a space that
//!     does not translate.
//! @param[in] regions
//!     Pointer to an array of physical ranges to identity-map.
//! @param[in] numRegions
//!     Number of ranges in @p regions.
//! @param[in] flags
//!     Flags for the creation of the DMA space.
//! @param[out] handle
//!     Handle to the new DMA space.
HEL_C_LINKAGE HelError helCreateDmaSpace(HelHandle iommuHandle,
		const struct HelDmaReservedRegion *regions, size_t numRegions,
		uint32_t flags, HelHandle *handle);

//! Maps memory objects into an address space.
//! @param[in] memoryHandle
//!     Handle to the memory object.
//! @param[in] spaceHandle
//!     Handle to the address space (see ::helCreateSpace).
//! @param[in] pointer
//!     Pointer to which the memory is mapped.
//!    	Can be specified as @p NULL to let the kernel pick a pointer.
//! @param[in] offset
//!    	Offset in bytes, relative to @p memoryHandle.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the mappping in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] actualPointer
//!    	Pointer to which the memory is mapped.
//!     Differs from @p pointer only if @p pointer was specified as @p NULL.
HEL_C_LINKAGE HelError helMapMemory(HelHandle memoryHandle, HelHandle spaceHandle,
		void *pointer, uintptr_t offset, size_t size, uint32_t flags, void **actualPointer);


//! Unmaps memory from an address space.
//!
//! @param[in] spaceHandle
//!     Handle to the address space containing @p pointer.
//! @param[in] pointer
//!     Pointer to the mapping that is unmapped.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the mapping that is unmapped.
//!    	Must be aligned to the system's page size.
HEL_C_LINKAGE HelError helUnmapMemory(HelHandle spaceHandle, void *pointer, size_t size);

HEL_C_LINKAGE HelError helPointerPhysical(HelHandle spaceHandle, const void *pointer, uintptr_t *physical);

HEL_C_LINKAGE HelError helMemoryInfo(HelHandle handle,
		size_t *size);

HEL_C_LINKAGE HelError helUpdateMemory(HelHandle handle, int type, uintptr_t offset, size_t length);

//! Notifies the kernel that a certain range of memory should be preloaded.
//!
//! This acts as a hint to the kernel and is meant purely as a performance optimization.
//! The kernel is free to ignore it.
//! @param[in] handle
//!     Handle to the memory object.
//! @param[in] offset
//!     Offset in bytes, relative to @p handle.
//! @param[in] length
//!     Length of the memory range that is preloaded.
HEL_C_LINKAGE HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length);

HEL_C_LINKAGE HelError helCreateVirtualizedSpace(HelHandle *handle);

//! @}
//! @name Thread Management
//! @{

//! Create a new thread.
//! @param[in] universeHandle
//!     Handle to universe of the new thread.
//! @param[in] spaceHandle
//!     Handle to universe of the new thread.
//! @param[in] abi
//!     ABI that the new thread should adhere to.
//! @param[in] ip
//!     Instruction pointer of the new thread.
//! @param[in] sp
//!     Stack pointer of the new thread.
//! @param[out] handle
//!     Handle to the new thread.
HEL_C_LINKAGE HelError helCreateThread(HelHandle universe, HelHandle spaceHandle,
		HelAbi abi, void *ip, void *sp, uint32_t flags, HelHandle *handle);

//! Query run-time statistics of a thread.
//! @param[in] handle
//!     Handle to the thread.
//! @param[out] stats
//!     Statistics related to the thread.
HEL_C_LINKAGE HelError helQueryThreadStats(HelHandle handle, struct HelThreadStats *stats);

//! Set the priority of a thread.
//!
//! Managarm always runs the runnable thread with highest priority.
//! The default priority of a thread is zero.
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] priority
//!     New priority value of the thread.
HEL_C_LINKAGE HelError helSetPriority(HelHandle handle, int priority);

//! Yields the current thread.
HEL_C_LINKAGE HelError helYield();

//! Kill (i.e., terminate) a thread.
//! @param[in] handle
//!     Handle to the thread.
HEL_C_LINKAGE HelError helKillThread(HelHandle handle);

//! Interrupt a thread.
//!
//! This system call temporarily suspends a thread.
//! The thread can later be resumed through the use of ::helResume.
//! @param[in] handle
//!     Handle to the thread.
HEL_C_LINKAGE HelError helInterruptThread(HelHandle handle);

//! Resume a suspended thread.
//!
//! Threads can explicitly be suspended through the use of ::helInterruptThread.
//! They are also suspended on faults and supercalls.
//! @param[in] handle
//!     Handle to the thread.
HEL_C_LINKAGE HelError helResume(HelHandle handle);

//! Load a register image (e.g., from a thread).
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] set
//!     Register set that will be accessed.
//! @param[out] image
//!     Copy of the register image.
HEL_C_LINKAGE HelError helLoadRegisters(HelHandle handle, int set, void *image);

//! Store a register image (e.g., to a thread).
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] set
//!     Register set that will be accessed.
//! @param[in] image
//!     Copy of the register image.
HEL_C_LINKAGE HelError helStoreRegisters(HelHandle handle, int set, const void *image);

//! Query register-related information.
//! @param[in] set
//      Register set to query information for.
//! @param[out] info
//!     Returned information.
HEL_C_LINKAGE HelError helQueryRegisterInfo(int set, struct HelRegisterInfo *info);

HEL_C_LINKAGE HelError helWriteFsBase(void *pointer);

HEL_C_LINKAGE HelError helReadFsBase(void **pointer);

HEL_C_LINKAGE HelError helWriteGsBase(void *pointer);

HEL_C_LINKAGE HelError helReadGsBase(void **pointer);

//! Gets the index of the cpu which the calling thread is running on.
HEL_C_LINKAGE HelError helGetCurrentCpu(int *cpu);

//! Read the system-wide monotone clock.
//!
//! @param[out] counter
//!     Current value of the system-wide clock in nanoseconds since boot.
HEL_C_LINKAGE HelError helGetClock(uint64_t *counter);

HEL_C_LINKAGE HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out_handle);

HEL_C_LINKAGE HelError helRunVirtualizedCpu(HelHandle handle, struct HelVmexitReason *reason);

HEL_C_LINKAGE HelError helAssertVirtualizedIrq(HelHandle handle, uint64_t irq, uint8_t level);

HEL_C_LINKAGE HelError helGetRandomBytes(void *buffer, size_t wantedSize, size_t *actualSize);

//! Get a thread's CPU affinity mask.
//! @param[in] handle
//!     Handle to the thread.
//! @param[out] mask
//!     Buffer to write the affinity bitmask to.
//! @param[in] size
//!     Size of bit mask buffer.
//! @param[out] actual_size
//!     Amount of bytes actually written to mask.
HEL_C_LINKAGE HelError helGetAffinity(HelHandle handle, uint8_t *mask, size_t size, size_t *actualSize);

//! Set a thread's CPU affinity mask.
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] mask
//!     Pointer to a bit mask of CPUs to schedule on.
//! @param[in] size
//!     Size of bit mask.
HEL_C_LINKAGE HelError helSetAffinity(HelHandle handle, uint8_t *mask, size_t size);

//! @}
//! @name Message Passing
//! @{

//! Create a stream (which always consists of two lanes).
//! @param[out] lane1
//!     Handle to the first lane of the new stream.
//! @param[out] lane2
//!     Handle to the second lane of the new stream.
//! @param[in] attach_credentials
//!     Enable or disable credentials for the new stream.
HEL_C_LINKAGE HelError helCreateStream(HelHandle *lane1, HelHandle *lane2, uint32_t attach_credentials);

HEL_C_LINKAGE HelError helShutdownLane(HelHandle handle);

//! Create a token object.
//!
//! A token object represents some unnamed credentials which can be shared.
//! Token objects can be passed to ImbueCredentials to send the credentials they
//! hold instead of the thread's credentials.
//! @param[out] handle
//!     Handle to the token object
HEL_C_LINKAGE HelError helCreateToken(HelHandle *handle);

//! @}
//! @name Inter-Thread Synchronization
//! @{

//! Waits on a futex.
//! @param[in] pointer
//!     Pointer that identifies the futex.
//! @param[in] expected
//!     Expected value of the futex. This function does nothing unless
//!     the futex pointed to by @pointer matches this value.
//! @param[in] deadline
//!     Timeout (in absolute monotone time, see ::helGetClock).
HEL_C_LINKAGE HelError helFutexWait(int *pointer, int expected, int64_t deadline);

//! Wakes up all waiters of a futex.
//! @param[in] pointer
//!     Pointer that identifies the futex.
//! @param[in] count
//!     Maximum number of waiters to wake.
HEL_C_LINKAGE HelError helFutexWake(int *pointer, unsigned int count);

//! @}
//! @name Event Handling
//! @{

//! Create an event that fires at most once.
//! @param[out] handle
//!     Handle to the new event.
HEL_C_LINKAGE HelError helCreateOneshotEvent(HelHandle *handle);

//! Create an event consisting of multiple bits that can fire independently.
//! @param[out] handle
//!     Handle to the new event.
HEL_C_LINKAGE HelError helCreateBitsetEvent(HelHandle *handle);

//! Raise an event.
//! @param[in] handle
//!     Handle to the event that will be raised.
HEL_C_LINKAGE HelError helRaiseEvent(HelHandle handle);

//! Access the IRQ pin that an IRQ is attached to.
//! For ::kHelAccessIrqAllocateMsi, allocate a fresh MSI pin from the system's
//! MSI controller instead of accessing an existing IRQ.
//! @param[in] mode
//!     Determines how the IRQ is identified (e.g., ::kHelAccessIrqByGsi).
//! @param[in] controller
//!     Identifies the interrupt controller that the IRQ belongs to.
//!     Unused for ::kHelAccessIrqByGsi and ::kHelAccessIrqAllocateMsi.
//!     For ::kHelAccessIrqByPhandle, this is the device tree phandle of the controller.
//! @param[in] index
//!     Identifies the IRQ within the interrupt controller.
//!     For ::kHelAccessIrqByGsi, this is the global system interrupt number.
//!     For ::kHelAccessIrqByPhandle, this is a controller-specific IRQ index.
//!     Unused for ::kHelAccessIrqAllocateMsi.
//! @param[in] name
//!     Names the pin in kernel diagnostics. The kernel reads ::kHelIrqNameSize bytes from
//!     this buffer and stops at the first null byte, if any.
//!     Only used for ::kHelAccessIrqAllocateMsi, which requires it to be non-null.
//! @param[out] handle
//!     Handle to the IRQ pin.
HEL_C_LINKAGE HelError helAccessIrq(
	HelHandle accessHandle, uint32_t mode, uint64_t controller, uint64_t index,
	const char *name, HelHandle *handle
);

//! Install an IRQ handler on an IRQ pin.
//! @param[in] pinHandle
//!     Handle to the IRQ pin that the handler is installed on.
//! @param[out] handle
//!     Handle to the new IRQ object.
HEL_C_LINKAGE HelError helHandleIrq(HelHandle pinHandle, HelHandle *handle);

//! Configure the trigger mode and polarity of an IRQ pin.
//! @param[in] pinHandle
//!     Handle to the IRQ pin that is configured.
//! @param[in] trigger
//!     Trigger mode of the IRQ (see ::kHelIrqTriggerEdge and ::kHelIrqTriggerLevel).
//!     ::kHelIrqTriggerNull states that the interrupt controller has no configurable
//!     trigger mode.
//! @param[in] polarity
//!     Polarity of the IRQ (see ::kHelIrqPolarityHigh and ::kHelIrqPolarityLow).
//!     ::kHelIrqPolarityNull states that the interrupt controller has no configurable
//!     polarity.
HEL_C_LINKAGE HelError helConfigureIrq(HelHandle pinHandle, uint32_t trigger, uint32_t polarity);

//! Retrieves the message address and data of an MSI pin.
//! @param[in] pinHandle
//!     Handle to the IRQ pin. The pin must be message-signaled
//!     (e.g., allocated via ::kHelAccessIrqAllocateMsi).
//! @param[out] info
//!     Message address / data pair that raises the MSI.
HEL_C_LINKAGE HelError helQueryMsiInfo(HelHandle pinHandle, struct HelMsiInfo *info);

HEL_C_LINKAGE HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence);

HEL_C_LINKAGE HelError helAutomateIrq(HelHandle handle, uint32_t flags, HelHandle kernlet);

//! @}
//! @name Input/Output
//! @{

HEL_C_LINKAGE HelError helAccessIo(
	HelHandle accessToken, const uintptr_t *portArray, size_t numPorts, HelHandle *handle
);

//! Enable userspace access to hardware I/O resources.
//! @param[in] handle
//!     Handle to the hardware I/O resource.
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);

//! @}
//! @name Kernlet Management
//! @{

//! Bind parameters to a kernlet.
//! @param[in] handle
//!     Handle to the unbound kernlet.
//! @param[in] data
//!     Pointer to an array of binding parameters.
//! @param[in] numData
//!     Number of binding parameters in @p data.
//! @param[out] boundHandle
//!     Handle to the bound kernlet.
HEL_C_LINKAGE HelError helBindKernlet(HelHandle handle,
		const union HelKernletData *data, size_t numData, HelHandle *boundHandle);

//! @}

extern inline __attribute__ (( always_inline )) const char *_helErrorString(HelError code) {
	switch(code) {
	case kHelErrNone:
		return "Success";
	case kHelErrIllegalSyscall:
		return "Illegal syscall";
	case kHelErrIllegalArgs:
		return "Illegal arguments";
	case kHelErrIllegalState:
		return "Illegal state";
	case kHelErrUnsupportedOperation:
		return "Unsupported operation";
	case kHelErrNoDescriptor:
		return "No such descriptor";
	case kHelErrBadDescriptor:
		return "Illegal descriptor for this operation";
	case kHelErrThreadTerminated:
		return "Thread terminated already";
	case kHelErrLaneShutdown:
		return "Lane shutdown";
	case kHelErrEndOfLane:
		return "End of lane";
	case kHelErrDismissed:
		return "IPC item dismissed by remote";
	case kHelErrBufferTooSmall:
		return "Buffer too small";
	case kHelErrQueueTooSmall:
		return "Buffer too small";
	case kHelErrFault:
		return "Segfault";
	case kHelErrNoHardwareSupport:
		return "Missing hardware support for this feature";
	case kHelErrNoMemory:
		return "Out of memory";
	case kHelErrTransmissionMismatch:
		return "Transmission mismatch";
	case kHelErrCancelled:
		return "Cancelled";
	case kHelErrOutOfBounds:
		return "Out of bounds";
	case kHelErrAlreadyExists:
		return "Already exists";
	case kHelErrBadRights:
		return "Missing rights for this operation";
	default:
		return 0;
	}
}

extern inline __attribute__ (( always_inline )) void _helCheckFailed(HelError err_code,
		const char *string, int fatal) {
	helLog(kHelLogSeverityError, string, strlen(string));

	const char *err_string = _helErrorString(err_code);
	if(err_string == 0)
		err_string = "(Unexpected error code)";
	helLog(kHelLogSeverityError, err_string, strlen(err_string));
	helLog(kHelLogSeverityError, "\n", 1);

	if(fatal)
		helPanic(0, 0);
}

#define HEL_STRINGIFY_AUX(x) #x
#define HEL_STRINGIFY(x) HEL_STRINGIFY_AUX(x)

#define HEL_CHECK(expr) do { HelError __error = expr; if(__error != kHelErrNone) \
		_helCheckFailed(__error, "HEL_CHECK failed: " #expr "\n" \
		"    In file " __FILE__ " on line " HEL_STRINGIFY(__LINE__) "\n", 1); } while(0)
#define HEL_SOFT_CHECK(expr) do { HelError __error = expr; if(__error != kHelErrNone) \
		_helCheckFailed(__error, "HEL_SOFT_CHECK failed: " #expr "\n" \
		"    In file " __FILE__ " on line " HEL_STRINGIFY(__LINE__) "\n", 0); } while(0)

#endif // HEL_H

