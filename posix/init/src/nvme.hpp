#pragma once

#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <string>

#include "reactor.hpp"

// Launches block-nvme for the NVMe controllers that show up on the PCI bus, until it is cancelled.
async::result<void> bindNvmePciDevices(Reactor &reactor, async::cancellation_token ct);

// Starts NVMe-oF once the network link to the NVMe-oF server is up.
// netserver is the mbus ID of netserver, server is the address of the NVMe-oF server.
async::result<void> startNvmeOf(Reactor &reactor, std::string netserver, std::string server);
