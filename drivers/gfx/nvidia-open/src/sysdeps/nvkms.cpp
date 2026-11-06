#include <assert.h>
#include <core/clock.hpp>
#include <helix/timer.hpp>
#include <print>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../gfx.hpp"

extern "C" {

#include "nv.h"
#include "nvkms.h"
#include "nvkms-kapi-internal.h"
#include "nvidia-modeset-os-interface.h"

}

extern "C" sem_t nvKmsLock = {};
extern "C" const char *const pNV_KMS_ID = "managarm nvidia driver";

#define STUBBED { assert(!"unimplemented"); }

struct nvkms_per_open {
	void *data;
	enum NvKmsClientType type;
	struct NvKmsKapiDevice *device;
};

void *nvkms_memset(void *ptr, NvU8 c, size_t size) { return memset(ptr, c, size); }

void *nvkms_memcpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }

void *nvkms_memmove(void *dest, const void *src, size_t n) { return memmove(dest, src, n); }

int nvkms_memcmp(const void *s1, const void *s2, size_t n) { return memcmp(s1, s2, n); }

size_t nvkms_strlen(const char *s) { return strlen(s); }

int nvkms_strcmp(const char *s1, const char *s2) { return strcmp(s1, s2); }

char *nvkms_strncpy(char *dest, const char *src, size_t n) { return strncpy(dest, src, n); }

int nvkms_snprintf(char *str, size_t size, const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	int ret = vsnprintf(str, size, format, ap);
	va_end(ap);

	return ret;
}

int nvkms_vsnprintf(char *str, size_t size, const char *format, va_list ap) {
	return vsnprintf(str, size, format, ap);
}

void nvkms_log(const int level, const char *gpuPrefix, const char *msg) {
	const char *levelPrefix;

	switch (level) {
		case NVKMS_LOG_LEVEL_WARN:
			levelPrefix = "WARNING: ";
			break;
		case NVKMS_LOG_LEVEL_ERROR:
			levelPrefix = "ERROR: ";
			break;
		case NVKMS_LOG_LEVEL_INFO:
		default:
			levelPrefix = "";
			break;
	}

	printf("gfx/nvidia-open [%d]: %s%s%s\n", level, levelPrefix, gpuPrefix, msg);
}

void nvkms_call_rm(void *ops) { rm_kernel_rmapi_op(nullptr, ops); }

void nvkms_free(void *ptr, size_t) { free(ptr); }

void *nvkms_alloc(size_t size, NvBool zero) {
	void *ptr = malloc(size);

	if (ptr && zero)
		memset(ptr, 0, size);

	return ptr;
}

void nvkms_usleep(NvU64 usec) { usleep(usec); }

NvU64 nvkms_get_usec(void) {
	auto ts = clk::getRealtime();
	return (ts.tv_sec * 1'000'000) + (ts.tv_nsec / 1'000);
}

int nvkms_copyin(
    void *kptr [[maybe_unused]], NvU64 uaddr [[maybe_unused]], size_t n [[maybe_unused]]
) STUBBED;
int nvkms_copyout(
    NvU64 uaddr [[maybe_unused]], const void *kptr [[maybe_unused]], size_t n [[maybe_unused]]
) STUBBED;

void nvkms_yield(void) {}

void nvkms_dump_stack(void) STUBBED;
NvBool nvkms_syncpt_op(
    enum NvKmsSyncPtOp op [[maybe_unused]], NvKmsSyncPtOpParams *params [[maybe_unused]]
) STUBBED;

NvBool nvkms_test_fail_alloc_core_channel(enum FailAllocCoreChannelMethod) { return NV_FALSE; }

NvBool nvkms_conceal_vrr_caps(void) { return NV_TRUE; }

NvBool nvkms_output_rounding_fix(void) { return NV_TRUE; }

NvBool nvkms_disable_hdmi_frl(void) { return NV_FALSE; }

NvBool nvkms_disable_vrr_memclk_switch(void) STUBBED;

NvBool nvkms_hdmi_deepcolor(void) { return NV_TRUE; }

NvBool nvkms_vblank_sem_control(void) { return NV_TRUE; }

NvBool nvkms_opportunistic_display_sync(void) { return NV_TRUE; }

enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void) {
	return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
}

NvBool nvkms_enable_overlay_layers(void) { return NV_FALSE; }

extern "C" {

struct nvkms_ref_ptr {
	void *pointer;
	std::atomic_size_t refcount = 1;
};
}

struct nvkms_ref_ptr *nvkms_alloc_ref_ptr(void *ptr) { return new nvkms_ref_ptr{ptr}; }

void nvkms_free_ref_ptr(struct nvkms_ref_ptr *ref_ptr) { delete ref_ptr; }

void nvkms_inc_ref(struct nvkms_ref_ptr *ref_ptr) {
	ref_ptr->refcount.fetch_add(1, std::memory_order_acq_rel);
}

void *nvkms_dec_ref(struct nvkms_ref_ptr *ref_ptr) {
	void *ptr = ref_ptr->pointer;
	ref_ptr->refcount.fetch_sub(1, std::memory_order_acq_rel);
	return ptr;
}

namespace {

struct nvkmsTimer {
	nvkms_timer_proc_t *proc;
	void *dataPtr;
	NvU32 dataU32;
	uint64_t tick;
	bool isRefPtr = false;
	bool cancel = false;
};

void *handleTimer(void *arg) {
	auto timer = reinterpret_cast<nvkmsTimer *>(arg);

	async::run(helix::sleepUntil(timer->tick, {}), helix::currentDispatcher);

	if (timer->cancel)
		return nullptr;

	if (timer->isRefPtr) {
		timer->proc(
		    nvkms_dec_ref(reinterpret_cast<nvkms_ref_ptr *>(timer->dataPtr)), timer->dataU32
		);
	} else {
		timer->proc(timer->dataPtr, timer->dataU32);
	}

	return nullptr;
}

void workqueueTimerHandler(void *arg) {
	auto timer = reinterpret_cast<nvkmsTimer *>(arg);

	if (timer->isRefPtr) {
		timer->proc(
		    nvkms_dec_ref(reinterpret_cast<nvkms_ref_ptr *>(timer->dataPtr)), timer->dataU32
		);
	} else {
		timer->proc(timer->dataPtr, timer->dataU32);
	}
}

} // namespace

nvkms_timer_handle_t *
nvkms_alloc_timer(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec) {
	auto now = helix::getClock();

	auto timer = new nvkmsTimer{proc, dataPtr, dataU32, now + (usec * 1'000)};

	if (usec) {
		pthread_t thread;
		pthread_create(&thread, nullptr, handleTimer, timer);
	} else {
		workqueueAdd(workqueueTimerHandler, timer);
	}

	return reinterpret_cast<nvkms_timer_handle_t *>(timer);
}

NvBool nvkms_alloc_timer_with_ref_ptr(
    nvkms_timer_proc_t *proc, struct nvkms_ref_ptr *ref_ptr, NvU32 dataU32, NvU64 usec
) {
	auto now = helix::getClock();

	nvkms_inc_ref(ref_ptr);
	auto timer = new nvkmsTimer{proc, ref_ptr, dataU32, now + (usec * 1'000), true};

	if (usec) {
		pthread_t thread;
		pthread_create(&thread, nullptr, handleTimer, timer);
	} else {
		workqueueAdd(workqueueTimerHandler, timer);
	}

	return true;
}

void nvkms_free_timer(nvkms_timer_handle_t *handle) {
	auto timer = reinterpret_cast<nvkmsTimer *>(handle);
	if (timer)
		timer->cancel = true;
}

void nvkms_event_queue_changed(nvkms_per_open_handle_t *pOpenKernel, NvBool eventsAvailable) {
	auto popen = reinterpret_cast<nvkms_per_open *>(pOpenKernel);

	switch (popen->type) {
		case NVKMS_CLIENT_USER_SPACE:
			assert(!"unimplemented");
		case NVKMS_CLIENT_KERNEL_SPACE: {
			if (eventsAvailable)
				workqueueAdd(workqueue_func_t(nvKmsKapiHandleEventQueueChange), popen->device);

			break;
		}
	}
}

void *nvkms_get_per_open_data(int fd [[maybe_unused]]) STUBBED;

NvBool nvkms_open_gpu(NvU32 gpuId) {
	auto g = GfxDevice::getGpu(gpuId);
	if (!g)
		return NV_FALSE;

	async::run(g->open(), helix::currentDispatcher);

	return NV_TRUE;
}

void nvkms_close_gpu(NvU32 gpuId [[maybe_unused]]) STUBBED;
NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *gpu_info [[maybe_unused]]) STUBBED;

NvBool nvkms_allow_write_combining(void) { return NV_TRUE; }

NvBool nvkms_kernel_supports_syncpts(void) { return NV_FALSE; }

NvBool nvkms_fd_is_nvidia_chardev(int fd [[maybe_unused]]) STUBBED;

namespace {

pthread_rwlock_t pmRwLock = PTHREAD_RWLOCK_INITIALIZER;

void nvkms_read_lock_pm_lock() { pthread_rwlock_rdlock(&pmRwLock); }

void nvkms_read_unlock_pm_lock() { pthread_rwlock_unlock(&pmRwLock); }

bool nvkms_read_trylock_pm_lock() { return pthread_rwlock_tryrdlock(&pmRwLock) != 0; }

struct nvkms_per_open *
nvkms_open_common(enum NvKmsClientType type, struct NvKmsKapiDevice *device, int *status) {
	auto popen = reinterpret_cast<nvkms_per_open *>(nvkms_alloc(sizeof(nvkms_per_open), NV_TRUE));

	if (popen == nullptr) {
		*status = -ENOMEM;
		goto failed;
	}

	popen->type = type;
	popen->device = device;

	sem_wait(&nvKmsLock);

	popen->data = nvKmsOpen(getpid(), type, popen);

	sem_post(&nvKmsLock);

	if (popen->data == nullptr) {
		*status = -EPERM;
		goto failed;
	}

	*status = 0;

	return popen;

failed:
	nvkms_free(popen, sizeof(*popen));

	return nullptr;
}

int nvkms_ioctl_common(struct nvkms_per_open *popen, NvU32 cmd, NvU64 address, const size_t size) {
	NvBool ret = NV_FALSE;

	sem_wait(&nvKmsLock);

	if (popen->data != nullptr)
		ret = nvKmsIoctl(popen->data, cmd, address, size);

	sem_post(&nvKmsLock);

	return ret ? 0 : -EPERM;
}

} // namespace

struct nvkms_per_open *nvkms_open_from_kapi(struct NvKmsKapiDevice *device) {
	int status = 0;

	nvkms_read_lock_pm_lock();
	struct nvkms_per_open *ret = nvkms_open_common(NVKMS_CLIENT_KERNEL_SPACE, device, &status);
	nvkms_read_unlock_pm_lock();

	return ret;
}

void nvkms_close_from_kapi(struct nvkms_per_open *popen [[maybe_unused]]) STUBBED;

NvBool nvkms_ioctl_from_kapi(
    struct nvkms_per_open *popen [[maybe_unused]],
    NvU32 cmd [[maybe_unused]],
    void *params_address [[maybe_unused]],
    const size_t param_size [[maybe_unused]]
) {
	nvkms_read_lock_pm_lock();
	NvBool ret = nvkms_ioctl_common(popen, cmd, (NvU64)(NvUPtr)params_address, param_size) == 0;
	nvkms_read_unlock_pm_lock();

	return ret;
}

NvBool nvkms_ioctl_from_kapi_try_pmlock(
    struct nvkms_per_open *popen, NvU32 cmd, void *params_address, const size_t param_size
) {
	if (nvkms_read_trylock_pm_lock())
		return NV_FALSE;

	auto ret = nvkms_ioctl_common(popen, cmd, (NvU64)(NvUPtr)params_address, param_size) == 0;
	nvkms_read_unlock_pm_lock();

	return ret;
}

nvkms_sema_handle_t *nvkms_sema_alloc(void) {
	sem_t *sem = reinterpret_cast<sem_t *>(malloc(sizeof(sem_t)));
	if (!sem)
		return reinterpret_cast<nvkms_sema_handle_t *>(sem);

	sem_init(sem, 0, 1);
	return reinterpret_cast<nvkms_sema_handle_t *>(sem);
}

void nvkms_sema_free(nvkms_sema_handle_t *s) {
	sem_t *sem = reinterpret_cast<sem_t *>(s);
	sem_destroy(sem);
	free(sem);
}

void nvkms_sema_down(nvkms_sema_handle_t *s) {
	sem_t *sem = reinterpret_cast<sem_t *>(s);
	assert(s);

	sem_wait(sem);
}

void nvkms_sema_up(nvkms_sema_handle_t *s) {
	sem_t *sem = reinterpret_cast<sem_t *>(s);
	assert(s);

	sem_post(sem);
}

struct nvkms_backlight_device *nvkms_register_backlight(
    NvU32 gpu_id [[maybe_unused]],
    NvU32 display_id [[maybe_unused]],
    void *drv_priv [[maybe_unused]],
    NvU32 current_brightness [[maybe_unused]]
) STUBBED;

void nvkms_unregister_backlight(struct nvkms_backlight_device *nvkms_bd [[maybe_unused]]) {}
