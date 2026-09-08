#ifdef __x86_64__
#include <thor-internal/arch/pmc-amd.hpp>
#include <thor-internal/arch/pmc-intel.hpp>
#endif
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

bool wantKernelProfile = false;

namespace {
	frg::manual_box<LogRingBuffer> globalProfileRing;

	initgraph::Task initProfilingSinks{&globalInitEngine, "generic.init-profiling-sinks",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage()},
		[] {
			if(!wantKernelProfile)
				return;

			auto *ring = getGlobalProfileRing();
			if(!ring)
				return;

			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(),
					[] (LogRingBuffer *ring) -> coroutine<void> {
				auto channel = co_await solicitIoChannel("kernel-profile");
				infoLogger() << "thor: Connecting profiling to I/O channel" << frg::endlog;
				co_await dumpRingToChannel(ring, std::move(channel), 2048);
			}(ring));
		}
	};
}

void initializeProfile() {
#ifdef __x86_64__
	if(!wantKernelProfile)
		return;

	if(!(getGlobalCpuFeatures()->profileFlags & CpuFeatures::profileIntelSupported)
			&& !(getGlobalCpuFeatures()->profileFlags & CpuFeatures::profileAmdSupported)) {
		urgentLogger() << "thor: Kernel profiling was requested but"
				" no hardware support is available" << frg::endlog;
		return;
	}

	void *profileMemory = kernelAlloc->allocate(1 << 20);
	globalProfileRing.initialize(reinterpret_cast<uintptr_t>(profileMemory), 1 << 20);

	// Dump the per-CPU profiling data to the global ring buffer.
	auto fiberMain = [] {
		infoLogger() << "thor: Profiling on CPU " << getCpuData()->cpuIndex << frg::endlog;
		getCpuData()->localProfileRing = frg::construct<SingleContextRecordRing>(*kernelAlloc);

		if(getGlobalCpuFeatures()->profileFlags & CpuFeatures::profileIntelSupported) {
			initializeIntelPmc();
			getCpuData()->profileMechanism.store(ProfileMechanism::intelPmc,
					std::memory_order_release);
			setIntelPmc();
		}else{
			assert(getGlobalCpuFeatures()->profileFlags & CpuFeatures::profileAmdSupported);
			getCpuData()->profileMechanism.store(ProfileMechanism::amdPmc,
					std::memory_order_release);
			setAmdPmc();
		}

		uint64_t deqPtr = 0;
		while(true) {
			char buffer[128];
			auto [success, recordPtr, newPtr, size] = getCpuData()->localProfileRing->dequeueAt(
					deqPtr, buffer, 128);
			deqPtr = newPtr;
			if(!success) {
				KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(1'000'000));
				continue;
			}
			assert(size);
			assert(size <= 128);

			globalProfileRing->enqueue(buffer, size);
		}
	};

	for (size_t c = 0; c < getCpuCount(); ++c)
		KernelFiber::run(fiberMain, &localScheduler.getFor(c));
#endif
}

LogRingBuffer *getGlobalProfileRing() {
	if(!globalProfileRing.valid())
		return nullptr;
	return globalProfileRing.get();
}

} // namespace thor
