#include <cassert>
#include <cstring>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>

#include "testsuite.hpp"

DEFINE_TEST(epoll_mod_active, ([] {
	int e;
	int pending;

	int fd = eventfd(0, 0);
	assert(fd >= 0);

	int epfd = epoll_create1(0);
	assert(epfd >= 0);

	epoll_event evt;

	memset(&evt, 0, sizeof(epoll_event));
	evt.events = 0;
	e = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
	assert(!e);

	// Nothing should be pending.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(!pending);

	uint64_t n = 1;
	auto written = write(fd, &n, sizeof(uint64_t));
	assert(written == sizeof(uint64_t));

	memset(&evt, 0, sizeof(epoll_event));
	evt.events = EPOLLIN;
	e = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);
	assert(!e);

	// The FD should be pending now.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	// The FD should be pending again.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	uint64_t discard;
	int ret = read(fd, &discard, sizeof(discard));
	assert(ret == sizeof(discard));

	// The FD should not be pending again.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 0);

	memset(&evt, 0, sizeof(epoll_event));
	evt.events = EPOLLIN | EPOLLONESHOT;
	e = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);
	assert(!e);

	// The FD should not be pending again.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 0);

	written = write(fd, &n, sizeof(uint64_t));
	assert(written == sizeof(uint64_t));

	// The FD should be pending again, once.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 0);

	close(epfd);
	close(fd);
}))

DEFINE_TEST(epoll_oneshot_rearm_while_ready, ([] {
	int e;
	int pending;

	int fd = eventfd(1, 0);
	assert(fd >= 0);

	int epfd = epoll_create1(0);
	assert(epfd >= 0);

	epoll_event evt;

	memset(&evt, 0, sizeof(epoll_event));
	evt.events = EPOLLIN | EPOLLONESHOT;
	e = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
	assert(!e);

	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	// Re-arm without an epoll_wait() in between; the FD is still readable.
	memset(&evt, 0, sizeof(epoll_event));
	evt.events = EPOLLIN | EPOLLONESHOT;
	e = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);
	assert(!e);

	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	// Switching to level-triggered mode also re-arms the item.
	memset(&evt, 0, sizeof(epoll_event));
	evt.events = EPOLLIN;
	e = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);
	assert(!e);

	for(int i = 0; i < 2; i++) {
		memset(&evt, 0, sizeof(epoll_event));
		pending = epoll_wait(epfd, &evt, 1, 0);
		assert(pending == 1);
		assert(evt.events & EPOLLIN);
	}

	close(epfd);
	close(fd);
}))

DEFINE_TEST(epoll_edge_triggered, ([] {
	int e;
	int pending;

	int fd = eventfd(0, EFD_NONBLOCK);
	assert(fd >= 0);

	int epfd = epoll_create1(0);
	assert(epfd >= 0);

	epoll_event evt;

	memset(&evt, 0, sizeof(epoll_event));
	evt.events = 0;
	e = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
	assert(!e);

	// Nothing should be pending.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(!pending);

	uint64_t n = 1;
	auto written = write(fd, &n, sizeof(uint64_t));
	assert(written == sizeof(uint64_t));

	memset(&evt, 0, sizeof(epoll_event));
	evt.events = EPOLLIN | EPOLLET;
	e = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);
	assert(!e);

	// The FD should be pending now.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	// We expect no new events, as this is edge-triggered.
	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 0);

	n = 1;
	written = write(fd, &n, sizeof(uint64_t));
	assert(written == sizeof(uint64_t));

	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 1);
	assert(evt.events & EPOLLIN);

	memset(&evt, 0, sizeof(epoll_event));
	pending = epoll_wait(epfd, &evt, 1, 0);
	assert(pending == 0);

	close(epfd);
	close(fd);
}));

DEFINE_TEST(pselect_no_fd_wait, ([] {
	struct timespec req = {0, 500000000};
	struct timespec start = {0, 0};
	struct timespec end = {0, 0};

	int ret = clock_gettime(CLOCK_MONOTONIC, &start);
	assert(ret == 0);

	ret = pselect(0, NULL, NULL, NULL, &req, NULL);
	assert(ret == 0);

	ret = clock_gettime(CLOCK_MONOTONIC, &end);
	assert(ret == 0);

	long elapsed = (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);
	assert(elapsed >= 500000000L);

	return 0;
}))
