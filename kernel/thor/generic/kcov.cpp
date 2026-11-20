#include <frg/bitops.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kcov.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

std::expected<smarter::shared_ptr<KcovBuffer>, Error> KcovBuffer::create(size_t length) {
	if(!length || length % sizeof(uint64_t) || length > maxSize)
		return std::unexpected{Error::illegalArgs};

	auto memoryOutcome = ImmediateMemory::create(frg::align_up(length, kPageSize));
	if(!memoryOutcome)
		return std::unexpected{memoryOutcome.error()};

	return allocate_rcu_shared<KcovBuffer>(*kernelAlloc, CtorToken{},
			std::move(*memoryOutcome), length / sizeof(uint64_t));
}

KcovBuffer::KcovBuffer(CtorToken, smarter::shared_ptr<ImmediateMemory> memory, size_t numWords)
: _memory{std::move(memory)}, _window{_memory},
		_area{reinterpret_cast<uint64_t *>(_window.bytes_data())},
		_numWords{numWords} { }

// --------------------------------------------------------
// Buffer handling
// --------------------------------------------------------

Error KcovBuffer::attach(smarter::shared_ptr<KcovBuffer> buffer, Thread *thread, unsigned int mode) {
	if(mode != kcovModePc && mode != kcovModeCmp)
		return Error::illegalArgs;

	auto kcov = buffer.get();
	unsigned int expected = kcovModeDisabled;
	if(!__atomic_compare_exchange_n(&kcov->mode, &expected, mode,
			false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		return Error::illegalState;

	StatelessIrqLock irqLock;
	auto lock = frg::guard(&thread->kcovMutex);

	if(thread->kcovExited || thread->kcov) {
		__atomic_store_n(&kcov->mode, kcovModeDisabled, __ATOMIC_RELAXED);
		return thread->kcovExited ? Error::threadExited : Error::illegalState;
	}

	thread->kcovRef = std::move(buffer);
	__atomic_store_n(&thread->kcov, kcov, __ATOMIC_RELEASE);
	return Error::success;
}

Error KcovBuffer::detach(Thread *thread) {
	smarter::shared_ptr<KcovBuffer> ref;
	{
		StatelessIrqLock irqLock;
		auto lock = frg::guard(&thread->kcovMutex);

		auto kcov = thread->kcov;
		if(!kcov)
			return Error::illegalState;

		__atomic_store_n(&thread->kcov, nullptr, __ATOMIC_RELAXED);
		__atomic_store_n(&kcov->mode, kcovModeDisabled, __ATOMIC_RELAXED);
		ref = std::move(thread->kcovRef);
	}
	return Error::success;
}

void KcovBuffer::exitCurrent(Thread *thread) {
	smarter::shared_ptr<KcovBuffer> ref;
	{
		StatelessIrqLock irqLock;
		auto lock = frg::guard(&thread->kcovMutex);

		thread->kcovExited = true;
		if(auto kcov = thread->kcov; kcov) {
			__atomic_store_n(&thread->kcov, nullptr, __ATOMIC_RELAXED);
			__atomic_store_n(&kcov->mode, kcovModeDisabled, __ATOMIC_RELAXED);
		}
		ref = std::move(thread->kcovRef);
	}
}

// --------------------------------------------------------
// Coverage hooks
// --------------------------------------------------------

namespace {

bool hooksEnabled = false;

inline constexpr uint64_t kcovCmpFlagsConst = 1;
inline constexpr uint64_t kcovCmpFlags8Bit = 0;
inline constexpr uint64_t kcovCmpFlags16Bit = 2;
inline constexpr uint64_t kcovCmpFlags32Bit = 4;
inline constexpr uint64_t kcovCmpFlags64Bit = 6;

// We need a stripped down version of StatelessIrqLock with coverage disabled.
struct KcovIrqGuard {
	THOR_KCOV_DISABLED KcovIrqGuard()
	: _intsEnabled{intsAreEnabled()} {
		if(_intsEnabled)
			disableInts();
	}

	KcovIrqGuard(const KcovIrqGuard &) = delete;

	THOR_KCOV_DISABLED ~KcovIrqGuard() {
		if(_intsEnabled)
			enableInts();
	}

	KcovIrqGuard &operator= (const KcovIrqGuard &) = delete;

private:
	bool _intsEnabled;
};

// Purposefully racy to prevent uncovered threads from needing to disable interrupts.
THOR_KCOV_DISABLED bool mightHaveKcov() {
	if(!__atomic_load_n(&hooksEnabled, __ATOMIC_RELAXED))
		return false;
	if(contextIpl() > ipl::exceptional)
		return false;
	auto thread = getCpuData()->activeThread;
	return thread && __atomic_load_n(&thread->kcov, __ATOMIC_RELAXED);
}

// Must be called with interrupts disabled
THOR_KCOV_DISABLED KcovBuffer *currentKcov(unsigned int mode) {
	auto thread = getCpuData()->activeThread;
	if(!thread)
		return nullptr;
	auto kcov = __atomic_load_n(&thread->kcov, __ATOMIC_ACQUIRE);
	if(!kcov || __atomic_load_n(&kcov->mode, __ATOMIC_RELAXED) != mode)
		return nullptr;
	return kcov;
}

THOR_KCOV_DISABLED void publishPc(uint64_t pc) {
	if(!mightHaveKcov())
		return;

	KcovIrqGuard irqGuard;
	auto kcov = currentKcov(kcovModePc);
	if(!kcov)
		return;
	kcov->recordPc(pc);
}

THOR_KCOV_DISABLED void publishCmp(uint64_t pc, uint64_t v1, uint64_t v2, uint64_t flags) {
	if(!mightHaveKcov())
		return;

	KcovIrqGuard irqGuard;
	auto kcov = currentKcov(kcovModeCmp);
	if(!kcov)
		return;
	kcov->recordCmp(pc, v1, v2, flags);
}

} // anonymous namespace

void enableKcovHooks() {
	__atomic_store_n(&hooksEnabled, true, __ATOMIC_RELAXED);
}

THOR_KCOV_DISABLED size_t KcovBuffer::_allocateRecord(size_t recordWords) {
	auto maxRecords = (_numWords - 1) / recordWords;
	auto count = __atomic_load_n(&_area[0], __ATOMIC_RELAXED);
	while(count < maxRecords) {
		if(__atomic_compare_exchange_n(&_area[0], &count, count + 1,
				true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return 1 + count * recordWords;
	}
	return 0;
}

THOR_KCOV_DISABLED void KcovBuffer::recordPc(uint64_t pc) {
	auto index = _allocateRecord(1);
	if(!index)
		return;
	_area[index] = pc;
}

THOR_KCOV_DISABLED void KcovBuffer::recordCmp(uint64_t pc, uint64_t v1, uint64_t v2, uint64_t flags) {
	auto index = _allocateRecord(4);
	if(!index)
		return;
	_area[index] = flags;
	_area[index + 1] = v1;
	_area[index + 2] = v2;
	_area[index + 3] = pc;
}

} // namespace thor

using namespace thor;

// The coverage hooks use noinline so that they can properly see the return PC with LTO

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_pc() {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishPc(pc);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp1(uint8_t a1, uint8_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags8Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp2(uint16_t a1, uint16_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags16Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp4(uint32_t a1, uint32_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags32Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp8(uint64_t a1, uint64_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags64Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp1(uint8_t a1, uint8_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags8Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp2(uint16_t a1, uint16_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags16Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp4(uint32_t a1, uint32_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags32Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp8(uint64_t a1, uint64_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags64Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_switch(uint64_t v, uint64_t *cases) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	uint64_t flags = kcovCmpFlagsConst;
	switch(cases[1]) {
	case 8:
		flags |= kcovCmpFlags8Bit;
		break;
	case 16:
		flags |= kcovCmpFlags16Bit;
		break;
	case 32:
		flags |= kcovCmpFlags32Bit;
		break;
	case 64:
		flags |= kcovCmpFlags64Bit;
		break;
	default:
		return;
	}

	for(uint64_t i = 0; i < cases[0]; ++i)
		publishCmp(pc, cases[i + 2], v, flags);
}
