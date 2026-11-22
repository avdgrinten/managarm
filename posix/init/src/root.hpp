#pragma once

#include <async/oneshot-event.hpp>
#include <async/result.hpp>
#include <optional>
#include <string>

#include "reactor.hpp"

// Finds the partition that holds the root file system.
class RootSearch final : public UeventRule {
public:
	// rootSpec is the root= option of the kernel command line.
	RootSearch(Reactor &reactor, const std::string &rootSpec);

	bool handle(const Uevent &uevent) override;

	// Completes once the root partition is found.
	async::result<void> wait();

	// Device node of the root partition; only valid after wait() completes.
	const std::string &devNode() { return devNode_; }

private:
	// Without root=, the first partition with the Managarm root type is used.
	std::optional<std::string> partuuid_;
	std::string description_;
	async::oneshot_event found_;
	std::string devNode_;
};
