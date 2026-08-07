#include <algorithm>
#include <eir/interface.hpp>
#include <frg/manual_box.hpp>
#include <frg/optional.hpp>
#include <frg/scope_exit.hpp>
#include <frg/vector.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/pm-interface.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/pci/pci.hpp>

#ifdef __x86_64__
#include <thor-internal/arch/pic.hpp>
#endif

#include <uacpi/acpi.h>
#include <uacpi/context.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>

namespace thor {
namespace acpi {

// Note: since firmware often provides unaligned MADTs,
//       we just mark all MADT structs as [[gnu::packed]].

struct [[gnu::packed]] MadtHeader {
	uint32_t localApicAddress;
	uint32_t flags;
};

struct [[gnu::packed]] MadtGenericEntry {
	uint8_t type;
	uint8_t length;
};

struct [[gnu::packed]] MadtLocalEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint8_t localApicId;
	uint32_t flags;
};

struct [[gnu::packed]] MadtLocalX2Entry {
	MadtGenericEntry generic;
	uint16_t reserved;
	uint32_t localX2ApicId;
	uint32_t flags;
	uint32_t processorId;
};

namespace local_flags {
static constexpr uint32_t enabled = 1;
};

struct [[gnu::packed]] MadtIoEntry {
	MadtGenericEntry generic;
	uint8_t ioApicId;
	uint8_t reserved;
	uint32_t mmioAddress;
	uint32_t systemIntBase;
};

enum OverrideFlags {
	polarityMask = 0x03,
	polarityDefault = 0x00,
	polarityHigh = 0x01,
	polarityLow = 0x03,

	triggerMask = 0x0C,
	triggerDefault = 0x00,
	triggerEdge = 0x04,
	triggerLevel = 0x0C
};

struct [[gnu::packed]] MadtIntOverrideEntry {
	MadtGenericEntry generic;
	uint8_t bus;
	uint8_t sourceIrq;
	uint32_t systemInt;
	uint16_t flags;
};

struct [[gnu::packed]] MadtLocalNmiEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint16_t flags;
	uint8_t localInt;
};

struct [[gnu::packed]] MadtLocalX2NmiEntry {
	MadtGenericEntry generic;
	uint16_t flags;
	uint32_t processorId;
	uint8_t localInt;
	uint8_t reserved[3];
};

} // namespace acpi
} // namespace thor

// --------------------------------------------------------

namespace thor {

frg::manual_box<frg::optional<GlobalIrqInfo>> isaIrqOverrides[16];

GlobalIrqInfo resolveIsaIrq(unsigned int irq) {
	assert(irq < 16);
	if ((*isaIrqOverrides[irq]))
		return *(*isaIrqOverrides[irq]);
	return GlobalIrqInfo{irq, IrqConfiguration{TriggerMode::edge, Polarity::high}};
}

// Same as resolveIsaIrq(irq) but allows to set more specific configuration options.
GlobalIrqInfo resolveIsaIrq(unsigned int irq, IrqConfiguration desired) {
	if (irq < 16 && *isaIrqOverrides[irq]) {
		assert(desired.compatible((*isaIrqOverrides[irq])->configuration));
		return *(*isaIrqOverrides[irq]);
	}
	return GlobalIrqInfo{irq, desired};
}

// --------------------------------------------------------

void configureIrq(GlobalIrqInfo info) {
	auto pin = acpi::getGlobalSystemIrq(info.gsi);
	assert(pin);
	pin->configure(info.configuration);
}

} // namespace thor

// --------------------------------------------------------

namespace thor {
namespace acpi {

void bootOtherProcessors() {
#ifdef __x86_64__
	uacpi_table madtTbl;

	auto ret = uacpi_table_find_by_signature("APIC", &madtTbl);
	assert(ret == UACPI_STATUS_OK);
	frg::scope_exit finish{[&] { uacpi_table_unref(&madtTbl); }};
	auto *madt = madtTbl.hdr;

	infoLogger() << "thor: Booting APs." << frg::endlog;

	// The BSP is already running and occupies CPU index 0; skip its MADT entry.
	auto bspApicId = getLocalApicId();

	size_t apCpuIndex = 1;
	size_t offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
	while (offset < madt->length) {
		auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
		switch (generic->type) {
			case ACPI_MADT_ENTRY_TYPE_LAPIC: {
				auto entry = (MadtLocalEntry *)generic;

				if ((entry->flags & local_flags::enabled) && entry->localApicId != bspApicId) {
					if (apCpuIndex < cpuConfigNote->effectiveCpus)
						bootSecondary(entry->localApicId, apCpuIndex);
					apCpuIndex++;
				}
			} break;
			case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC: {
				auto entry = (MadtLocalX2Entry *)generic;

				if ((entry->flags & local_flags::enabled) && entry->localX2ApicId != bspApicId) {
					if (apCpuIndex < cpuConfigNote->effectiveCpus)
						bootSecondary(entry->localX2ApicId, apCpuIndex);
					apCpuIndex++;
				}
			} break;
			default:
				// Do nothing.
		}
		offset += generic->length;
	}

	if (apCpuIndex != cpuConfigNote->totalCpus)
		panicLogger() << "thor: Found " << apCpuIndex << " CPUs, but Eir detected "
		              << cpuConfigNote->totalCpus << frg::endlog;
#endif
}

// --------------------------------------------------------

void dumpMadt() {
	uacpi_table madtTbl;

	auto ret = uacpi_table_find_by_signature("APIC", &madtTbl);
	assert(ret == UACPI_STATUS_OK);
	frg::scope_exit finish{[&] { uacpi_table_unref(&madtTbl); }};
	auto *madt = madtTbl.hdr;

	infoLogger() << "thor: Dumping MADT" << frg::endlog;

	size_t offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
	while (offset < madt->length) {
		MadtGenericEntry generic;
		auto genericPtr = (void *)(madtTbl.virt_addr + offset);
		memcpy(&generic, genericPtr, sizeof(generic));
		// auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
		switch (generic.type) {
			case ACPI_MADT_ENTRY_TYPE_LAPIC: {
				MadtLocalEntry entry;
				memcpy(&entry, genericPtr, sizeof(MadtLocalEntry));
				infoLogger() << "    Local APIC id: " << (int)entry.localApicId
				             << ((entry.flags & local_flags::enabled) ? "" : " (disabled)")
				             << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
				MadtIoEntry entry;
				memcpy(&entry, genericPtr, sizeof(MadtIoEntry));
				infoLogger() << "    I/O APIC id: " << (int)entry.ioApicId
				             << ", system interrupt base: " << (int)entry.systemIntBase
				             << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: { // interrupt source override
				MadtIntOverrideEntry entry;
				memcpy(&entry, genericPtr, sizeof(MadtIntOverrideEntry));

				const char *bus, *polarity, *trigger;
				if (entry.bus == 0) {
					bus = "ISA";
				} else {
					panicLogger() << "Unexpected bus in MADT interrupt override" << frg::endlog;
				}

				if ((entry.flags & OverrideFlags::polarityMask) == OverrideFlags::polarityDefault) {
					polarity = "default";
				} else if ((entry.flags & OverrideFlags::polarityMask)
				           == OverrideFlags::polarityHigh) {
					polarity = "high";
				} else if ((entry.flags & OverrideFlags::polarityMask)
				           == OverrideFlags::polarityLow) {
					polarity = "low";
				} else {
					panicLogger() << "Unexpected polarity in MADT interrupt override"
					              << frg::endlog;
				}

				if ((entry.flags & OverrideFlags::triggerMask) == OverrideFlags::triggerDefault) {
					trigger = "default";
				} else if ((entry.flags & OverrideFlags::triggerMask)
				           == OverrideFlags::triggerEdge) {
					trigger = "edge";
				} else if ((entry.flags & OverrideFlags::triggerMask)
				           == OverrideFlags::triggerLevel) {
					trigger = "level";
				} else {
					panicLogger() << "Unexpected trigger mode in MADT interrupt override"
					              << frg::endlog;
				}

				infoLogger() << "    Int override: " << bus << " IRQ " << (int)entry.sourceIrq
				             << " is mapped to GSI " << entry.systemInt
				             << " (Polarity: " << polarity << ", trigger mode: " << trigger << ")"
				             << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_LAPIC_NMI: {
				MadtLocalNmiEntry entry;
				memcpy(&entry, genericPtr, sizeof(MadtLocalNmiEntry));
				infoLogger() << "    Local APIC NMI: processor " << (int)entry.processorId
				             << ", lint: " << (int)entry.localInt << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC: {
				MadtLocalX2Entry entry;
				memcpy(&entry, genericPtr, sizeof(MadtLocalX2Entry));
				infoLogger() << "    Local x2APIC id: " << entry.localX2ApicId
				             << ((entry.flags & local_flags::enabled) ? "" : " (disabled)")
				             << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC_NMI: {
				MadtLocalX2NmiEntry entry;
				memcpy(&entry, genericPtr, sizeof(MadtLocalX2NmiEntry));
				infoLogger() << "    Local x2APIC NMI: processor " << (int)entry.processorId
				             << ", lint: " << (int)entry.localInt << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_RINTC: {
				acpi_madt_rintc entry;
				memcpy(&entry, genericPtr, sizeof(acpi_madt_rintc));
				infoLogger() << "    HART " << entry.hart_id << ", Controller ID "
				             << entry.ext_intc_id << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_PLIC: {
				acpi_madt_plic entry;
				memcpy(&entry, genericPtr, sizeof(acpi_madt_plic));
				infoLogger() << "    PLIC " << entry.id << ", GSI Base at "
				             << frg::hex_fmt{entry.gsi_base} << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_GICC: {
				acpi_madt_gicc entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				infoLogger() << "    GICC " << entry.interface_number << ", MPIDR "
				             << frg::hex_fmt{entry.mpidr}
				             << ((entry.flags & ACPI_GICC_ENABLED) ? "" : " (disabled)")
				             << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_GICD: {
				acpi_madt_gicd entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				infoLogger() << "    GICD " << entry.id << ", address "
				             << frg::hex_fmt{entry.address} << ", version "
				             << static_cast<int>(entry.gic_version) << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_GICR: {
				acpi_madt_gicr entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				infoLogger() << "    GICR address " << frg::hex_fmt{entry.address} << ", length "
				             << entry.length << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_GIC_MSI_FRAME: {
				acpi_madt_gic_msi_frame entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				infoLogger() << "    GIC MSI frame " << entry.id << ", address "
				             << frg::hex_fmt{entry.address} << ", SPIs " << entry.spi_base << ".."
				             << entry.spi_base + entry.spi_count << frg::endlog;
			} break;
			case ACPI_MADT_ENTRY_TYPE_GIC_ITS: {
				acpi_madt_gic_its entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				infoLogger() << "    GIC ITS " << entry.id << ", address "
				             << frg::hex_fmt{entry.address} << frg::endlog;
			} break;
			default: {
				infoLogger() << "    Unexpected MADT entry of type " << generic.type << frg::endlog;
			}
		}
		offset += generic.length;
	}
}

initgraph::Stage *getTablesDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.tables-discovered"};
	return &s;
}

initgraph::Stage *getNsAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.ns-available"};
	return &s;
}

frg::manual_box<frg::hash_map<uint32_t, IrqPin *, frg::hash<uint32_t>, KernelAlloc>>
    globalSystemIrqs;

IrqPin *getGlobalSystemIrq(size_t n) {
	auto irq = globalSystemIrqs->get(static_cast<uint32_t>(n));
	return irq ? *irq : nullptr;
}

void setGlobalSystemIrq(size_t n, IrqPin *pin) { globalSystemIrqs->insert(n, pin); }

namespace {

alignas(alignof(uintptr_t)) constinit std::array<char, 4096> earlyTableBuffer{};

} // namespace

static initgraph::Task initTablesTask{
    &globalInitEngine, "acpi.init-tables", initgraph::Entails{getTablesDiscoveredStage()}, [] {
	    if (!acpiRsdpNote->rsdp)
		    return;

	    auto ret = uacpi_setup_early_table_access(earlyTableBuffer.data(), earlyTableBuffer.size());
	    assert(ret == UACPI_STATUS_OK);

	    globalSystemIrqs.initialize(frg::hash<uint32_t>{}, *kernelAlloc);
    }
};

static initgraph::Task discoverIoApicsTask{
    &globalInitEngine,
    "acpi.discover-ioapics",
    initgraph::Requires{getTablesDiscoveredStage(), getFibersAvailableStage()},
    initgraph::Entails{getTaskingAvailableStage()},
    [] {
	    if (!acpiRsdpNote->rsdp)
		    return;

	    dumpMadt();

	    uacpi_table madtTbl;

	    auto ret = uacpi_table_find_by_signature("APIC", &madtTbl);
	    assert(ret == UACPI_STATUS_OK);
	    frg::scope_exit finish{[&] { uacpi_table_unref(&madtTbl); }};
	    auto *madt = madtTbl.hdr;

	    // Configure all interrupt controllers.
	    // TODO: This should be done during thor's initialization in order to avoid races.
	    infoLogger() << "thor: Configuring I/O APICs." << frg::endlog;

	    size_t offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
	    while (offset < madt->length) {
		    auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
		    if (generic->type == ACPI_MADT_ENTRY_TYPE_IOAPIC) {
#ifdef __x86_64__
			    // TODO: Move this to x86_64 code.
			    auto entry = (MadtIoEntry *)generic;
			    setupIoApic(entry->ioApicId, entry->systemIntBase, entry->mmioAddress);
#endif
		    }
		    offset += generic->length;
	    }

	    // Determine IRQ override configuration.
	    for (int i = 0; i < 16; i++)
		    isaIrqOverrides[i].initialize();

	    offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
	    while (offset < madt->length) {
		    auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
		    if (generic->type == 2) { // interrupt source override
			    auto entry = (MadtIntOverrideEntry *)generic;

			    // ACPI defines only ISA IRQ overrides.
			    assert(entry->bus == 0);
			    assert(entry->sourceIrq < 16);

			    GlobalIrqInfo line;
			    line.gsi = entry->systemInt;

			    auto trigger = entry->flags & OverrideFlags::triggerMask;
			    auto polarity = entry->flags & OverrideFlags::polarityMask;
			    if (trigger == OverrideFlags::triggerDefault
			        && polarity == OverrideFlags::polarityDefault) {
				    line.configuration.trigger = TriggerMode::edge;
				    line.configuration.polarity = Polarity::high;
			    } else {
				    assert(trigger != OverrideFlags::triggerDefault);
				    assert(polarity != OverrideFlags::polarityDefault);

				    switch (trigger) {
					    case OverrideFlags::triggerEdge:
						    line.configuration.trigger = TriggerMode::edge;
						    break;
					    case OverrideFlags::triggerLevel:
						    line.configuration.trigger = TriggerMode::level;
						    break;
					    default:
						    panicLogger() << "Illegal IRQ trigger mode in MADT" << frg::endlog;
				    }

				    switch (polarity) {
					    case OverrideFlags::polarityHigh:
						    line.configuration.polarity = Polarity::high;
						    break;
					    case OverrideFlags::polarityLow:
						    line.configuration.polarity = Polarity::low;
						    break;
					    default:
						    panicLogger() << "Illegal IRQ polarity in MADT" << frg::endlog;
				    }
			    }

			    assert(!(*isaIrqOverrides[entry->sourceIrq]));
			    *isaIrqOverrides[entry->sourceIrq] = line;
		    }
		    offset += generic->length;
	    }
    }
};

#ifdef __x86_64__
static void configureIsaIrqs() {
	// Configure the ISA IRQs.
	// TODO: This is a hack. We assume that HPET will use legacy replacement.
	infoLogger() << "thor: Configuring ISA IRQs." << frg::endlog;
	configureIrq(resolveIsaIrq(0));
	configureIrq(resolveIsaIrq(1));
	configureIrq(resolveIsaIrq(4));
	configureIrq(resolveIsaIrq(12));
	configureIrq(resolveIsaIrq(14));
}
#endif

static initgraph::Task loadAcpiNamespaceTask{
    &globalInitEngine,
    "acpi.load-namespace",
    initgraph::Requires{getTaskingAvailableStage(), pci::getBus0AvailableStage()},
    initgraph::Entails{getNsAvailableStage()},
    [] {
	    if (!acpiRsdpNote->rsdp)
		    return;
	    if (debugOptionsNote->useSif) {
		    infoLogger() << "thor: Skipping ACPI namespace init (sif)" << frg::endlog;
		    // The ISA IRQ setup only depends on the MADT, which thor still owns.
#ifdef __x86_64__
		    configureIsaIrqs();
#endif
		    return;
	    }

	    initGlue();

	    auto ret = uacpi_initialize(0);
	    assert(ret == UACPI_STATUS_OK);

	    ret = uacpi_namespace_load();
	    assert(ret == UACPI_STATUS_OK);

#if defined(__aarch64__)
	    ret = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_GIC);
#elif defined(__riscv)
	    ret = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_RINTC);
#else
	    ret = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
#endif
	    assert(ret == UACPI_STATUS_OK);

	    initEc();

	    ret = uacpi_namespace_initialize();
	    assert(ret == UACPI_STATUS_OK);

#ifdef __x86_64__
	    configureIsaIrqs();
#endif

	    initEvents();
    }
};

static initgraph::Task bootApsTask{
    &globalInitEngine, "acpi.boot-aps", initgraph::Requires{&loadAcpiNamespaceTask}, [] {
	    if (!acpiRsdpNote->rsdp)
		    return;

	    bootOtherProcessors();
    }
};

static initgraph::Task initPmInterfaceTask{
    &globalInitEngine, "acpi.init-pm-interface", initgraph::Requires{&loadAcpiNamespaceTask}, [] {
	    if (!acpiRsdpNote->rsdp)
		    return;
	    if (debugOptionsNote->useSif)
		    return;

	    initializePmInterface();
    }
};

} // namespace acpi
} // namespace thor
