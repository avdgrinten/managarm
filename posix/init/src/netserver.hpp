#pragma once

#include <async/result.hpp>
#include <expected>
#include <protocols/mbus/client.hpp>
#include <string>

#include "reactor.hpp"

// Returns the mbus entity of netserver. Blocks until netserver shows up on mbus.
std::expected<mbus_ng::EntityId, mbus_ng::Error> findNetserver();

// Binds netserver to the NIC that is identified by a UEFI device path.
async::result<void> bindNetserver(Reactor &reactor, std::string devicePath);
