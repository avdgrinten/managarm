#include <mutex>
#include <print>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <core/clock.hpp>
#include <core/dispatch.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include <protocols/fs/common.hpp>

#include "clocks.hpp"
#include "fs.hpp"
#include "interval-timer.hpp"
#include "protocols/fs/common.hpp"
#include "timerfd.hpp"

// Avoid <linux/timerfd.h> inclusion since that header is not compatible with libc.
#define TFD_IOC_SET_TICKS _IOW('T', 0, uint64_t)

namespace {

bool logTimerfd = false;

struct OpenFile : FileWithDefaults {
private:
	struct Timer : posix::IntervalTimer {
		Timer(smarter::weak_ptr<File> file, uint64_t initial, uint64_t interval)
		: IntervalTimer{initial, interval}, file_{file} {
			assert(file_.lock()->kind() == FileKind::timerfd);
		}

		void raise(bool success) override {
			auto f = smarter::static_pointer_cast<OpenFile>(file_.lock());
			if(!f || !success)
				return;

			{
				std::lock_guard lock{f->mutex_};
				if(f->_activeTimer.get() != this)
					return;
				f->_expirations++;
				f->_theSeq++;
			}
			f->_seqBell.raise();
		}

		void expired() override {
			auto f = smarter::static_pointer_cast<OpenFile>(file_.lock());
			if(!f)
				return;

			std::shared_ptr<Timer> self;
			std::lock_guard lock{f->mutex_};
			if(f->_activeTimer.get() != this)
				return;
			self = std::move(f->_activeTimer);
		}

	private:
		smarter::weak_ptr<File> file_;
	};

public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(int clock, bool non_block)
	: FileWithDefaults{FileKind::timerfd,  StructName::get("timerfd"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)},
			_clock{clock}, nonBlock_{non_block},
			_activeTimer{nullptr}, _expirations{0}, _theSeq{0} {
		assert(_clock == CLOCK_MONOTONIC || _clock == CLOCK_REALTIME);
	}

	~OpenFile() override {
		// Nothing to do here.
	}

	void handleClose() override {
		std::shared_ptr<Timer> timer;
		{
			std::lock_guard lock{mutex_};
			timer = std::move(_activeTimer);
		}
		if(timer)
			timer->cancel();
		_seqBell.raise();
		_cancelServe.cancel();
	}

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ct) override {
		if(max_length < sizeof(uint64_t))
			co_return std::unexpected{Error::illegalArguments};

		std::unique_lock lock{mutex_};
		if(!_expirations && nonBlock_)
			co_return std::unexpected{Error::wouldBlock};

		while(!_expirations) {
			lock.unlock();
			auto result = co_await _seqBell.async_wait_if([&] {
				std::lock_guard lock{mutex_};
				return !_expirations;
			}, ct);
			if (!result)
				co_return std::unexpected{Error::interrupted};
			lock.lock();
		}

		memcpy(data, &_expirations, sizeof(uint64_t));
		_expirations = 0;
		co_return sizeof(uint64_t);
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t in_seq, int mask,
			async::cancellation_token cancellation) override {
		if(logTimerfd)
			std::cout << "posix: timerfd::pollWait(" << in_seq << ")" << std::endl;

		std::unique_lock lock{mutex_};
		assert(in_seq <= _theSeq);

		auto computeEdges = [&] {
			int edges = 0;
			if (_theSeq > in_seq)
				edges |= EPOLLIN;
			return edges;
		};

		int edges = 0;
		while(true) {
			if(!isOpen())
				co_return Error::fileClosed;

			edges = computeEdges();
			if (edges & mask)
				break;

			lock.unlock();
			auto result = co_await _seqBell.async_wait_if([&] {
				std::lock_guard lock{mutex_};
				return isOpen() && !(computeEdges() & mask);
			}, cancellation);
			lock.lock();
			if (!result)
				break;
		}

		co_return PollWaitResult(_theSeq, edges & mask);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		std::lock_guard lock{mutex_};
		co_return PollStatusResult(_theSeq, _expirations ? EPOLLIN : 0);
	}

	async::result<int> getFileFlags() override {
		int flags = 0;

		if(nonBlock_)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	async::result<void> setFileFlags(int flags) override {
		if(flags & ~O_NONBLOCK) {
			std::println("posix: setFileFlags on \e[1;34m{}\e[0m called with unknown flags {:#x}",
				structName(), flags & ~O_NONBLOCK);
			co_return;
		}

		if(flags & O_NONBLOCK)
			nonBlock_ = true;
		else
			nonBlock_ = false;
		co_return;
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, OpenFile *self) {
			switch(req.command()) {
				case TFD_IOC_SET_TICKS: {
					{
						std::lock_guard lock{self->mutex_};
						self->_expirations = req.ticks();
						self->_theSeq++;
					}
					self->_seqBell.raise();

					managarm::fs::GenericIoctlReply resp;
					resp.set_error(managarm::fs::Errors::SUCCESS);

					auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);
					HEL_CHECK(send_resp.error());
					break;
				}
				default: {
					std::println("timerfd: unexpected ioctl request 0x{:x}", req.command());
					auto [dismiss] = co_await helix_ng::exchangeMsgs(
						conversation, helix_ng::dismiss());
					HEL_CHECK(dismiss.error());
					break;
				}
			}
			co_return {};
		}
	};

	async::result<void> ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		auto res = co_await dispatchRequest<
			managarm::fs::GenericIoctlRequest
		>(conversation, std::move(msg), HandleIoctl{}, this);

		if (!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	// Returns the previous setting in oldInitial and oldInterval.
	void setTime(bool relative, const timespec initial, const timespec interval,
			timespec &oldInitial, timespec &oldInterval) {
		uint64_t initialNanos = 0;
		uint64_t intervalNanos = 0;

		if(initial.tv_sec || initial.tv_nsec) {
			initialNanos = posix::convertToNanos(initial, _clock, relative);
			intervalNanos = posix::convertToNanos(interval, CLOCK_MONOTONIC);
		}

		std::shared_ptr<Timer> newTimer;
		if(initialNanos || intervalNanos)
			newTimer = std::make_shared<Timer>(weakFile(), initialNanos, intervalNanos);

		std::shared_ptr<Timer> oldTimer;
		{
			std::lock_guard lock{mutex_};
			oldTimer = std::exchange(_activeTimer, newTimer);
			// Pending expirations belong to the old setting, so disarming must drop them too.
			_expirations = 0;
		}

		timeOf(oldTimer.get(), oldInitial, oldInterval);
		if(oldTimer)
			oldTimer->cancel();
		if(newTimer)
			Timer::arm(std::move(newTimer));
	}

	void getTime(timespec &initial, timespec &interval) {
		std::shared_ptr<Timer> timer;
		{
			std::lock_guard lock{mutex_};
			timer = _activeTimer;
		}
		timeOf(timer.get(), initial, interval);
	}

private:
	static void timeOf(Timer *timer, timespec &initial, timespec &interval) {
		if(timer) {
			uint64_t initialNanos = 0;
			uint64_t intervalNanos = 0;

			timer->getTime(initialNanos, intervalNanos);

			initial.tv_sec = initialNanos / 1'000'000'000;
			initial.tv_nsec = initialNanos % 1'000'000'000;
			interval.tv_sec = intervalNanos / 1'000'000'000;
			interval.tv_nsec = intervalNanos % 1'000'000'000;
		} else {
			initial.tv_sec = 0;
			initial.tv_nsec = 0;
			interval.tv_sec = 0;
			interval.tv_nsec = 0;
		}
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;
	int _clock;
	std::atomic<bool> nonBlock_;

	// Leaf lock; _seqBell is raised and timers are cancelled after releasing it.
	std::mutex mutex_;

	// Currently active timer.
	// Protected by mutex_.
	std::shared_ptr<Timer> _activeTimer;

	// Number of expirations since last read().
	// Protected by mutex_.
	uint64_t _expirations;

	// Protected by mutex_.
	uint64_t _theSeq;
	async::recurring_event _seqBell;
};

} // anonymous namespace

namespace timerfd {

smarter::shared_ptr<File, FileHandle> createFile(int clock, bool non_block) {
	auto file = smarter::make_shared<OpenFile>(clock, non_block);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

void setTime(File *file, int flags, struct timespec initial, struct timespec interval,
		timespec &oldInitial, timespec &oldInterval) {
	if(logTimerfd)
		std::cout << "setTime() initial: " << initial.tv_sec << " + " << initial.tv_nsec
				<< ", interval: " << interval.tv_sec << " + " << interval.tv_nsec << std::endl;

	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->setTime(!(flags & TFD_TIMER_ABSTIME), initial, interval, oldInitial, oldInterval);
}

void getTime(File *file, timespec &initial, timespec &interval) {
	auto timerfd = static_cast<OpenFile *>(file);
	timerfd->getTime(initial, interval);
}

} // namespace timerfd

