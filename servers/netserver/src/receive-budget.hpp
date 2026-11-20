#pragma once

#include <cassert>
#include <cstddef>

// Bounds the bytes that a socket queues for reception, like SO_RCVBUF on Linux.
struct ReceiveBudget {
	// Linux' default SO_RCVBUF (net.core.rmem_default).
	static constexpr size_t defaultLimit = 212992;
	// Charged per queued packet on top of its data, like the skb overhead in Linux' truesize.
	static constexpr size_t entryOverhead = 256;

	// Like Linux, admits packets until the budget is exhausted, so a large packet still fits.
	bool tryCharge(size_t size) {
		if(used_ >= limit_)
			return false;
		used_ += size + entryOverhead;
		return true;
	}

	void release(size_t size) {
		assert(used_ >= size + entryOverhead);
		used_ -= size + entryOverhead;
	}

private:
	size_t limit_ = defaultLimit;
	size_t used_ = 0;
};
