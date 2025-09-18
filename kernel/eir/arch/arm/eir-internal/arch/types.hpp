#pragma once

#include <stdint.h>

namespace eir {

using address_t = uint64_t;
using physaddr_t = uint64_t;

extern uintptr_t eirTTBR[2];

} // namespace eir
