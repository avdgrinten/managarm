#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <frg/list.hpp>
#include <frg/mutex.hpp>
#include <frg/scope_exit.hpp>
#include <frg/std_compat.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <ostrace.bragi.hpp>

namespace protocols::ostrace {

namespace {

constexpr size_t pageSize = 32 * 1024;
// Upper bound on the number of pages of a process.
constexpr size_t maxPages = 32;
// Time (in nanoseconds) after the first event of a page at which the page is sealed.
// This ensures that pages are sealed (and flushed) even if no further events come in.
constexpr uint64_t maxLatency = 50'000'000;

struct Page {
	// The buffer, size, writer, firstEvent and numEvents are only accessed by the thread that owns
	// the page (under mutex) and by the flusher once the page is sealed.
	std::mutex mutex;
	std::vector<char> buffer = std::vector<char>(pageSize);
	size_t size = 0;
	// ThreadState::writer of the thread that the page belongs to.
	uint64_t writer = 0;
	// The page contains the events firstEvent to firstEvent + numEvents - 1.
	uint64_t firstEvent = 0;
	uint64_t numEvents = 0;
	// Time at which the sealer seals the page.
	// Written under both mutex and Source::mutex_.
	uint64_t deadline = 0;
	// Incremented whenever the page is sealed.
	// Written under both mutex and Source::mutex_.
	uint64_t generation = 0;
	// Links the page into Source::freePages_, openPages_ or sealedPages_.
	// Protected by Source::mutex_.
	frg::default_list_hook<Page> hook;
};

using PageList = frg::intrusive_list<
	Page,
	frg::locate_member<Page, frg::default_list_hook<Page>, &Page::hook>
>;

// Only accessed by the thread that it belongs to.
struct ThreadState {
	// Current active page. This page has already at least one event.
	// The sealer may have sealed the page since, see activeGeneration.
	Page *active = nullptr;
	// Page::generation of the active page when the thread took the page.
	uint64_t activeGeneration = 0;
	// Indexed by term ID. Whether the active page already contains a Definition of the term.
	std::vector<bool> defined;
	// Identifies the thread in the Frames of its pages.
	uint64_t writer = 0;
	// Number of the next event. Dropped events also consume a number, such that readers see a gap.
	uint64_t nextEvent = 0;
};

// All Contexts of a process share a single source.
//
// Two coroutines on the thread that connects move pages to the kernel: the sealer seals active
// pages once their deadline passes, and the flusher sends sealed pages to the kernel.
struct Source {
	Source() = default;

	Source(const Source &) = delete;
	Source &operator= (const Source &) = delete;

	// Connects to the kernel. Returns whether ostrace is enabled.
	async::result<bool> connect();

	// Allocates a term ID.
	ItemId allocateId() {
		return ItemId{nextId_.fetch_add(1, std::memory_order_relaxed)};
	}

	ThreadState *threadState();

	// Returns an empty page. If all pages are in use, this discards the oldest sealed page.
	// Returns nullptr if no page can be discarded either.
	Page *acquirePage();

	// Starts the deadline of a newly active page. The caller holds Page::mutex.
	// Sets mustWakeSealer if the caller needs to wake up the sealer.
	void open(Page *page, bool &mustWakeSealer);

	// Seals an open page. Called both from writeEvent() and from sealExpired_().
	// The caller holds Page::mutex. Sets mustWakeFlusher if the caller needs to wake up the flusher.
	void seal(Page *page, bool &mustWakeFlusher);

	void wakeSealer() {
		if(!sealerPending_.exchange(true, std::memory_order_acq_rel))
			sealerEvent_.raise();
	}

	void wakeFlusher() {
		if(!flusherPending_.exchange(true, std::memory_order_acq_rel))
			flusherEvent_.raise();
	}

private:
	// Sealer coroutine.
	async::result<void> sealExpired_();
	// Flusher coroutine.
	async::result<void> flush_();
	// Sends pages to the kernel.
	async::result<void> send_(Page *page);

	async::mutex connectMutex_;
	// Protected by connectMutex_.
	bool connected_ = false;
	// Protected by connectMutex_.
	bool enabled_ = false;
	// Set by connect() under connectMutex_. Immutable once connected_ is set.
	helix::UniqueLane lane_;

	std::atomic<uint64_t> nextId_{1};
	std::atomic<uint64_t> nextWriter_{0};

	std::atomic<bool> sealerPending_{false};
	std::atomic<bool> flusherPending_{false};
	async::recurring_event sealerEvent_;
	async::recurring_event flusherEvent_;

	// Lock order: Page::mutex before mutex_.
	std::mutex mutex_;
	// Protected by mutex_.
	std::vector<std::unique_ptr<Page>> pages_;
	// Protected by mutex_.
	PageList freePages_;
	// Pages in the order in which they were opened, and thus by deadline.
	// Protected by mutex_.
	PageList openPages_;
	// Sealed pages in the order in which they were sealed.
	// Protected by mutex_.
	PageList sealedPages_;
};

// Never destructed since other threads can still emit events during exit.
Source &globalSource() {
	static frg::eternal<Source> source;
	return *source;
}

async::result<bool> Source::connect() {
	co_await connectMutex_.async_lock();
	frg::unique_lock lock{frg::adopt_lock, connectMutex_};

	if(connected_)
		co_return enabled_;

	// Find ostrace in mbus.
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "ostrace"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	lane_ = (co_await entity.getRemoteLane()).unwrap();

	// Perform the negotiation request.
	managarm::ostrace::NegotiateReq req;

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	recvResp.reset();
	assert(maybeResp);
	auto &resp = maybeResp.value();

	if(resp.error() != managarm::ostrace::Error::OSTRACE_GLOBALLY_DISABLED) {
		assert(resp.error() == managarm::ostrace::Error::SUCCESS);
		enabled_ = true;
		async::detach(sealExpired_());
		async::detach(flush_());
	}

	connected_ = true;
	co_return enabled_;
}

ThreadState *Source::threadState() {
	// Leaked such that destructors of other thread_locals can still emit events.
	thread_local ThreadState *state = nullptr;
	if(!state) {
		state = new ThreadState;
		state->writer = nextWriter_.fetch_add(1, std::memory_order_relaxed);
	}
	return state;
}

Page *Source::acquirePage() {
	std::lock_guard lock{mutex_};

	Page *page;
	if(!freePages_.empty()) {
		page = freePages_.pop_back();
	}else if(pages_.size() < maxPages) {
		pages_.push_back(std::make_unique<Page>());
		page = pages_.back().get();
	}else if(!sealedPages_.empty()) {
		// Readers see a gap in the event numbers of the page's writer.
		page = sealedPages_.pop_front();
	}else{
		return nullptr;
	}

	return page;
}

void Source::open(Page *page, bool &mustWakeSealer) {
	std::lock_guard lock{mutex_};

	// Reading the clock under the lock keeps openPages_ sorted by deadline.
	page->deadline = helix::getClock() + maxLatency;
	if(openPages_.empty())
		mustWakeSealer = true;
	openPages_.push_back(page);
}

void Source::seal(Page *page, bool &mustWakeFlusher) {
	std::lock_guard lock{mutex_};

	openPages_.erase(openPages_.iterator_to(page));
	++page->generation;
	// Otherwise, the flusher finds the page before it waits again.
	if(sealedPages_.empty())
		mustWakeFlusher = true;
	sealedPages_.push_back(page);
}

async::result<void> Source::sealExpired_() {
	while(true) {
		// Clear before inspecting openPages_: open() requests a wakeup if it runs after this.
		sealerPending_.exchange(false, std::memory_order_acq_rel);

		auto now = helix::getClock();
		std::optional<uint64_t> deadline;
		bool mustWakeFlusher = false;
		while(true) {
			Page *page;
			uint64_t generation;
			{
				std::lock_guard lock{mutex_};
				if(openPages_.empty())
					break;
				page = openPages_.front();
				if(page->deadline > now) {
					deadline = page->deadline;
					break;
				}
				generation = page->generation;
			}

			// Page::mutex is ordered before mutex_, so only take Page::mutex here.
			// We need Page::mutex to call seal().
			std::lock_guard pageLock{page->mutex};
			// We need to re-check that nobody else sealed the page in the meantime.
			if(page->generation == generation)
				seal(page, mustWakeFlusher);
		}
		if(mustWakeFlusher)
			wakeFlusher();

		if(deadline) {
			co_await helix::sleepUntil(*deadline, {});
		}else{
			(void)co_await sealerEvent_.async_wait_if([this] {
				return !sealerPending_.load(std::memory_order_acquire);
			}, {});
		}
	}
}

async::result<void> Source::flush_() {
	while(true) {
		// Clear before inspecting sealedPages_: sealers wake us if they seal after this.
		flusherPending_.exchange(false, std::memory_order_acq_rel);

		while(true) {
			Page *page;
			{
				std::lock_guard lock{mutex_};
				if(sealedPages_.empty())
					break;
				page = sealedPages_.pop_front();
			}

			co_await send_(page);

			std::lock_guard lock{mutex_};
			freePages_.push_back(page);
		}

		(void)co_await flusherEvent_.async_wait_if([this] {
			return !flusherPending_.load(std::memory_order_acquire);
		}, {});
	}
}

async::result<void> Source::send_(Page *page) {
	managarm::ostrace::EmitReq req;
	req.set_size(page->size);
	req.set_writer(page->writer);
	req.set_first_event(page->firstEvent);
	req.set_num_events(page->numEvents);

	auto [offer, sendReq, sendData, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::sendBuffer(page->buffer.data(), page->size),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(sendData.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	recvResp.reset();
	assert(maybeResp);
	auto &resp = maybeResp.value();
	assert(resp.error() == managarm::ostrace::Error::SUCCESS);
}

} // anonymous namespace

namespace detail {

void writeEvent(std::span<const Term *const> terms, size_t size,
		frg::function_ref<void(std::span<char>)> write) {
	auto &source = globalSource();
	auto *thread = source.threadState();

	bool mustWakeSealer = false;
	bool mustWakeFlusher = false;
	// Declared before the lock, such that the wakeups happen after the lock is dropped.
	frg::scope_exit wakeOnExit{[&] {
		if(mustWakeSealer)
			source.wakeSealer();
		if(mustWakeFlusher)
			source.wakeFlusher();
	}};
	std::unique_lock<std::mutex> pageLock;

	auto event = thread->nextEvent++;

	// Upper bound on the size of the Definitions that the page lacks.
	// Using it for the fit checks avoids determining the missing Definitions up front.
	size_t maxDefinitionsSize = 0;
	for(auto *term : terms)
		maxDefinitionsSize += term->definition().size();

	auto *page = thread->active;
	if(page) {
		pageLock = std::unique_lock{page->mutex};
		if(page->generation != thread->activeGeneration) {
			// The sealer sealed the page.
			pageLock = {};
			page = nullptr;
		}else if(page->size + maxDefinitionsSize + size > pageSize) {
			source.seal(page, mustWakeFlusher);
			pageLock = {};
			page = nullptr;
		}
		if(!page)
			thread->active = nullptr;
	}
	if(!page) {
		// Only drop events that do not fit into any page once the active page is sealed,
		// such that the dropped event is a gap between pages.
		if(maxDefinitionsSize + size > pageSize)
			return;
		page = source.acquirePage();
		if(!page)
			return;
		pageLock = std::unique_lock{page->mutex};
		page->size = 0;
		page->writer = thread->writer;
		page->numEvents = 0;
		thread->active = page;
		thread->activeGeneration = page->generation;
		thread->defined.clear();
		source.open(page, mustWakeSealer);
	}

	for(auto *term : terms) {
		auto id = static_cast<uint64_t>(term->id());
		if(id < thread->defined.size() && thread->defined[id])
			continue;
		auto definition = term->definition();
		std::ranges::copy(definition, page->buffer.data() + page->size);
		page->size += definition.size();

		// Setting the bit right away also skips terms that the event uses more than once.
		if(id >= thread->defined.size())
			thread->defined.resize(id + 1);
		thread->defined[id] = true;
	}

	write({page->buffer.data() + page->size, size});
	page->size += size;
	if(!page->numEvents)
		page->firstEvent = event;
	++page->numEvents;
}

} // namespace detail

Context::Context(Vocabulary &vocabulary)
: vocabulary_{&vocabulary}, enabled_{false} { }

async::result<void> Context::create() {
	co_await initMutex_.async_lock();
	frg::unique_lock lock{frg::adopt_lock, initMutex_};

	if(isInitialized())
		co_return;

	if(co_await globalSource().connect()) {
		for (auto *term : vocabulary_->terms())
			define_(term);

		// Release: only publish the vocabulary once all of its terms are defined.
		enabled_.store(true, std::memory_order_release);
	}

	// Release: callers that skip create() must observe the completed initialization.
	initialized_.store(true, std::memory_order_release);
}

void Context::define_(Term *term) {
	assert(!term->ctx_);
	term->ctx_ = this;
	term->id_ = globalSource().allocateId();

	managarm::ostrace::Definition definition;
	definition.set_id(static_cast<uint64_t>(term->id_));
	definition.set_name(term->name());
	auto tailSize = definition.size_of_tail();
	// Never freed since other threads can still emit events during exit.
	auto *buffer = new char[8 + tailSize];
	bool encodeSuccess = bragi::write_head_tail(definition,
			std::span<char>(buffer, 8),
			std::span<char>(buffer + 8, tailSize));
	assert(encodeSuccess);
	term->definition_ = {buffer, 8 + tailSize};
}

} // namespace protocols::ostrace
