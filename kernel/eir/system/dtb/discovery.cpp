#include <assert.h>
#include <dtb.hpp>
#include <eir-internal/uart/uart.hpp>
#include <frg/manual_box.hpp>
#include <frg/slab.hpp>

#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/dtb/discovery.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <frg/array.hpp>

namespace eir {

size_t findAddressCells(DeviceTreeNode parent) {
	auto maybeProperty = parent.findProperty("#address-cells");
	if (maybeProperty)
		return maybeProperty->asU32();
	return 2;
}

size_t findSizeCells(DeviceTreeNode parent) {
	auto maybeProperty = parent.findProperty("#size-cells");
	if (maybeProperty)
		return maybeProperty->asU32();
	return 1;
}

// UEFI doesn't need this and doesn't provide eirImageFloor either.
#ifndef EIR_UEFI

void discoverMemoryFromDtb() {
	DeviceTree dt{physToVirt<void>(eirDtbPtr)};
	auto rootNode = dt.rootNode();

	eir::infoLogger() << "DTB pointer 0x" << frg::hex_fmt{eirDtbPtr} << frg::endlog;
	eir::infoLogger() << "DTB size: 0x" << frg::hex_fmt{dt.size()} << frg::endlog;

	if constexpr (dumpDtb) {
		// Debugging code to dump the DTB.
		struct Walker {
			void push(DeviceTreeNode node) {
				auto log = eir::infoLogger();
				for (unsigned int i = 0; i < nesting_; ++i)
					log << "  ";
				log << node.name() << frg::endlog;

				++nesting_;

				for (auto prop : node.properties()) {
					auto plog = eir::infoLogger();
					for (unsigned int i = 0; i < nesting_; ++i)
						plog << "  ";
					plog << "Property " << prop.name() << ", " << prop.size() << " bytes"
					     << frg::endlog;
				}
			}

			void pop() { --nesting_; }

		private:
			unsigned int nesting_ = 0;
		} w;

		eir::infoLogger() << "Dumping DTB" << frg::endlog;
		dt.walkTree(w);
	}

	// Find the initrd.
	DeviceTreeNode chosenNode;
	bool foundChosen = false;

	DeviceTreeNode reservedMemoryNode;
	bool foundReservedMemoryNode = false;

	DeviceTreeNode memoryNodes[32];
	size_t nMemoryNodes = 0;

	dt.rootNode().discoverSubnodes(
	    [](DeviceTreeNode &node) {
		    frg::string_view name{node.name()};
		    return name == "memory" || name.starts_with("memory@") || name == "chosen"
		           || name == "reserved-memory";
	    },
	    [&](DeviceTreeNode node) {
		    frg::string_view name{node.name()};

		    if (name == "chosen") {
			    assert(!foundChosen);
			    chosenNode = node;
			    foundChosen = true;
		    } else if (name == "reserved-memory") {
			    assert(!foundReservedMemoryNode);

			    reservedMemoryNode = node;
			    foundReservedMemoryNode = true;
		    } else {
			    if (nMemoryNodes >= 32)
				    eir::panicLogger() << "Cannot deal with > 32 DTB memory nodes" << frg::endlog;
			    memoryNodes[nMemoryNodes++] = node;
		    }
	    }
	);

	if (!foundChosen)
		eir::panicLogger() << "DTB does not contain \"chosen\" node" << frg::endlog;
	if (!nMemoryNodes)
		eir::panicLogger() << "DTB does not contain \"memory\" node" << frg::endlog;

	uint64_t initrdStart = 0;
	uint64_t initrdEnd = 0;
	if (auto p = chosenNode.findProperty("linux,initrd-start")) {
		if (p->size() == 4)
			initrdStart = p->asU32();
		else if (p->size() == 8)
			initrdStart = p->asU64();
		else
			eir::panicLogger() << "Invalid linux,initrd-start size" << frg::endlog;
	}

	if (auto p = chosenNode.findProperty("linux,initrd-end")) {
		if (p->size() == 4)
			initrdEnd = p->asU32();
		else if (p->size() == 8)
			initrdEnd = p->asU64();
		else
			eir::panicLogger() << "Invalid linux,initrd-end size" << frg::endlog;
	}

	assert(initrdStart && initrdEnd);

	eir::infoLogger() << "initrd is between 0x" << frg::hex_fmt(initrdStart) << " and 0x"
	                  << frg::hex_fmt(initrdEnd) << frg::endlog;

	initrd = physToVirt<void>(initrdStart);

	if (cmdline.size() == 0) {
		if (auto p = chosenNode.findProperty("bootargs")) {
			cmdline = static_cast<const char *>(p->data());
		}
	}

	// Determine all reserved memory areas.
	InitialRegion reservedRegions[32];
	size_t nReservedRegions = 0;

	uintptr_t eirStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {eirStart, eirEnd - eirStart};
	reservedRegions[nReservedRegions++] = {initrdStart, initrdEnd - initrdStart};
	reservedRegions[nReservedRegions++] = {eirDtbPtr, dt.size()};

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;

	// Handle entries from the top-level table within the DTB.
	for (auto ent : dt.memoryReservations()) {
		eir::infoLogger() << "    At 0x" << frg::hex_fmt{ent.address} << ", ends at 0x"
		                  << frg::hex_fmt{ent.address + ent.size} << " (0x"
		                  << frg::hex_fmt{ent.size} << " bytes)" << frg::endlog;

		if (nReservedRegions >= frg::array_size(reservedRegions))
			eir::panicLogger() << "Cannot deal with > " << frg::array_size(reservedRegions)
			                   << " DTB memory reservations" << frg::endlog;
		reservedRegions[nReservedRegions++] = {ent.address, ent.size};
	}

	if (foundReservedMemoryNode) {
		auto addressCells = findAddressCells(reservedMemoryNode);
		auto sizeCells = findSizeCells(reservedMemoryNode);

		reservedMemoryNode.discoverSubnodes(
		    [](DeviceTreeNode &) { return true; },
		    [&](DeviceTreeNode childNode) {
			    // Children without "reg" correspond to OS-allocated reserved memory.
			    auto reg = childNode.findProperty("reg");
			    if (!reg) {
				    eir::infoLogger() << "DTB reserved-memory child " << childNode.name()
				                      << " has no \"reg\" property" << frg::endlog;
				    return;
			    }

			    size_t j = 0;
			    while (j < reg->size()) {
				    auto base = reg->asPropArrayEntry(addressCells, j);
				    j += addressCells * 4;

				    auto size = reg->asPropArrayEntry(sizeCells, j);
				    j += sizeCells * 4;

				    eir::infoLogger()
				        << "    " << childNode.name() << ", at 0x" << frg::hex_fmt{base}
				        << ", ends at 0x" << frg::hex_fmt{base + size} << " (0x"
				        << frg::hex_fmt{size} << " bytes)" << frg::endlog;

				    if (nReservedRegions >= frg::array_size(reservedRegions))
					    eir::panicLogger()
					        << "Cannot deal with > " << frg::array_size(reservedRegions)
					        << " DTB memory reservations" << frg::endlog;
				    reservedRegions[nReservedRegions++] = {base, size};
			    }
		    }
		);
	}

	auto addressCells = findAddressCells(rootNode);
	auto sizeCells = findSizeCells(rootNode);

	for (size_t i = 0; i < nMemoryNodes; ++i) {
		auto &node = memoryNodes[i];

		auto reg = node.findProperty("reg");
		if (!reg)
			eir::panicLogger() << "DTB memory node has no \"reg\" property" << frg::endlog;

		size_t j = 0;
		while (j < reg->size()) {
			auto base = reg->asPropArrayEntry(addressCells, j);
			j += addressCells * 4;

			auto size = reg->asPropArrayEntry(sizeCells, j);
			j += sizeCells * 4;

			createInitialRegions({base, size}, {reservedRegions, nReservedRegions});
		}
	}
}

static initgraph::Task discoverMemory{
    &globalInitEngine, "discover-memory", initgraph::Entails{getInitrdAvailableStage()}, [] {
	    // Some protocols like Limine and UEFI provide their own memory map.
	    if (!BootCaps::get().hasMemoryMap)
		    discoverMemoryFromDtb();
    }
};

#endif

constinit uart::AnyUart dtbUart;
constinit frg::manual_box<uart::UartLogHandler> dtbUartLogHandler;

static initgraph::Task discoverOutput{
    &globalInitEngine, "discover-output", initgraph::Entails{getInitrdAvailableStage()}, [] {
	    DeviceTree dt{physToVirt<void>(eirDtbPtr)};

	    auto chosen = dt.findNode("/chosen");
	    if (!chosen)
		    return;

	    auto stdoutPath = chosen->findProperty("stdout-path");
	    if (!stdoutPath)
		    return;

	    auto chosenNode = dt.findNode(*stdoutPath->asString());
	    if (!chosenNode)
		    return;

	    uart::initFromDtb(dtbUart, dt, chosenNode.value());

	    if (!std::holds_alternative<std::monostate>(dtbUart)) {
		    dtbUartLogHandler.initialize(&dtbUart);
		    enableLogHandler(dtbUartLogHandler.get());

		    infoLogger() << "eir: Chosen output path: " << chosenNode->name() << frg::endlog;
		    return;
	    }
    }
};

} // namespace eir
