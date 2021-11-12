#include <eir-internal/main.hpp>
#include <eir-internal/dtb/discovery.hpp>

// Set by assembly stub.
void *eirDtbPtr;

namespace eir {

static initgraph::Task discoverMemory{&globalInitEngine, "riscv.discover-memory",
	//initgraph::Requires{acpi::getTablesDiscoveredStage()},
	//initgraph::Entails{getBus0AvailableStage()},
	[] {
		discoverMemoryFromDtb(eirDtbPtr);
	}
};

} // namespace eir
