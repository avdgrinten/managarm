#pragma once

#include <stdint.h>

namespace {

constexpr bool logIRQs = false;
constexpr bool logTXDescriptor = false;
constexpr bool logRXDescriptor = false;
constexpr bool logDriverStart = false;
// Dumps PHY, PCIe link and chip configuration state and logs indirect register access timeouts.
constexpr bool logHardwareState = true;
// Period of the PHY state and tally counter monitor in nanoseconds; 0 disables it.
constexpr uint64_t monitorIntervalNs = 5'000'000'000;

}
