#include <assert.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(socket_accept_timeout, ([] {
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un local;
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/testsocket");
	unlink("/tmp/testsocket");

	struct timespec timeout = {1, 0};
	int ret = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	assert(!ret);

	size_t len = strlen(local.sun_path) + sizeof(local.sun_family);
	ret = bind(s, (struct sockaddr *) &local, len);
	assert(!ret);

	ret = listen(s, 1);
	assert(!ret);

	struct timeval before;
	gettimeofday(&before, nullptr);

	struct sockaddr_un remote;
	socklen_t sock_len;
	ret = accept(s, (struct sockaddr *) &remote, &sock_len);
	assert(ret == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);

	struct timeval after;
	gettimeofday(&after, nullptr);

	struct timeval diff = {0, 0};
	timersub(&after, &before, &diff);
	fprintf(stderr, "accept() waited for %ld.%06ld sec\n", diff.tv_sec, diff.tv_usec);
	assert(diff.tv_sec >= 1);

	unlink("/tmp/testsocket");
}));

DEFINE_TEST(socket_invalid_types, ([] {
	int s = socket(AF_UNIX, 0, 0);
	assert(s == -1);
	assert(errno == ESOCKTNOSUPPORT);

	s = socket(AF_UNIX, INT_MAX, 0);
	assert(s == -1);
	assert(errno == EINVAL);
}));

DEFINE_TEST(socket_peercred, ([] {
	pid_t child = fork();
	assert(child >= 0);

	if(!child) {
		// The test suite normally runs as root. Drop privileges in the child so
		// that an implementation which hard-codes root credentials cannot pass.
		if(!getuid()) {
			int ret = setgid(2000);
			assert(!ret);
			ret = setuid(1000);
			assert(!ret);
		}
		assert(getuid() || getgid());

		int fds[2];
		int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
		assert(!ret);

		struct ucred creds;
		socklen_t creds_size = sizeof(creds);
		ret = getsockopt(fds[0], SOL_SOCKET, SO_PEERCRED, &creds, &creds_size);
		assert(!ret);
		assert(creds_size == sizeof(creds));
		assert(creds.pid == getpid());
		assert(creds.uid == getuid());
		assert(creds.gid == getgid());

		close(fds[0]);
		close(fds[1]);
		_exit(0);
	}

	int status;
	pid_t ret = waitpid(child, &status, 0);
	assert(ret == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}));

DEFINE_TEST(socket_shutdown_wr, ([] {
	int fds[2];
	int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
	assert(!ret);

	struct pollfd pfd;
	pfd.fd = fds[1];
	pfd.events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP | POLLNVAL;
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLOUT));

	ret = write(fds[0], &fds[0], sizeof(fds[0]));
	assert(ret == sizeof(fds[0]));

	ret = shutdown(fds[1], SHUT_WR);
	assert(!ret);

	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLIN | POLLOUT));

	pfd.fd = fds[0];
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLOUT));

	int discard = 0;
	ret = send(fds[1], &discard, sizeof(discard), MSG_NOSIGNAL);
	ret = send(fds[1], &discard, sizeof(discard), MSG_NOSIGNAL);
	assert(ret == -1);
	assert(errno == EPIPE);
	close(fds[0]);
	close(fds[1]);
}));

DEFINE_TEST(socket_shutdown_rd, ([] {
	int fds[2];
	int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
	assert(!ret);
	ret = write(fds[0], &fds[0], sizeof(fds[0]));
	assert(ret == sizeof(fds[0]));

	struct pollfd pfd;
	pfd.fd = fds[1];
	pfd.events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP | POLLNVAL;
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLIN | POLLOUT));

	ret = shutdown(fds[1], SHUT_RD);
	assert(!ret);

	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLIN | POLLOUT | POLLRDHUP));

	pfd.fd = fds[0];
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLOUT));

	int discard;
	ret = recv(fds[1], &discard, sizeof(discard), 0);
	ret = recv(fds[1], &discard, sizeof(discard), 0);
	assert(ret == 0);
	close(fds[0]);
	close(fds[1]);
}));

// From https://gist.github.com/netbsduser/b219af354dbe01083f7a1c57ac2c531a
DEFINE_TEST(socket_msg_boundary, ([] {
	int sock[2];
	int r;
	struct msghdr msg;
	struct iovec iov;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	char buf[10];

	r = socketpair(PF_UNIX, SOCK_STREAM, 0, sock);
	if (r < 0)
		err(EXIT_FAILURE, "socketpair");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *) "Hello";
	iov.iov_len = 5;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*((int *)CMSG_DATA(cmsg)) = 0;

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg 1");

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg 2");

	iov.iov_base = buf;
	iov.iov_len = 10;

	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	printf("recvmsg returned %d, should be 5; controllen is %u\n", r,
	    msg.msg_controllen);
	assert(r == 5);

	iov.iov_len = 5;
	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	printf("received remaining %d bytes; controllen is %u\n", r,
	    msg.msg_controllen);
}));

// From https://gist.github.com/netbsduser/b219af354dbe01083f7a1c57ac2c531a
DEFINE_TEST(socket_msg_partial_read, ([] {
	int sock[2];
	int r;
	struct msghdr msg;
	struct iovec iov;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	char buf[10];

	r = socketpair(PF_UNIX, SOCK_STREAM, 0, sock);
	if (r < 0)
		err(EXIT_FAILURE, "socketpair");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *) "Hello";
	iov.iov_len = 5;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*((int *)CMSG_DATA(cmsg)) = 0;

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg");

	/* this should dispose of the rights */
	r = read(sock[1], buf, 1);
	if (r < 0)
		err(EXIT_FAILURE, "read");

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf) - 1;

	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	printf("recvmsg returned %d bytes; controllen is %u (should be 0)\n",
		r, msg.msg_controllen);
	assert(msg.msg_controllen == 0);
}));

// From https://gist.github.com/netbsduser/b219af354dbe01083f7a1c57ac2c531a
DEFINE_TEST(socket_msg_fd_truncation, ([] {
	int sock[2];
	int r;
	struct msghdr msg;
	struct iovec iov;
	char send_cmsgbuf[CMSG_SPACE(sizeof(int) * 4)];
	char recv_cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	char buf[10];
	int *fds;

	r = socketpair(PF_UNIX, SOCK_STREAM, 0, sock);
	if (r < 0)
		err(EXIT_FAILURE, "socketpair");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = send_cmsgbuf;
	msg.msg_controllen = sizeof(send_cmsgbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *) "Hello";
	iov.iov_len = 5;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 4);
	fds = (int *)CMSG_DATA(cmsg);
	fds[0] = 0;
	fds[1] = 0;
	fds[2] = 0;
	fds[3] = 0;

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = recv_cmsgbuf;
	msg.msg_controllen = sizeof(recv_cmsgbuf);

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf) - 1;

	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	assert(msg.msg_flags & MSG_CTRUNC);

	if (msg.msg_controllen > 0) {
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			int nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);

			printf("received %d FDs\n", nfds);
		} else {
			printf("received cmsg other than SCM_RIGHTS\n");
		}
	} else {
		printf("no control data received\n");
	}
}));

DEFINE_TEST(socket_connect_dgram, ([] {
	int client_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	assert(client_fd != -1);
	int server_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	assert(server_fd != -1);

	struct sockaddr_un server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_un));
	server_addr.sun_family = AF_UNIX;
	strcpy(server_addr.sun_path, "/tmp/managarm-test-dgram.sock");

	unlink(server_addr.sun_path);
	int ret = bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr));
	assert(ret == 0);

	const char *msg = "hello";
	ret = send(client_fd, msg, strlen(msg), 0);
	assert(ret == -1);
	assert(errno == ENOTCONN);

	ret = connect(client_fd, (struct sockaddr *) &server_addr, sizeof(server_addr));
	assert(ret == 0);

	struct sockaddr_un peer_addr;
	socklen_t peer_addr_len = sizeof(struct sockaddr_un);
	ret = getpeername(client_fd, (struct sockaddr *) &peer_addr, &peer_addr_len);
	assert(ret == 0);
	assert(strcmp(peer_addr.sun_path, server_addr.sun_path) == 0);

	ret = send(client_fd, msg, strlen(msg), 0);
	assert(ret == (int)strlen(msg));

	char buffer[16];
	struct sockaddr_un remote_addr;
	socklen_t remote_addr_len = sizeof(struct sockaddr_un);
	ret = recvfrom(server_fd, buffer, 15, 0, (struct sockaddr *) &remote_addr, &remote_addr_len);
	assert(ret == (int)strlen(msg));
	buffer[ret] = 0;
	assert(strcmp(buffer, msg) == 0);

	struct sockaddr_un zero_addr;
	memset(&zero_addr, 0, sizeof(zero_addr));
	zero_addr.sun_family = AF_UNSPEC;
	ret = connect(client_fd, (struct sockaddr *) &zero_addr, sizeof(zero_addr));
	assert(ret == 0);

	ret = send(client_fd, msg, strlen(msg), 0);
	assert(ret == -1);
	assert(errno == ENOTCONN);

	struct sockaddr_in inet_addr;
	memset(&inet_addr, 0, sizeof(inet_addr));
	inet_addr.sin_family = AF_INET;
	inet_addr.sin_port = 420;
	inet_addr.sin_addr = {INADDR_LOOPBACK};
	ret = connect(client_fd, (struct sockaddr *) &inet_addr, sizeof(inet_addr));
	assert(ret == -1);
	assert(errno == EINVAL);

	close(client_fd);
	close(server_fd);
	unlink(server_addr.sun_path);
}));

DEFINE_TEST(socket_stream_eof, ([] {
	int fds[2];
	int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	assert(!ret);

	ret = shutdown(fds[0], SHUT_WR);
	assert(!ret);

	char buf[10];
	ssize_t n = read(fds[1], buf, sizeof(buf));
	assert(n == 0);

	close(fds[0]);
	close(fds[1]);

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	assert(!ret);

	close(fds[0]);

	n = read(fds[1], buf, sizeof(buf));
	assert(n == 0);

	close(fds[1]);

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	assert(!ret);

	ret = write(fds[0], "a", 1);
	assert(ret == 1);

	ret = shutdown(fds[0], SHUT_WR);
	assert(!ret);

	n = read(fds[1], buf, sizeof(buf));
	assert(n == 1);
	assert(buf[0] == 'a');

	n = read(fds[1], buf, sizeof(buf));
	assert(n == 0);

	close(fds[0]);
	close(fds[1]);
}));

namespace {

double elapsedSince(const struct timespec &before) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - before.tv_sec) + (now.tv_nsec - before.tv_nsec) / 1e9;
}

} // namespace

DEFINE_TEST(socket_tcp_timeouts, ([] {
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	assert(listener >= 0);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int ret = bind(listener, (struct sockaddr *) &addr, sizeof(addr));
	assert(!ret);
	ret = listen(listener, 1);
	assert(!ret);
	socklen_t addrLen = sizeof(addr);
	ret = getsockname(listener, (struct sockaddr *) &addr, &addrLen);
	assert(!ret);

	struct timeval timeout = {0, 500'000};
	struct timespec before;

	// SO_RCVTIMEO bounds accept().
	ret = setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	assert(!ret);
	clock_gettime(CLOCK_MONOTONIC, &before);
	ret = accept(listener, nullptr, nullptr);
	assert(ret == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	assert(elapsedSince(before) >= 0.5);

	int client = socket(AF_INET, SOCK_STREAM, 0);
	assert(client >= 0);
	ret = connect(client, (struct sockaddr *) &addr, sizeof(addr));
	assert(!ret);
	int server = accept(listener, nullptr, nullptr);
	assert(server >= 0);

	// SO_RCVTIMEO bounds recv() without data.
	ret = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	assert(!ret);
	char buf[4096];
	clock_gettime(CLOCK_MONOTONIC, &before);
	ssize_t n = recv(client, buf, sizeof(buf), 0);
	assert(n == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	assert(elapsedSince(before) >= 0.5);

	// The socket still works after a timeout.
	n = write(server, "a", 1);
	assert(n == 1);
	n = recv(client, buf, sizeof(buf), 0);
	assert(n == 1);
	assert(buf[0] == 'a');

	// SO_SNDTIMEO bounds send() once the peer stops reading and all buffers are full.
	ret = setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	assert(!ret);
	memset(buf, 'b', sizeof(buf));
	size_t total = 0;
	while(true) {
		n = send(client, buf, sizeof(buf), 0);
		if(n < 0)
			break;
		total += n;
		assert(total < (size_t(256) << 20));
	}
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	assert(total > 0);

	// Like on Linux, a negative timeout makes recv() time out at once.
	timeout = {-1, 0};
	ret = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	assert(!ret);
	clock_gettime(CLOCK_MONOTONIC, &before);
	n = recv(client, buf, sizeof(buf), 0);
	assert(n == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	assert(elapsedSince(before) < 0.5);

	// A zero timeout disables it again.
	timeout = {0, 0};
	ret = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	assert(!ret);

	close(server);
	close(client);
	close(listener);
}));

DEFINE_TEST(socket_packet_validation, ([] {
	// Protocol 0 is valid; the socket receives nothing until bind() sets a protocol.
	int s = socket(AF_PACKET, SOCK_RAW, 0);
	if(s < 0 && errno == EPERM)
		skip_test("AF_PACKET requires CAP_NET_RAW");
	assert(s >= 0);

	int t = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
	assert(t >= 0);
	close(t);

	errno = 0;
	assert(socket(AF_PACKET, SOCK_SEQPACKET, 0) < 0);
	assert(errno == ESOCKTNOSUPPORT);

	errno = 0;
	assert(socket(AF_PACKET, SOCK_STREAM, htons(ETH_P_ALL)) < 0);
	assert(errno == ESOCKTNOSUPPORT);

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);

	errno = 0;
	assert(bind(s, reinterpret_cast<struct sockaddr *>(&sll), sizeof(struct sockaddr)) < 0);
	assert(errno == EINVAL);

	sll.sll_family = AF_INET;
	errno = 0;
	assert(bind(s, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)) < 0);
	assert(errno == EINVAL);

	// Without bind() there is no device to send on.
	char frame[ETH_ZLEN] = {};
	errno = 0;
	assert(write(s, frame, sizeof(frame)) < 0);
	assert(errno == ENXIO);

	close(s);
}));
