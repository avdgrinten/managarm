#pragma once

#include <async/result.hpp>
#include <string>

#include "reactor.hpp"

// Finds the partition that holds the root file system and returns its device node.
// rootSpec is the root= option of the kernel command line.
async::result<std::string> findRoot(Reactor &reactor, std::string rootSpec);
