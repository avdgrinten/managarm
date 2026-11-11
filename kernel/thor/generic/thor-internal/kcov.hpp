#pragma once

#include <expected>

#include <thor-internal/error.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/rcu-base.hpp>

#define THOR_KCOV_DISABLED [[clang::no_sanitize("coverage")]]

#ifdef THOR_HAS_KCOV

namespace thor {

struct Thread;

// Same as the kHelKcovMode* constants.
inline constexpr unsigned int kcovModeDisabled = 0;
inline constexpr unsigned int kcovModePc = 1;
inline constexpr unsigned int kcovModeCmp = 2;

struct KcovBuffer final : RcuProtected {
private:
	struct CtorToken {};

public:
	static constexpr size_t maxSize = size_t{64} << 20;

	static std::expected<smarter::shared_ptr<KcovBuffer>, Error> create(size_t length);

	static Error attach(smarter::shared_ptr<KcovBuffer> buffer, Thread *thread, unsigned int mode);
	static Error detach(Thread *thread);
	// Called by a thread when it exits.
	static void exitCurrent(Thread *thread);

	KcovBuffer(CtorToken, smarter::shared_ptr<ImmediateMemory> memory, size_t numWords);
	KcovBuffer(const KcovBuffer &) = delete;

	KcovBuffer &operator= (const KcovBuffer &) = delete;

	smarter::shared_ptr<MemoryView> getMemory() {
		return _memory;
	}

	void recordPc(uint64_t pc);
	void recordCmp(uint64_t pc, uint64_t v1, uint64_t v2, uint64_t flags);

	unsigned int mode{kcovModeDisabled};

private:
	size_t _allocateRecord(size_t recordWords);

	smarter::shared_ptr<ImmediateMemory> _memory;
	ImmediateWindow _window;
	// Cached _window pointer and size as to not call other functions in
	// instrumentation callbacks.
	uint64_t *_area;
	size_t _numWords;
};

void enableKcovHooks();

} // namespace thor

#endif // THOR_HAS_KCOV
