#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <x86/gdt.hpp>
#include <x86/idt.hpp>
#include <x86/machine.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <initgraph.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <thor-internal/types.hpp>

// NOTE: This header only provides architecture-specific structure and
// inline function definitions. Check arch-generic/cpu.hpp for the
// remaining function prototypes.

namespace thor {

// --------------------------------------------------------
// Global runtime functions
// --------------------------------------------------------

enum {
	kGdtIndexNull = 0,
	// Padding for the TSS.
	kGdtIndexPadding = 1,
	// Note that the TSS consumes two entries in the GDT.
	// We put it into the second GDT entry so that it is properly aligned.
	kGdtIndexTask = 2,
	// The order of the following segments is dictated by syscall/sysret.
	kGdtIndexKernelCode = 4,
	kGdtIndexKernelData = 5,
	// The order of the following segments is dictated by syscall/sysret.
	kGdtIndexUserCompat = 6,
	kGdtIndexUserData = 7,
	kGdtIndexUserCode = 8,
};

constexpr uint16_t selectorFor(uint16_t segment, uint16_t rpl) {
	return (segment << 3) | rpl;
}

enum {
	kSelTask = selectorFor(kGdtIndexTask, 0),

	kSelKernelCode = selectorFor(kGdtIndexKernelCode, 0),
	kSelKernelData = selectorFor(kGdtIndexKernelData, 0),
	kSelUserCompat = selectorFor(kGdtIndexUserCompat, 3),
	kSelUserData = selectorFor(kGdtIndexUserData, 3),
	kSelUserCode = selectorFor(kGdtIndexUserCode, 3),
};

struct CpuDescriptorTables {
	CpuDescriptorTables();

	CpuDescriptorTables(const CpuDescriptorTables &) = delete;

	CpuDescriptorTables &operator= (const CpuDescriptorTables &) = delete;

	uint32_t gdt[9 * 2];
	uint32_t idt[256 * 4];
	common::x86::Tss64 tss;
};

extern PerCpu<CpuDescriptorTables> cpuDescriptorTables;

struct Executor;

struct Continuation {
	void *sp;
};

// note: this struct is accessed from assembly.
// do not change the field offsets!
struct Frame {
	IplState iplState;
	// rbx, rcx and r11 are clobbered by syscalls.
	Word rbx;
	Word rcx;
	Word r11;
	// The remaining registers are preserved by all entry points.
	Word rax;
	Word rdx;
	Word rdi;
	Word rsi;
	Word r8;
	Word r9;
	Word r10;
	Word r12;
	Word r13;
	Word r14;
	Word r15;
	Word rbp;

	// Error for faults.
	Word code;

	// iret frame.
	Word rip;
	Word cs;
	Word rflags;
	Word rsp;
	Word ss;
};

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	Word *ip() { return &_frame()->rip; }
	Word *sp() { return &_frame()->rsp; }
	Word *cs() { return &_frame()->cs; }
	Word *ss() { return &_frame()->ss; }
	Word *rflags() { return &_frame()->rflags; }
	Word *code() { return &_frame()->code; }

	IplState *iplState() { return &_frame()->iplState; }

	bool inUserMode() {
		if(*cs() == kSelUserCompat
				|| *cs() == kSelUserCode) {
			return true;
		}else{
			assert(*cs() == kSelKernelCode);
			return false;
		}
	}

	bool allowUserPages();

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct IrqImageAccessor {
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);

	Word *ip() { return &_frame()->rip; }

	// TODO: These are only exposed for debugging.
	Word *cs() { return &_frame()->cs; }
	Word *rflags() { return &_frame()->rflags; }
	Word *ss() { return &_frame()->ss; }

	IplState *iplState() { return &_frame()->iplState; }

	bool intsEnabled() {
		return *rflags() & 0x200;
	}

	bool inUserMode() {
		if(*cs() == kSelUserCompat
				|| *cs() == kSelUserCode) {
			return true;
		}else{
			return false;
		}
	}

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct SyscallImageAccessor {
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

	Word *number() { return &_frame()->rdi; }
	Word *in0() { return &_frame()->rsi; }
	Word *in1() { return &_frame()->rdx; }
	Word *in2() { return &_frame()->rax; }
	Word *in3() { return &_frame()->r8; }
	Word *in4() { return &_frame()->r9; }
	Word *in5() { return &_frame()->r10; }
	Word *in6() { return &_frame()->r12; }
	Word *in7() { return &_frame()->r13; }
	Word *in8() { return &_frame()->r14; }

	Word *error() { return &_frame()->rdi; }
	Word *out0() { return &_frame()->rsi; }
	Word *out1() { return &_frame()->rdx; }

	IplState *iplState() { return &_frame()->iplState; }

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct NmiImageAccessor {
	void **expectedGs() {
		return &_layout()->expectedGs;
	}

	Word *ip() { return &_frame()->rip; }
	Word *cs() { return &_frame()->cs; }
	Word *rflags() { return &_frame()->rflags; }
	Word *bp() { return &_frame()->rbp; }

	IplState *iplState() { return &_frame()->iplState; }

private:
	struct Layout {
		Frame frame;
		void *expectedGs;
	};

	Layout *_layout() {
		return reinterpret_cast<Layout *>(_pointer);
	}

	Frame *_frame() {
		return &_layout()->frame;
	}

	char *_pointer;
};

// CpuData is some high-level struct that inherits from PlatformCpuData.
struct CpuData;

struct AbiParameters {
	uintptr_t ip;
	uintptr_t sp;
	uintptr_t argument;
};

struct UserContext {
	static void deactivate();

	UserContext();

	UserContext(const UserContext &other) = delete;

	UserContext &operator= (const UserContext &other) = delete;

	void enableIoPort(uintptr_t port);

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpu_data);

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
	common::x86::Tss64 tss;
};

struct FiberContext {
	FiberContext(UniqueKernelStack stack);

	FiberContext(const FiberContext &other) = delete;

	FiberContext &operator= (const FiberContext &other) = delete;

	// TODO: This should be private.
	UniqueKernelStack stack;
};

struct Executor;

// Restores the current executor from its saved image.
// This is functions does the heavy lifting during task switch.
// Note: due to the attribute, this must be declared before the friend declaration below.
[[noreturn]] void restoreExecutor(Executor *executor);

struct Executor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);
	friend void restoreExecutor(Executor *executor);
	friend void doForkExecutor(Executor *executor, void (*functor)(void *), void *context);

	static size_t determineSize();
	static size_t determineSimdSize();

	Executor();

private:
	explicit Executor(UserContext *context);

public:
	explicit Executor(UserContext *context, void (*launch)());
	explicit Executor(UserContext *context, AbiParameters abi);
	explicit Executor(FiberContext *context, AbiParameters abi);

	Executor(const Executor &other) = delete;

	~Executor();

	Executor &operator= (const Executor &other) = delete;

	void *getSyscallStack() {
		return _syscallStack;
	}

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	Word *rflags() { return &general()->rflags; }

	Word *ip() { return &general()->rip; }
	Word *sp() { return &general()->rsp; }
	Word *cs() { return &general()->cs; }
	Word *ss() { return &general()->ss; }

	Word *arg0() { return &general()->rsi; }
	Word *arg1() { return &general()->rdx; }
	Word *result0() { return &general()->rdi; }
	Word *result1() { return &general()->rsi; }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct General {
		Word rax;			// offset 0x00
		Word rbx;			// offset 0x08
		Word rcx;			// offset 0x10
		Word rdx;			// offset 0x18
		Word rsi;			// offset 0x20
		Word rdi;			// offset 0x28
		Word rbp;			// offset 0x30

		Word r8;			// offset 0x38
		Word r9;			// offset 0x40
		Word r10;			// offset 0x48
		Word r11;			// offset 0x50
		Word r12;			// offset 0x58
		Word r13;			// offset 0x60
		Word r14;			// offset 0x68
		Word r15;			// offset 0x70

		Word rip;			// offset 0x78
		Word cs;			// offset 0x80
		Word rflags;		// offset 0x88
		Word rsp;			// offset 0x90
		Word ss;			// offset 0x98
		Word clientFs;		// offset 0xA0
		Word clientGs;		// offset 0xA8
		IplState iplState;	// offset 0xB0
	};
	static_assert(sizeof(General) == 0xB8, "Bad sizeof(General)");

	struct FxState {
		uint16_t fcw; // x87 control word
		uint16_t fsw; // x87 status word
		uint8_t ftw; // x87 tag word
		uint8_t reserved0;
		uint16_t fop;
		uint64_t fpuIp;
		uint64_t fpuDp;
		uint32_t mxcsr;
		uint32_t mxcsrMask;
		uint8_t st0[10];
		uint8_t reserved1[6];
		uint8_t st1[10];
		uint8_t reserved2[6];
		uint8_t st2[10];
		uint8_t reserved3[6];
		uint8_t st3[10];
		uint8_t reserved4[6];
		uint8_t st4[10];
		uint8_t reserved5[6];
		uint8_t st5[10];
		uint8_t reserved6[6];
		uint8_t st6[10];
		uint8_t reserved7[6];
		uint8_t st7[10];
		uint8_t reserved8[6];
		uint8_t xmm0[16];
		uint8_t xmm1[16];
		uint8_t xmm2[16];
		uint8_t xmm3[16];
		uint8_t xmm4[16];
		uint8_t xmm5[16];
		uint8_t xmm6[16];
		uint8_t xmm7[16];
		uint8_t xmm8[16];
		uint8_t xmm9[16];
		uint8_t xmm10[16];
		uint8_t xmm11[16];
		uint8_t xmm12[16];
		uint8_t xmm13[16];
		uint8_t xmm14[16];
		uint8_t xmm15[16];
		uint8_t reserved9[48];
		uint8_t available[48];
	};
	static_assert(sizeof(FxState) == 512, "Bad sizeof(FxState)");
public:
	General *general() {
		return reinterpret_cast<General *>(_pointer);
	}

	FxState *_fxState() {
		// fxState is offset from General by 0x08 bytes
		// to make it aligned to a 64-byte boundary as required by xsave.
		return reinterpret_cast<FxState *>(_pointer + sizeof(General) + 0x08);
	}

	UserAccessRegion *currentUar() {
		return _uar;
	}

private:
	// Private function only used for the static_assert check.
	//
	// We can't put the static_assert outside because the members are private
	// and we can't put it at the end of the struct body because the type
	// is incomplete at that point.
	static void staticChecks() {
		static_assert(offsetof(Executor, _uar) == THOR_EXECUTOR_UAR);
	}

	char *_pointer;
	void *_syscallStack;
	common::x86::Tss64 *_tss;
	UserAccessRegion *_uar;
};

struct CpuFeatures {
	static constexpr uint32_t profileIntelSupported = 1;
	static constexpr uint32_t profileAmdSupported = 2;

	bool haveXsave;
	bool haveAvx;
	bool haveZmm;
	bool haveInvariantTsc;
	bool haveTscDeadline;
	bool haveVmx;
	bool haveSvm;
	uint32_t profileFlags;
	size_t xsaveRegionSize;
};

extern bool cpuFeaturesKnown;
extern CpuFeatures globalCpuFeatures;

[[gnu::const]] inline CpuFeatures *getGlobalCpuFeatures() {
	assert(cpuFeaturesKnown);
	return &globalCpuFeatures;
}

initgraph::Stage *getCpuFeaturesKnownStage();

// Determine whether this address belongs to the higher half.
inline constexpr bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void setupBootCpuContext();
void initializeThisProcessor();

void bootSecondary(unsigned int apic_id, size_t cpuIndex);

// Save the current SIMD register state into the given executor.
inline void saveCurrentSimdState(Executor *executor) {
	if(getGlobalCpuFeatures()->haveXsave) {
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	} else {
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

// --------------------------------------------------------
// TSC functionality.
// --------------------------------------------------------

inline void pause() {
	asm volatile ("pause");
}

} // namespace thor
