#pragma once

#include <optional>

#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace vsock {

// Waits for the vsock server, which only exists if the machine has a vsock device.
async::result<void> enumerateServer();

// Returns std::nullopt if no vsock server was found (yet).
std::optional<helix::BorrowedLane> getLane();

} // namespace vsock
