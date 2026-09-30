#include <frg/scope_exit.hpp>
#include <algorithm>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/pci/pci_legacy.hpp>
#include <thor-internal/pci/pcie_ecam.hpp>
#include <thor-internal/main.hpp>

#ifdef __x86_64__
#include <thor-internal/arch/pic.hpp>
#endif

#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>
#include <uacpi/namespace.h>

namespace thor::pci {

struct AcpiPciIrqRouter : PciIrqRouter {
	AcpiPciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_, uacpi_namespace_node *node);

	PciIrqRouter *makeDownstreamRouter(PciBus *bus) override;

private:
	uacpi_namespace_node *acpiNode = nullptr;
};

AcpiPciIrqRouter::AcpiPciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_,
		uacpi_namespace_node *node)
: PciIrqRouter{parent_, associatedBus_}, acpiNode{node} {
	uacpi_pci_routing_table *pci_routes;

	if(!acpiNode) {
		for(int i = 0; i < 4; i++) {
			bridgeIrqs[i] = parent->resolveIrqRoute(
					associatedBus->associatedBridge->slot, static_cast<IrqIndex>(i + 1));
			if(bridgeIrqs[i])
				infoLogger() << "thor:     Bridge IRQ [" << i << "]: "
						<< bridgeIrqs[i]->name() << frg::endlog;
		}

		routingModel = RoutingModel::expansionBridge;
		return;
	}

	auto ret = uacpi_get_pci_routing_table(acpiNode, &pci_routes);
	if(ret == UACPI_STATUS_NOT_FOUND) {
		if(parent) {
			infoLogger() << "thor: There is no _PRT for bus " << associatedBus->busId << ";"
					" assuming expansion bridge routing" << frg::endlog;
			for(int i = 0; i < 4; i++) {
				bridgeIrqs[i] = parent->resolveIrqRoute(
						associatedBus->associatedBridge->slot, static_cast<IrqIndex>(i + 1));
				if(bridgeIrqs[i])
					infoLogger() << "thor:     Bridge IRQ [" << i << "]: "
							<< bridgeIrqs[i]->name() << frg::endlog;
			}

			routingModel = RoutingModel::expansionBridge;
		}else{
			infoLogger() << "thor: There is no _PRT for bus " << associatedBus->busId << ";"
					" giving up IRQ routing of this bus" << frg::endlog;
		}
		return;
	} else if (ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: Failed to evaluate _PRT: "
					<< uacpi_status_to_string(ret) << frg::endlog;

		auto *bus = uacpi_namespace_node_generate_absolute_path(node);
		infoLogger() << "giving up IRQ routing of bus: " << bus << frg::endlog;
		uacpi_kernel_free(const_cast<char*>(bus));
		return;
	}

	// Walk through the PRT and determine the routing.
	for (size_t i = 0; i < pci_routes->num_entries; ++i) {
		auto *entry = &pci_routes->entries[i];

		// These are the defaults
		auto triggering = TriggerMode::level;
		auto polarity = Polarity::low;
		auto gsi = entry->index;
		auto slot = (entry->address >> 16) & 0xFFFF;

		assert((entry->address & 0xFFFF) == 0xFFFF && "TODO: support routing of individual functions");
		if(entry->source) {
			// linux doesn't support this, we should be fine as well
			assert(entry->index == 0 && "TODO: support routing multi-irq links");

			uacpi_resources *resources;
			ret = uacpi_get_current_resources(entry->source, &resources);
			assert(ret == UACPI_STATUS_OK);

			switch (resources->entries[0].type) {
				case UACPI_RESOURCE_TYPE_IRQ: {
					auto *irq = &resources->entries[0].irq;
					assert(irq->num_irqs >= 1);
					gsi = irq->irqs[0];
					if(irq->triggering == UACPI_TRIGGERING_EDGE)
						triggering = TriggerMode::edge;
					if(irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
						polarity = Polarity::high;
					break;
				}
				case UACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
					auto *irq = &resources->entries[0].extended_irq;
					assert(irq->num_irqs >= 1);
					gsi = irq->irqs[0];
					if(irq->triggering == UACPI_TRIGGERING_EDGE)
						triggering = TriggerMode::edge;
					if(irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
						polarity = Polarity::high;
					break;
				}
				default:
					assert(false && "invalid link _CRS type");
			}

			uacpi_free_resources(resources);
		}

		auto index = static_cast<IrqIndex>(entry->pin + 1);

		infoLogger() << "    Route for slot " << slot
				<< ", " << nameOf(index) << ": "
				<< "GSI " << gsi << frg::endlog;

		configureIrq(GlobalIrqInfo{gsi, { triggering, polarity}});
		auto pin = acpi::getGlobalSystemIrq(gsi);
		routingTable.push({slot, index, std::move(pin)});
	}

	uacpi_free_pci_routing_table(pci_routes);
	routingModel = RoutingModel::rootTable;
}

PciIrqRouter *AcpiPciIrqRouter::makeDownstreamRouter(PciBus *bus) {
	uacpi_namespace_node *deviceHandle = nullptr;

	if (acpiNode) {
		struct DeviceSearchCtx {
			uint64_t targetAddr;
			uacpi_namespace_node *outHandle;
		} ctx = {
			.targetAddr = (bus->associatedBridge->slot << 16) | bus->associatedBridge->function,
			.outHandle = {},
		};

		uacpi_namespace_for_each_child(acpiNode,
				[] (uacpi_handle opaque, uacpi_namespace_node *node, uint32_t) {
				auto *ctx = reinterpret_cast<DeviceSearchCtx *>(opaque);
				uint64_t addr = 0;

				auto ret = uacpi_eval_simple_integer(node, "_ADR", &addr);
				if(ret != UACPI_STATUS_OK && ret != UACPI_STATUS_NOT_FOUND)
					return UACPI_ITERATION_DECISION_CONTINUE;

				if(addr == ctx->targetAddr) {
					ctx->outHandle = node;
					return UACPI_ITERATION_DECISION_BREAK;
				}

				return UACPI_ITERATION_DECISION_CONTINUE;
			}, nullptr, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &ctx);

		deviceHandle = ctx.outHandle;
	}

	if (deviceHandle) {
		const char *acpiPath = uacpi_namespace_node_generate_absolute_path(deviceHandle);
		infoLogger() << "            ACPI: " << acpiPath << frg::endlog;
		uacpi_kernel_free(const_cast<char*>(acpiPath));
	}

	return frg::construct<AcpiPciIrqRouter>(*kernelAlloc, this, bus, deviceHandle);
}

static void addLegacyConfigIo() {
	auto io = frg::construct<LegacyPciConfigIo>(*kernelAlloc);
	for (int i = 0; i < 256; i++) {
		addConfigSpaceIo(0, i, io);
	}
}

static initgraph::Task discoverConfigIoSpaces{&globalInitEngine, "pci.discover-acpi-config-io",
	initgraph::Requires{acpi::getTablesDiscoveredStage()},
	initgraph::Entails{getBus0AvailableStage()},
	[] {
		if (!acpiRsdpNote->rsdp)
			return;
		if (debugOptionsNote->useSif)
			return;

		uacpi_table mcfgTbl;

		auto ret = uacpi_table_find_by_signature("MCFG", &mcfgTbl);
		if(ret == UACPI_STATUS_NOT_FOUND) {
			urgentLogger() << "thor: No MCFG table!" << frg::endlog;
			addLegacyConfigIo();
			return;
		}
	    frg::scope_exit finish{[&] { uacpi_table_unref(&mcfgTbl); }};

		if(mcfgTbl.hdr->length < sizeof(acpi_sdt_hdr) + 8 + sizeof(acpi_mcfg_allocation)) {
			urgentLogger() << "thor: MCFG table has no entries, assuming legacy PCI!"
					<< frg::endlog;
			addLegacyConfigIo();
			return;
		}

		size_t nEntries = (mcfgTbl.hdr->length - 44) / 16;
		auto mcfgEntries = (acpi_mcfg_allocation *)((uintptr_t)mcfgTbl.virt_addr + sizeof(acpi_sdt_hdr) + 8);
		for (size_t i = 0; i < nEntries; i++) {
			auto &entry = mcfgEntries[i];
			infoLogger() << "Found config space for segment " << entry.segment
				<< ", buses " << entry.start_bus << "-" << entry.end_bus
				<< ", ECAM MMIO base at " << (void *)entry.address << frg::endlog;

			// MCFG base addresses are relative to bus 0, while EcamPcieConfigIo expects
			// a window that starts at the first bus that it decodes.
			auto io = frg::construct<EcamPcieConfigIo>(*kernelAlloc,
					entry.address + (uintptr_t{entry.start_bus} << 20), entry.segment,
					entry.start_bus, entry.end_bus);

			for (int j = entry.start_bus; j <= entry.end_bus; j++) {
				addConfigSpaceIo(entry.segment, j, io);
			}
		}
	}
};

static initgraph::Task discoverAcpiRootBuses{&globalInitEngine, "pci.discover-acpi-root-buses",
	initgraph::Requires{getTaskingAvailableStage(), acpi::getNsAvailableStage()},
	initgraph::Entails{getRootsDiscoveredStage()},
	[] {
		if (!acpiRsdpNote->rsdp)
			return;
		if (debugOptionsNote->useSif)
			return;

		static const char *pciRootIds[] = {
			acpi::ACPI_HID_PCI,
			acpi::ACPI_HID_PCIE,
			nullptr,
		};

		uacpi_find_devices_at(
			uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB),
			pciRootIds, [](void*, uacpi_namespace_node *node, uint32_t) {
				uint64_t seg = 0, bus = 0, uid = 0;

				uacpi_eval_simple_integer(node, "_SEG", &seg);
				uacpi_eval_simple_integer(node, "_BBN", &bus);
				auto uid_status = uacpi_eval_simple_integer(node, "_UID", &uid);

				infoLogger() << "thor: Found PCI host bridge " << frg::hex_fmt{seg} << ":"
					<< frg::hex_fmt{bus} << frg::endlog;

				PciMsiController *msiController = nullptr;
				#ifdef __x86_64__
					struct ApicMsiController final : PciMsiController {
						smarter::shared_ptr<MsiPin> allocateMsiPin(
								frg::string<KernelAlloc> name) override {
							return allocateApicMsi(std::move(name));
						}
					};

					msiController = frg::construct<ApicMsiController>(*kernelAlloc);
				#endif

				auto rootBus = frg::construct<PciBus>(*kernelAlloc, nullptr, nullptr,
						getConfigIoFor(seg, bus), msiController, seg, bus);
				rootBus->irqRouter = frg::construct<AcpiPciIrqRouter>(*kernelAlloc, nullptr, rootBus, node);
				addRootBus(rootBus);

				rootBus->acpiNode = frg::construct<acpi::AcpiObject>(*kernelAlloc, node, uid);
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), [](PciBus *bus, frg::optional<uint64_t> uid) -> coroutine<void> {
					Properties props;
					if(uid)
						props.decStringProperty("acpi.uid", *uid, 1);
					co_await bus->acpiNode->run(std::move(props));

					co_await bus->mbusPublished.wait();
					Properties updateProps;
					updateProps.stringProperty("unix.subsystem", frg::string<KernelAlloc>{*kernelAlloc, "acpi"});
					updateProps.decStringProperty("acpi.physical_node", bus->mbusId, 1);
					co_await bus->acpiNode->updateProperties(updateProps);
				}(rootBus, (uid_status == UACPI_STATUS_OK) ? frg::optional<uint64_t>(uid) : frg::null_opt));

				return UACPI_ITERATION_DECISION_CONTINUE;
		}, nullptr);
	}
};

} // namespace thor::pci
