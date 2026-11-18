#pragma once

#include <hel.h>

#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch/virtualization.hpp>
#include <thor-internal/arch/vlapic.hpp>

namespace thor::vmx {
	constexpr uint64_t FEATURE_CONTROL_LOCK              = 0x00000001;
	constexpr uint64_t FEATURE_CONTROL_VMXON_OUTSIDE_SMX = 0x00000004;
	constexpr uint64_t MSR_IA32_VMX_PINBASED_CTLS        = 0x00000481;
	constexpr uint64_t PIN_BASED_VM_EXEC_CONTROLS        = 0x00004000;
	constexpr uint64_t MSR_IA32_VMX_PROCBASED_CTLS       = 0x00000482;
	constexpr uint64_t PROC_BASED_VM_EXEC_CONTROLS       = 0x00004002;
	constexpr uint64_t PROC_BASED_VM_EXEC_CONTROLS2      = 0x0000401e;
	constexpr uint64_t EXCEPTION_BITMAP                  = 0x00004004;
	constexpr uint64_t VM_EXIT_CONTROLS                  = 0x0000400c;
	constexpr uint64_t MSR_IA32_VMX_EXIT_CTLS            = 0x00000483;
	constexpr uint64_t VM_EXIT_HOST_ADDR_SPACE_SIZE      = 0x00000200;
	constexpr uint64_t VM_ENTRY_CONTROLS                 = 0x00004012;
	constexpr uint64_t MSR_IA32_VMX_ENTRY_CTLS           = 0x00000484;
	constexpr uint64_t VM_ENTRY_IA32E_MODE               = 0x00000200;
	constexpr uint64_t HOST_CR0                          = 0x00006c00;
	constexpr uint64_t HOST_CR3                          = 0x00006c02;
	constexpr uint64_t HOST_CR4                          = 0x00006c04;
	constexpr uint64_t HOST_ES_SELECTOR                  = 0x00000c00;
	constexpr uint64_t HOST_CS_SELECTOR                  = 0x00000c02;
	constexpr uint64_t HOST_SS_SELECTOR                  = 0x00000c04;
	constexpr uint64_t HOST_DS_SELECTOR                  = 0x00000c06;
	constexpr uint64_t HOST_FS_SELECTOR                  = 0x00000c08;
	constexpr uint64_t HOST_GS_SELECTOR                  = 0x00000c0a;
	constexpr uint64_t HOST_TR_SELECTOR                  = 0x00000c0c;
	constexpr uint64_t HOST_FS_BASE                      = 0x00006c06;
	constexpr uint64_t HOST_GS_BASE                      = 0x00006c08;
	constexpr uint64_t HOST_TR_BASE                      = 0x00006c0a;
	constexpr uint64_t HOST_GDTR_BASE                    = 0x00006c0c;
	constexpr uint64_t HOST_IDTR_BASE                    = 0x00006c0e;
	constexpr uint64_t HOST_IA32_SYSENTER_ESP            = 0x00006c10;
	constexpr uint64_t HOST_IA32_SYSENTER_EIP            = 0x00006c12;
	constexpr uint64_t HOST_IA32_SYSENTER_CS             = 0x00004c00;
	constexpr uint64_t HOST_RSP                          = 0x00006c14;
	constexpr uint64_t HOST_RIP                          = 0x00006c16;
	constexpr uint64_t RFLAG_RESERVED                    = (1 << 1);
	constexpr uint64_t GUEST_RFLAG                       = 0x00006820;
	constexpr uint64_t HOST_GDT_LIMIT                    = 14 * 8;

	constexpr uint64_t IA32_VMX_BASIC_MSR                = 0x480;
	constexpr uint64_t IA32_VMX_CR0_FIXED0_MSR           = 0x486;
	constexpr uint64_t IA32_VMX_CR0_FIXED1_MSR           = 0x487;
	constexpr uint64_t IA32_VMX_CR4_FIXED0_MSR           = 0x488;
	constexpr uint64_t IA32_VMX_CR4_FIXED1_MSR           = 0x489;
	constexpr uint64_t IA32_VMX_PINBASED_CTLS_MSR        = 0x481;
	constexpr uint64_t IA32_VMX_PRI_PROCBASED_CTLS_MSR   = 0x482;
	constexpr uint64_t IA32_VMX_SEC_PROCBASED_CTLS_MSR   = 0x48b;
	constexpr uint64_t IA32_VMX_EPT_VPID_CAP_MSR         = 0x48c;
	constexpr uint64_t IA32_VMX_VM_EXIT_CTLS_MSR         = 0x483;
	constexpr uint64_t IA32_VMX_VM_ENTRY_CTLS_MSR        = 0x484;
	constexpr uint64_t HOST_EFER_FULL                    = 0x00002c02;
	// Used instead of the CTLS MSRs if bit 55 of IA32_VMX_BASIC is set.
	constexpr uint64_t IA32_VMX_TRUE_PINBASED_CTLS_MSR   = 0x48d;
	constexpr uint64_t IA32_VMX_TRUE_PROCBASED_CTLS_MSR  = 0x48e;
	constexpr uint64_t IA32_VMX_TRUE_EXIT_CTLS_MSR       = 0x48f;
	constexpr uint64_t IA32_VMX_TRUE_ENTRY_CTLS_MSR      = 0x490;
	constexpr uint64_t CR0_GUEST_HOST_MASK               = 0x00006000;
	constexpr uint64_t CR4_GUEST_HOST_MASK               = 0x00006002;
	constexpr uint64_t CR0_READ_SHADOW                   = 0x00006004;
	constexpr uint64_t CR4_READ_SHADOW                   = 0x00006006;

	constexpr uint64_t GUEST_DR7 = 0x0000681A;
	constexpr uint64_t GUEST_RSP = 0x0000681C;
	constexpr uint64_t GUEST_RIP = 0x0000681E;
	constexpr uint64_t GUEST_CR0 = 0x00006800;
	constexpr uint64_t GUEST_CR3 = 0x00006802;
	constexpr uint64_t GUEST_CR4 = 0x00006804;
	constexpr uint64_t CTLS_EPTP = 0x0000201A;

	constexpr uint64_t GUEST_ES_SELECTOR                = 0x00000800;
	constexpr uint64_t GUEST_CS_SELECTOR                = 0x00000802;
	constexpr uint64_t GUEST_SS_SELECTOR                = 0x00000804;
	constexpr uint64_t GUEST_DS_SELECTOR                = 0x00000806;
	constexpr uint64_t GUEST_FS_SELECTOR                = 0x00000808;
	constexpr uint64_t GUEST_GS_SELECTOR                = 0x0000080a;
	constexpr uint64_t GUEST_LDTR_SELECTOR              = 0x0000080c;
	constexpr uint64_t GUEST_TR_SELECTOR                = 0x0000080e;
	constexpr uint64_t GUEST_ES_LIMIT                   = 0x00004800;
	constexpr uint64_t GUEST_CS_LIMIT                   = 0x00004802;
	constexpr uint64_t GUEST_SS_LIMIT                   = 0x00004804;
	constexpr uint64_t GUEST_DS_LIMIT                   = 0x00004806;
	constexpr uint64_t GUEST_FS_LIMIT                   = 0x00004808;
	constexpr uint64_t GUEST_GS_LIMIT                   = 0x0000480a;
	constexpr uint64_t GUEST_LDTR_LIMIT                 = 0x0000480c;
	constexpr uint64_t GUEST_TR_LIMIT                   = 0x0000480e;
	constexpr uint64_t GUEST_GDTR_LIMIT                 = 0x00004810;
	constexpr uint64_t GUEST_IDTR_LIMIT                 = 0x00004812;
	constexpr uint64_t GUEST_ES_BASE                    = 0x00006806;
	constexpr uint64_t GUEST_CS_BASE                    = 0x00006808;
	constexpr uint64_t GUEST_SS_BASE                    = 0x0000680a;
	constexpr uint64_t GUEST_DS_BASE                    = 0x0000680c;
	constexpr uint64_t GUEST_FS_BASE                    = 0x0000680e;
	constexpr uint64_t GUEST_GS_BASE                    = 0x00006810;
	constexpr uint64_t GUEST_LDTR_BASE                  = 0x00006812;
	constexpr uint64_t GUEST_TR_BASE                    = 0x00006814;
	constexpr uint64_t GUEST_GDTR_BASE                  = 0x00006816;
	constexpr uint64_t GUEST_IDTR_BASE                  = 0x00006818;

	constexpr uint64_t GUEST_ES_ACCESS_RIGHT        = 0x00004814;
	constexpr uint64_t GUEST_CS_ACCESS_RIGHT        = 0x00004816;
	constexpr uint64_t GUEST_SS_ACCESS_RIGHT        = 0x00004818;
	constexpr uint64_t GUEST_DS_ACCESS_RIGHT        = 0x0000481A;
	constexpr uint64_t GUEST_FS_ACCESS_RIGHT        = 0x0000481C;
	constexpr uint64_t GUEST_GS_ACCESS_RIGHT        = 0x0000481E;
	constexpr uint64_t GUEST_LDTR_ACCESS_RIGHT      = 0x00004820;
	constexpr uint64_t GUEST_TR_ACCESS_RIGHT        = 0x00004822;
	constexpr uint64_t GUEST_INTERRUPTIBILITY_STATE = 0x00004824;
	constexpr uint64_t GUEST_SMBASE                 = 0x00004828;
	constexpr uint64_t GUEST_IA32_SYSENTER_CS       = 0x0000482A;
	constexpr uint64_t GUEST_IA32_SYSENTER_ESP      = 0x00006824;
	constexpr uint64_t GUEST_IA32_SYSENTER_EIP      = 0x00006826;
	constexpr uint64_t VM_EXIT_MSR_STORE_COUNT      = 0x0000400E;
	constexpr uint64_t VM_EXIT_MSR_LOAD_COUNT       = 0x00004010;
	constexpr uint64_t VM_ENTRY_MSR_LOAD_COUNT      = 0x00004014;
	constexpr uint64_t VM_EXIT_MSR_STORE_ADDR       = 0x00002006;
	constexpr uint64_t VM_EXIT_MSR_LOAD_ADDR        = 0x00002008;
	constexpr uint64_t VM_ENTRY_MSR_LOAD_ADDR       = 0x0000200A;
	constexpr uint64_t GUEST_VMX_PREEMPTION_TIMER   = 0x0000482E;
	constexpr uint64_t VMCS_FIELD_GUEST_EFER_FULL   = 0x00002806;
	constexpr uint64_t MSR_FS_BASE                  = 0xc0000100;
	constexpr uint64_t MSR_GS_BASE                  = 0xc0000101;
	constexpr uint64_t EFER                         = 0xc0000080;

	constexpr uint64_t GUEST_ACTIVITY_STATE              = 0X00004826;
	constexpr uint64_t VMX_PREEMPTION_TIMER_VALUE        = 0x0000482E;
	constexpr uint64_t VMCS_LINK_POINTER                 = 0x00002800;
	constexpr uint64_t GUEST_INTR_STATUS                 = 0x00000810;
	constexpr uint64_t GUEST_PML_INDEX                   = 0x00000812;
	constexpr uint64_t VM_EXIT_REASON                    = 0x00004402;
	constexpr uint64_t VM_INSTRUCTION_ERROR              = 0x00004400;
	constexpr uint64_t EPT_VIOLATION_ADDRESS             = 0x00002400;
	constexpr uint64_t EPT_VIOLATION_FLAGS               = 0x00006400;
	constexpr uint64_t EXIT_QUALIFICATION                = 0x00006400;
	constexpr uint64_t VM_EXIT_INSTRUCTION_LEN           = 0x0000440c;
	constexpr uint64_t VM_ENTRY_INTR_INFO                = 0x00004016;
	constexpr uint64_t VM_ENTRY_EXCEPTION_ERROR_CODE     = 0x00004018;
	constexpr uint64_t VM_ENTRY_INSTRUCTION_LEN          = 0x0000401a;
	constexpr uint64_t IDT_VECTORING_INFO                = 0x00004408;
	constexpr uint64_t IDT_VECTORING_ERROR_CODE          = 0x0000440a;

	constexpr uint64_t DATA_ACCESS_RIGHT = (0x3 | 1 << 4 | 1 << 7);
	constexpr uint64_t CODE_ACCESS_RIGHT = (0x3 | 1 << 4 | 1 << 7 | 1 << 13);
	constexpr uint64_t LDTR_ACCESS_RIGHT = (0x2 | 1 << 7);
	constexpr uint64_t TR_ACCESS_RIGHT   = (0x3 | 1 << 7);

	constexpr uint64_t VMEXIT_EXTERNAL_INTERRUPT           = 1;
	constexpr uint64_t VMEXIT_TRIPLE_FAULT                 = 2;
	constexpr uint64_t VMEXIT_INTERRUPT_WINDOW             = 7;
	constexpr uint64_t VMEXIT_CPUID                        = 10;
	constexpr uint64_t VMEXIT_HLT                          = 12;
	constexpr uint64_t VMEXIT_VMCALL                       = 18;
	constexpr uint64_t VMEXIT_CR_ACCESS                    = 28;
	constexpr uint64_t VMEXIT_IO_INSTRUCTION               = 30;
	constexpr uint64_t VMEXIT_RDMSR                        = 31;
	constexpr uint64_t VMEXIT_WRMSR                        = 32;
	constexpr uint64_t VMEXIT_EPT_VIOLATION                = 48;
	constexpr uint64_t VMEXIT_PREEMPTION_TIMER             = 52;
	constexpr uint64_t VMEXIT_XSETBV                       = 55;

	constexpr uint64_t IA32_VMX_MISC_MSR              = 0x485;
	constexpr uint64_t PIN_BASED_PREEMPTION_TIMER     = 1 << 6;

	constexpr uint64_t VMEXIT_CONTROLS_LONG_MODE      = 1 << 9;
	constexpr uint64_t VMEXIT_CONTROLS_SAVE_IA32_EFER = 1 << 20;
	constexpr uint64_t VMEXIT_CONTROLS_LOAD_IA32_EFER = 1 << 21;
	constexpr uint64_t INTERRUPT_WINDOW_EXITING       = 1 << 2;
	constexpr uint64_t VMEXIT_ON_HLT                  = 1 << 7;
	constexpr uint64_t CR8_LOAD_EXITING               = 1 << 19;
	constexpr uint64_t CR8_STORE_EXITING              = 1 << 20;
	constexpr uint64_t VMEXIT_ON_PIO                  = 1 << 24;
	constexpr uint64_t SECONDARY_CONTROLS_ON          = UINT64_C(1) << 31;
	constexpr uint64_t EPT_ENABLE                     = 1 << 1;
	constexpr uint64_t UNRESTRICTED_GUEST             = 1 << 7;
	constexpr uint64_t VMEXIT_ON_DESCRIPTOR           = 1 << 2;


	struct GuestState {
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
		uint64_t cr2;
	};

	bool vmxon();

	struct Vmcs final : X86VirtualizedCpu {
	private:
		struct CtorToken {};

	public:
		static std::expected<smarter::shared_ptr<Vmcs>, Error> create(
				smarter::shared_ptr<EptSpace> ept) {
			auto ptr = allocate_rcu_shared<Vmcs>(Allocator{}, CtorToken{}, std::move(ept));
			return ptr;
		}

		Vmcs(CtorToken, smarter::shared_ptr<EptSpace> ept);
		~Vmcs();

		Vmcs(const Vmcs& vmcs) = delete;
		Vmcs& operator=(const Vmcs& vmcs) = delete;

		frg::expected<Error, HelVmexitReason> run(const HelVmexitCompletion *completion) override;
		void storeRegs(const HelX86VirtualizationRegs *regs) override;
		void loadRegs(HelX86VirtualizationRegs *res) override;

		void storeBasicRegs(const HelX86VirtualizationBasicRegs *regs) override;
		void loadBasicRegs(HelX86VirtualizationBasicRegs *regs) override;

		bool storeMsrs(const HelX86VirtualizationMsrs *msrs) override;
		void loadMsrs(HelX86VirtualizationMsrs *msrs) override;

		void *region;
		uint8_t* hostFstate;
		uint8_t* guestFstate;

		smarter::shared_ptr<EptSpace> space;

		GuestState state;
		uint64_t saved_host_rsp;

	private:
		// An entry of the VM-entry / VM-exit MSR areas.
		struct MsrEntry {
			uint32_t index;
			uint32_t reserved;
			uint64_t data;
		};

		// Number of MSRs that we switch through the MSR areas (see the constructor).
		static constexpr unsigned numSwitchedMsrs = 5;
		static constexpr unsigned kernelGsBaseEntry = 4;

		void acquire_();
		void release_();

		// The CPU on which the VMCS is active (or null) and whether it was launched there.
		CpuData *activeCpu_ = nullptr;
		bool launched_ = false;
		bool resident_ = false;

		// Access GPRs by their encoding in instructions.
		// The following functions must be called with interrupts disabled while the VMCS is active.
		uint64_t readGpr(unsigned index);
		void writeGpr(unsigned index, uint64_t value);
		// Blocking by STI / MOV SS ends with the instruction that it covers;
		// this needs to be called whenever we skip an instruction.
		void clearInterruptShadow();

		uint64_t guestCr0Value();
		uint64_t guestEfer();
		void setLongModeActive(bool active, uint64_t efer);

		// These return false if an exception is injected.
		bool emulateCrAccess(uint64_t qualification);
		bool writeCr0(uint64_t value);
		bool writeCr4(uint64_t value);
		bool emulateXsetbv();
		// Enables the MSR areas if the guest uses any of the switched MSRs.
		void updateMsrSwitching();

		// Makes sure that cachedRsp_, cachedRip_ and cachedRflags_ are valid.
		void loadBasicRegsCache();

		// The VM-entry controls (without the IA-32e mode guest control).
		uint32_t entryControls_ = 0;

		// One page for the MSR areas: guest values (loaded on entry) followed by
		// host values (loaded on exit). The guest's KERNEL_GS_BASE is stored on exit
		// since SWAPGS changes it without an exit.
		PhysicalAddr msrArea_;
		MsrEntry *guestMsrs_;
		MsrEntry *hostMsrs_;
		// Number of MSRs in the areas (0 or numSwitchedMsrs).
		unsigned msrCount_ = 0;
		uint64_t sysenterCs_ = 0, sysenterEsp_ = 0, sysenterEip_ = 0;
		// Guest EFER.LME; while it is set, the guest may switch to long mode at any time.
		bool guestLme_ = false;
		// Guest CR4.OSXSAVE; CR4 writes that change it cause exits.
		// While it is clear the guest cannot observe XCR0.
		bool guestOsxsave_ = false;
		uint64_t guestXcr0_ = 1;

		// The processor-based controls (without interrupt-window exiting).
		uint32_t procControls_ = 0;
		// The value in the VMCS (to avoid needless VMWRITEs, which are slow when nested).
		uint32_t currentProcControls_ = 0;

		// The VMCS host state is written only when CPU or the FS base changes.
		bool hostSelectorsWritten_ = false;
		bool hostFsBaseWritten_ = false;
		CpuData *hostStateCpu_ = nullptr;
		uint64_t hostFsBase_ = 0;

		// The CR3 of the address space changes when its PCID is rebound.
		uint64_t hostCr3_ = 0;

		// The guest state as in the VMCS, valid from loadRegs()/storeRegs() until the next entry.
		bool regsCacheValid_ = false;
		HelX86VirtualizationRegs regsCache_{};

		// RSP, RIP and RFLAGS as in the VMCS.
		bool basicCacheValid_ = false;
		uint64_t cachedRsp_ = 0, cachedRip_ = 0, cachedRflags_ = 0;

		// cachedRip_ is newer than the VMCS (after a completion skipped an instruction);
		// run() writes it before the next entry.
		bool ripDirty_ = false;

		bool havePreemptionTimer_ = false;
		unsigned preemptionTimerShift_ = 0;
		// The value in the VMCS (UINT32_MAX: effectively disarmed).
		uint32_t preemptionTimerValue_ = UINT32_MAX;
	};
}
