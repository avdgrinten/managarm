#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir/interface.hpp>
#include <frg/manual_box.hpp>

namespace eir {

static constinit BootCaps linuxCaps = {
    .hasMemoryMap = false,
};

[[gnu::constructor]] void initBootCaps() {
	linuxCaps.imageStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	linuxCaps.imageEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
}

const BootCaps &BootCaps::get() { return linuxCaps; };

} // namespace eir
