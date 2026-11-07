#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

struct join_test_data {
	pthread_t mainThread;
	int efd;
};

static void *thread_A_func(void *arg) {
	auto data = static_cast<join_test_data *>(arg);

	// *data dies as soon as we release the main thread: pthread_exit() unwinds the frame
	// that holds it and the libc exit path then reuses that stack.
	auto mainThread = data->mainThread;
	int efd = data->efd;

	uint64_t val = 1;
	ssize_t bytes_written = write(efd, &val, sizeof(uint64_t));
	assert(bytes_written == sizeof(uint64_t));

	void *code;
	int ret = pthread_join(mainThread, &code);
	assert(ret == 0);
	fprintf(stderr, "main thread exited with 0x%lx\n", (uintptr_t) code);
	assert((uintptr_t) code == 0xDEAD);

	exit(0);
}

DEFINE_TEST(pthread_join_on_exiting_thread, ([] {
	int child = fork();

	if (!child) {
		int efd = eventfd(0, 0);
		assert(efd >= 0);

		join_test_data data;
		data.efd = efd;
		data.mainThread = pthread_self();

		pthread_t thread_A;
		int ret = pthread_create(&thread_A, nullptr, thread_A_func, &data);
		assert(ret == 0);

		uint64_t val;
		ssize_t read_len = read(efd, &val, sizeof(val));
		fprintf(stderr, "read returns %ld\n", read_len);
		assert(read_len == 8);

		pthread_exit((void *) 0xDEAD);
	}

	int status;
	auto ret = waitpid(child, &status, 0);
	assert(ret == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}));

struct blocked_thread_data {
	int efd;
	int readFd;
};

static void *block_in_epoll_wait(void *arg) {
	auto data = static_cast<blocked_thread_data *>(arg);

	int epfd = epoll_create1(0);
	assert(epfd >= 0);
	epoll_event evt{};
	evt.events = EPOLLIN;
	int e = epoll_ctl(epfd, EPOLL_CTL_ADD, data->readFd, &evt);
	assert(!e);

	uint64_t val = 1;
	ssize_t written = write(data->efd, &val, sizeof(val));
	assert(written == sizeof(val));

	epoll_wait(epfd, &evt, 1, -1);
	assert(!"epoll_wait() returned");
	return nullptr;
}

static void *block_in_poll(void *arg) {
	auto data = static_cast<blocked_thread_data *>(arg);

	uint64_t val = 1;
	ssize_t written = write(data->efd, &val, sizeof(val));
	assert(written == sizeof(val));

	pollfd pfd{.fd = data->readFd, .events = POLLIN, .revents = 0};
	poll(&pfd, 1, -1);
	assert(!"poll() returned");
	return nullptr;
}

// The process must exit even though its second thread still blocks in blocker().
static void exit_with_blocked_thread(void *(*blocker)(void *)) {
	int child = fork();
	assert(child >= 0);

	if (!child) {
		int fds[2];
		int e = pipe(fds);
		assert(!e);

		blocked_thread_data data;
		data.efd = eventfd(0, 0);
		assert(data.efd >= 0);
		data.readFd = fds[0];

		pthread_t thread;
		int ret = pthread_create(&thread, nullptr, blocker, &data);
		assert(ret == 0);

		uint64_t val;
		ssize_t read_len = read(data.efd, &val, sizeof(val));
		assert(read_len == sizeof(val));
		// Give the thread time to actually block.
		usleep(50'000);
		exit(0);
	}

	for (int i = 0; i < 500; i++) {
		int status;
		auto ret = waitpid(child, &status, WNOHANG);
		assert(ret >= 0);
		if (ret == child) {
			assert(WIFEXITED(status));
			assert(WEXITSTATUS(status) == 0);
			return;
		}
		usleep(10'000);
	}
	assert(!"process did not exit within 5 s");
}

DEFINE_TEST(exit_with_thread_in_epoll_wait, ([] {
	exit_with_blocked_thread(block_in_epoll_wait);
}))

DEFINE_TEST(exit_with_thread_in_poll, ([] {
	exit_with_blocked_thread(block_in_poll);
}))
