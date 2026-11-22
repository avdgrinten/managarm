#pragma once

#include <protocols/mbus/client.hpp>

// Returns the mbus entity of netserver. Blocks until netserver shows up on mbus.
mbus_ng::EntityId findNetserver();
