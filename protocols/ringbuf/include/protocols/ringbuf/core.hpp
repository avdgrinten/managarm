#pragma once

// Memory layout and wait-free core of the ringbuf protocol (see ringbuf.bragi).
// This header is freestanding such that it can be shared between the kernel and userspace.
// None of the classes here perform any synchronization among multiple producers
// (or among multiple users of the same ConsumerCore).

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

namespace protocols::ringbuf {

// Values match managarm::ringbuf::Mode.
enum class Mode : int32_t {
	reliable = 1,
	dropOldest = 2,
};

// Values match managarm::ringbuf::Framing.
enum class Framing : int32_t {
	stream = 1,
	records = 2,
};

struct RingParameters {
	Mode mode;
	Framing framing;
	size_t size;
};

enum class Role {
	producer,
	consumer,
};

// Configuration of a ring and role that the side that calls receive() plays for this ring.
struct RingSpec {
	RingParameters params;
	Role role;
};

// Determines which kick lanes are required. Roles are the roles of the side that calls receive().
struct LaneNeeds {
	LaneNeeds(const RingSpec *specs, size_t numSpecs) {
		for(size_t i = 0; i < numSpecs; ++i) {
			auto &spec = specs[i];
			// Consumers always wait; producers only wait for Mode::reliable.
			bool producerWaits = spec.params.mode == Mode::reliable;
			if(spec.role == Role::consumer) {
				toReceiver = true;
				toProvider = toProvider || producerWaits;
			}else{
				toProvider = true;
				toReceiver = toReceiver || producerWaits;
			}
		}
	}

	// Lane on which the side that calls provide() offers kicks.
	bool toReceiver = false;
	// Lane on which the side that calls receive() offers kicks.
	bool toProvider = false;
};

// Offset of the data area within the producer memory.
inline constexpr size_t dataOffset = 0x1000;
inline constexpr size_t consumerMemorySize = 0x1000;

// Header of the producer memory. Only written by the producer.
struct ProducerHeader {
	// Everything before this position is committed.
	std::atomic<uint64_t> head;
	// Mode::dropOldest only: everything before this position is invalidated.
	std::atomic<uint64_t> tail;
	// The consumer should not wait for its watermark as long as it is behind this position.
	std::atomic<uint64_t> flushed;
	// Mode::reliable only: the producer wants a kick once consumed reaches this position.
	std::atomic<uint64_t> wakeAt;
};

// Header of the consumer memory. Only written by the consumer.
struct ConsumerHeader {
	// Mode::reliable only: everything before this position can be overwritten.
	std::atomic<uint64_t> consumed;
	// The consumer wants a kick once head reaches this position.
	std::atomic<uint64_t> wakeAt;
};

static_assert(sizeof(ProducerHeader) <= dataOffset);
static_assert(sizeof(ConsumerHeader) <= consumerMemorySize);

// Framing::records only. Records are aligned to recordAlign; hence, headers never wrap around.
struct RecordHeader {
	uint32_t size;
	uint32_t reserved;
};

inline constexpr size_t recordAlign = 8;
static_assert(sizeof(RecordHeader) == recordAlign);

inline constexpr size_t effectiveRecordSize(size_t recordSize) {
	return (sizeof(RecordHeader) + recordSize + recordAlign - 1) & ~(recordAlign - 1);
}

inline constexpr bool isValidRingSize(uint64_t size) {
	return size >= dataOffset && !(size & (size - 1));
}

// True if moving a position from oldPosition to newPosition requires a kick.
inline constexpr bool passesWakeAt(uint64_t wakeAt, uint64_t oldPosition, uint64_t newPosition) {
	return oldPosition < wakeAt && wakeAt <= newPosition;
}

namespace detail {

inline void copyToRing(std::byte *data, size_t ringSize, uint64_t position,
		const void *source, size_t size) {
	auto offset = position & (ringSize - 1);
	auto preWrapSize = size < ringSize - offset ? size : ringSize - offset;
	memcpy(data + offset, source, preWrapSize);
	memcpy(data, static_cast<const std::byte *>(source) + preWrapSize, size - preWrapSize);
}

inline void copyFromRing(const std::byte *data, size_t ringSize, uint64_t position,
		void *destination, size_t size) {
	auto offset = position & (ringSize - 1);
	auto preWrapSize = size < ringSize - offset ? size : ringSize - offset;
	memcpy(destination, data + offset, preWrapSize);
	memcpy(static_cast<std::byte *>(destination) + preWrapSize, data, size - preWrapSize);
}

} // namespace detail

struct WritableSpan {
	std::byte *data;
	size_t size;
};

struct ReadableSpan {
	const std::byte *data;
	size_t size;
};

struct ProducerCore {
	ProducerCore() = default;

	// For Mode::reliable, consumerHeader is the header of the (single) consumer.
	ProducerCore(Mode mode, ProducerHeader *header, const ConsumerHeader *consumerHeader,
			std::byte *data, size_t ringSize)
	: mode_{mode}, header_{header}, consumerHeader_{consumerHeader},
			data_{data}, ringSize_{ringSize} {
		assert(isValidRingSize(ringSize));
	}

	uint64_t head() const { return head_; }

	// Mode::reliable only: picks up the progress of the consumer.
	// Returns false if the consumer violates the protocol.
	// seq_cst since armSpace() relies on the ordering against the store to wakeAt.
	[[nodiscard]] bool refresh() {
		assert(mode_ == Mode::reliable);
		auto consumed = consumerHeader_->consumed.load(std::memory_order_seq_cst);
		if(consumed < consumed_ || consumed > head_)
			return false;
		consumed_ = consumed;
		return true;
	}

	// Mode::reliable only: free space as of the last refresh().
	size_t freeSize() const {
		assert(mode_ == Mode::reliable);
		return ringSize_ - (head_ - consumed_);
	}

	// Mode::reliable only: contiguous part of the free space.
	WritableSpan writableSpan() const {
		auto offset = head_ & (ringSize_ - 1);
		auto size = freeSize();
		if(size > ringSize_ - offset)
			size = ringSize_ - offset;
		return {data_ + offset, size};
	}

	// Mode::reliable only: commits bytes that were written to writableSpan().
	void produce(size_t size) {
		assert(size <= freeSize());
		commit_(head_ + size);
	}

	// Mode::reliable only: returns false if there is not enough free space (as of the last refresh()).
	[[nodiscard]] bool tryEnqueueRecord(const void *record, size_t recordSize) {
		assert(effectiveRecordSize(recordSize) <= ringSize_);
		if(effectiveRecordSize(recordSize) > freeSize())
			return false;
		writeRecord_(record, recordSize);
		return true;
	}

	// Mode::dropOldest only: invalidates the oldest records as necessary.
	void enqueueRecord(const void *record, size_t recordSize) {
		assert(mode_ == Mode::dropOldest);
		assert(effectiveRecordSize(recordSize) <= ringSize_);

		// Compute the invalidated part of the ring buffer.
		auto invalidated = tail_;
		while(invalidated + ringSize_ < head_ + effectiveRecordSize(recordSize)) {
			assert(invalidated < head_);
			RecordHeader tailHeader;
			detail::copyFromRing(data_, ringSize_, invalidated, &tailHeader, sizeof(RecordHeader));
			assert(tailHeader.size <= ringSize_);
			invalidated += effectiveRecordSize(tailHeader.size);
		}

		// Invalidate the ring *before* writing to it.
		tail_ = invalidated;
		header_->tail.store(invalidated, std::memory_order_release);

		writeRecord_(record, recordSize);
	}

	// Asks the consumer to process everything that was committed so far, regardless of its
	// watermark. Returns true if the caller needs to check needsKickAfterFlush().
	[[nodiscard]] bool flush() {
		if(flushed_ == head_)
			return false;
		flushed_ = head_;
		header_->flushed.store(flushed_, std::memory_order_seq_cst);
		return true;
	}

	// To be called for each consumer after the head moved on from oldHead.
	bool needsKick(const ConsumerHeader *consumerHeader, uint64_t oldHead) const {
		return passesWakeAt(consumerHeader->wakeAt.load(std::memory_order_seq_cst),
				oldHead, head_);
	}

	// To be called for each consumer after flush() returned true.
	bool needsKickAfterFlush(const ConsumerHeader *consumerHeader) const {
		return consumerHeader->wakeAt.load(std::memory_order_seq_cst) > head_;
	}

	// Mode::reliable only: prepares to wait until the given amount of space is free.
	// Returns false if there is no need to wait; performs a refresh().
	[[nodiscard]] bool armSpace(size_t size, bool &violation) {
		assert(size <= ringSize_);
		violation = !refresh();
		if(violation || freeSize() >= size)
			return false;
		header_->wakeAt.store(head_ + size - ringSize_, std::memory_order_seq_cst);
		violation = !refresh();
		return !violation && freeSize() < size;
	}

private:
	void commit_(uint64_t newHead) {
		head_ = newHead;
		// Commit the operation *after* writing to the ring.
		header_->head.store(newHead, std::memory_order_seq_cst);
	}

	void writeRecord_(const void *record, size_t recordSize) {
		RecordHeader recordHeader{static_cast<uint32_t>(recordSize), 0};
		detail::copyToRing(data_, ringSize_, head_, &recordHeader, sizeof(RecordHeader));
		detail::copyToRing(data_, ringSize_, head_ + sizeof(RecordHeader), record, recordSize);
		commit_(head_ + effectiveRecordSize(recordSize));
	}

	Mode mode_ = Mode::reliable;
	ProducerHeader *header_ = nullptr;
	const ConsumerHeader *consumerHeader_ = nullptr;
	std::byte *data_ = nullptr;
	size_t ringSize_ = 0;

	// The shared copies of our own positions are never read back.
	uint64_t head_ = 0;
	uint64_t tail_ = 0;
	uint64_t flushed_ = 0;
	// Validated copy of the consumer's position.
	uint64_t consumed_ = 0;
};

enum class DequeueStatus {
	success,
	empty,
	// The other side does not adhere to the protocol; the ring cannot be used anymore.
	violation,
};

struct DequeueResult {
	DequeueStatus status;
	// Size of the record; the record is truncated if this is larger than the buffer.
	size_t recordSize = 0;
	// Mode::dropOldest only: number of bytes that were overwritten before we could dequeue them.
	uint64_t lost = 0;
};

struct ConsumerCore {
	ConsumerCore() = default;

	ConsumerCore(Mode mode, const ProducerHeader *producerHeader, ConsumerHeader *header,
			const std::byte *data, size_t ringSize)
	: mode_{mode}, producerHeader_{producerHeader}, header_{header},
			data_{data}, ringSize_{ringSize} {
		assert(isValidRingSize(ringSize));
	}

	// Position of the next byte that is consumed.
	uint64_t position() const { return position_; }

	// Mode::reliable only: picks up the progress of the producer.
	// Returns false if the producer violates the protocol.
	[[nodiscard]] bool refresh() {
		assert(mode_ == Mode::reliable);
		auto head = producerHeader_->head.load(std::memory_order_seq_cst);
		if(head < head_ || head > position_ + ringSize_)
			return false;
		head_ = head;
		return true;
	}

	// Mode::reliable only: number of available bytes as of the last refresh().
	size_t availableSize() const {
		assert(mode_ == Mode::reliable);
		return head_ - position_;
	}

	// Mode::reliable only: contiguous part of the available bytes.
	ReadableSpan readableSpan() const {
		auto offset = position_ & (ringSize_ - 1);
		auto size = availableSize();
		if(size > ringSize_ - offset)
			size = ringSize_ - offset;
		return {data_ + offset, size};
	}

	// Mode::reliable only: frees bytes that were taken from readableSpan().
	// Returns true if the producer needs a kick.
	[[nodiscard]] bool consume(size_t size) {
		assert(size <= availableSize());
		auto oldPosition = position_;
		position_ += size;
		header_->consumed.store(position_, std::memory_order_seq_cst);
		return passesWakeAt(producerHeader_->wakeAt.load(std::memory_order_seq_cst),
				oldPosition, position_);
	}

	// Copies the next record to the buffer (and truncates it if it does not fit).
	// kick is set to true if the producer needs a kick.
	[[nodiscard]] DequeueResult dequeueRecord(void *buffer, size_t maxSize, bool &kick) {
		kick = false;
		if(mode_ == Mode::dropOldest)
			return dequeueDropOldest_(buffer, maxSize);

		if(!refresh())
			return {DequeueStatus::violation};
		if(!availableSize())
			return {DequeueStatus::empty};
		if(availableSize() < sizeof(RecordHeader))
			return {DequeueStatus::violation};

		RecordHeader recordHeader;
		detail::copyFromRing(data_, ringSize_, position_, &recordHeader, sizeof(RecordHeader));
		if(recordHeader.size > ringSize_ || effectiveRecordSize(recordHeader.size) > availableSize())
			return {DequeueStatus::violation};
		detail::copyFromRing(data_, ringSize_, position_ + sizeof(RecordHeader),
				buffer, recordHeader.size < maxSize ? recordHeader.size : maxSize);

		kick = consume(effectiveRecordSize(recordHeader.size));
		return {DequeueStatus::success, recordHeader.size};
	}

	// Prepares to wait until watermark bytes are available or until the producer flushes.
	// Returns false if there is no need to wait.
	[[nodiscard]] bool armData(size_t watermark, bool &violation) {
		assert(watermark && watermark <= ringSize_);
		violation = false;
		header_->wakeAt.store(position_ + watermark, std::memory_order_seq_cst);

		auto head = producerHeader_->head.load(std::memory_order_seq_cst);
		if(mode_ == Mode::reliable) {
			violation = !refresh();
			if(violation)
				return false;
			head = head_;
		}
		if(head >= position_ + watermark)
			return false;
		auto flushed = producerHeader_->flushed.load(std::memory_order_seq_cst);
		return flushed <= position_;
	}

private:
	DequeueResult dequeueDropOldest_(void *buffer, size_t maxSize) {
		while(true) {
			// Find a valid position to dequeue from.
			auto beforeTail = producerHeader_->tail.load(std::memory_order_relaxed);
			if(position_ < beforeTail) {
				lost_ += beforeTail - position_;
				position_ = beforeTail;
			}

			auto head = producerHeader_->head.load(std::memory_order_acquire);
			if(position_ == head)
				return {DequeueStatus::empty};
			if(position_ > head || head - position_ < sizeof(RecordHeader))
				return {DequeueStatus::violation};

			RecordHeader recordHeader;
			detail::copyFromRing(data_, ringSize_, position_, &recordHeader, sizeof(RecordHeader));
			// The size is garbage if the record is overwritten concurrently.
			bool saneSize = recordHeader.size <= ringSize_
					&& effectiveRecordSize(recordHeader.size) <= head - position_;
			if(saneSize)
				detail::copyFromRing(data_, ringSize_, position_ + sizeof(RecordHeader),
						buffer, recordHeader.size < maxSize ? recordHeader.size : maxSize);

			// Validate the data *after* copying.
			auto afterTail = producerHeader_->tail.load(std::memory_order_acquire);
			if(position_ < afterTail)
				continue;
			if(!saneSize)
				return {DequeueStatus::violation};

			position_ += effectiveRecordSize(recordHeader.size);
			auto lost = lost_;
			lost_ = 0;
			return {DequeueStatus::success, recordHeader.size, lost};
		}
	}

	Mode mode_ = Mode::reliable;
	const ProducerHeader *producerHeader_ = nullptr;
	ConsumerHeader *header_ = nullptr;
	const std::byte *data_ = nullptr;
	size_t ringSize_ = 0;

	// The shared copy of our own position is never read back.
	uint64_t position_ = 0;
	// Validated copy of the producer's position.
	uint64_t head_ = 0;
	// Mode::dropOldest only: lost bytes that were not reported yet.
	uint64_t lost_ = 0;
};

} // namespace protocols::ringbuf
