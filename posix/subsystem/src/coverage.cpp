#include <future>
#include <thread>

#include <async/oneshot-event.hpp>
#include <async/queue.hpp>
#include <frg/std_compat.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include "coverage.hpp"
#include "process.hpp"

#define PCOV_DISABLED [[clang::no_sanitize("coverage")]]

struct PcovRecording {
	PcovRecording(helix::BorrowedDescriptor buffer, size_t numWords, unsigned int mode)
	: mapping{buffer, 0, numWords * sizeof(uint64_t)},
		words{reinterpret_cast<uint64_t *>(mapping.get())}, numWords{numWords}, mode{mode} { }

	helix::Mapping mapping;
	uint64_t *words;
	size_t numWords;
	unsigned int mode;
};

struct PcovContext {
	// Must be called on the pcov thread.
	void setRecording(std::shared_ptr<PcovRecording> newRecording) {
		auto oldRecording = std::move(recording);
		recording = std::move(newRecording);
		__atomic_store_n(&current, recording.get(), __ATOMIC_RELAXED);
	}

	async::run_queue *runQueue = nullptr;

	// Only accessed on the pcov thread.
	std::shared_ptr<PcovRecording> recording;
	PcovRecording *current = nullptr;
};

namespace {

// --------------------------------------------------------
// Pcov threads
// --------------------------------------------------------

constinit thread_local PcovContext *currentPcovContext = nullptr;

async::queue<PcovContext *, frg::stl_allocator> idlePcovContexts;

void pcovThreadMain(PcovContext *self, std::promise<void> promise) {
	currentPcovContext = self;
	self->runQueue = helix::Dispatcher::global().runQueue();
	promise.set_value();
	async::run_forever(helix::currentDispatcher);
}

PcovContext *acquirePcovContext() {
	if(auto context = idlePcovContexts.maybe_get())
		return *context;

	auto context = new PcovContext;
	std::promise<void> promise;
	auto future = promise.get_future();
	std::thread{pcovThreadMain, context, std::move(promise)}.detach();
	future.wait();
	return context;
}

async::result<void> serveOnPcovThread(PcovContext *context, std::shared_ptr<Process> process,
		async::result<void> serveTree) {
	// Move to the pcov thread.
	co_await async::invocable([] { });

	co_await std::move(serveTree);

	context->setRecording(nullptr);
	auto expected = context;
	process->pcovContext.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
	idlePcovContexts.put(context);
}

async::result<void> doSetRecording(PcovContext *context, Process *process,
		std::shared_ptr<PcovRecording> recording) {
	// Move to the pcov thread.
	co_await async::invocable([] { });

	if(process->pcovContext.load(std::memory_order_relaxed) == context)
		context->setRecording(std::move(recording));
}

async::result<void> setRecording(Process *process, std::shared_ptr<PcovRecording> recording) {
	auto context = process->pcovContext.load(std::memory_order_acquire);
	if(!context)
		co_return;

	auto done = std::make_shared<async::oneshot_event>();
	async::detach_on(context->runQueue, doSetRecording(context, process, std::move(recording)),
			[done] { done->raise(); });

	co_await done->wait();
}

} // anonymous namespace

async::result<void> pcovStartRecording(Process *process, helix::BorrowedDescriptor buffer,
		size_t numWords, unsigned int mode) {
	if(!process->pcovContext.load(std::memory_order_relaxed))
		co_return;

	co_await setRecording(process, std::make_shared<PcovRecording>(buffer, numWords, mode));
}

async::result<void> pcovStopRecording(Process *process) {
	co_await setRecording(process, nullptr);
}

void detachServeOnPcovThread(std::shared_ptr<Process> process, async::result<void> serveTree) {
	auto context = acquirePcovContext();
	process->pcovContext.store(context, std::memory_order_release);
	async::detach_on(context->runQueue,
			serveOnPcovThread(context, std::move(process), std::move(serveTree)));
}

// --------------------------------------------------------
// Coverage hooks
// --------------------------------------------------------

namespace {

inline constexpr uint64_t kcovCmpFlagsConst = 1;
inline constexpr uint64_t kcovCmpFlags8Bit = 0;
inline constexpr uint64_t kcovCmpFlags16Bit = 2;
inline constexpr uint64_t kcovCmpFlags32Bit = 4;
inline constexpr uint64_t kcovCmpFlags64Bit = 6;

PCOV_DISABLED uint64_t *allocateRecord(unsigned int mode, size_t recordWords) {
	auto context = currentPcovContext;
	if(!context)
		return nullptr;

	auto recording = __atomic_load_n(&context->current, __ATOMIC_RELAXED);
	if(!recording || recording->mode != mode)
		return nullptr;

	auto words = recording->words;
	auto maxRecords = (recording->numWords - 1) / recordWords;
	auto count = __atomic_load_n(&words[0], __ATOMIC_RELAXED);
	while(count < maxRecords) {
		if(__atomic_compare_exchange_n(&words[0], &count, count + 1,
				true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return &words[1 + count * recordWords];
	}

	return nullptr;
}

PCOV_DISABLED void recordCmp(uint64_t pc, uint64_t v1, uint64_t v2, uint64_t flags) {
	auto record = allocateRecord(kHelKcovModeCmp, 4);
	if(!record)
		return;

	record[0] = flags;
	record[1] = v1;
	record[2] = v2;
	record[3] = pc;
}

} // anonymous namespace

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_pc() {
	auto record = allocateRecord(kHelKcovModePc, 1);
	if(record)
		record[0] = reinterpret_cast<uint64_t>(__builtin_return_address(0));
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_const_cmp1(uint8_t a1, uint8_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2,
			kcovCmpFlagsConst | kcovCmpFlags8Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_const_cmp2(uint16_t a1, uint16_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2,
			kcovCmpFlagsConst | kcovCmpFlags16Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_const_cmp4(uint32_t a1, uint32_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2,
			kcovCmpFlagsConst | kcovCmpFlags32Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_const_cmp8(uint64_t a1, uint64_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2,
			kcovCmpFlagsConst | kcovCmpFlags64Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_cmp1(uint8_t a1, uint8_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2, kcovCmpFlags8Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_cmp2(uint16_t a1, uint16_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2, kcovCmpFlags16Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_cmp4(uint32_t a1, uint32_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2, kcovCmpFlags32Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_cmp8(uint64_t a1, uint64_t a2) {
	recordCmp(reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2, kcovCmpFlags64Bit);
}

extern "C" PCOV_DISABLED void __sanitizer_cov_trace_switch(uint64_t v, uint64_t *cases) {
	auto pc = reinterpret_cast<uint64_t>(__builtin_return_address(0));
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
		recordCmp(pc, cases[i + 2], v, flags);
}
