#include <thor-internal/user-io.hpp>

namespace thor {

namespace {
	using protocols::ringbuf::Framing;
	using protocols::ringbuf::Mode;
	using protocols::ringbuf::Role;
}

std::expected<smarter::shared_ptr<UserIoChannel>, Error> UserIoChannel::create(
		frg::string<KernelAlloc> tag, frg::string<KernelAlloc> descriptiveTag,
		size_t outRingSize, size_t inRingSize) {
	if(outRingSize > maxRingSize || inRingSize > maxRingSize)
		return std::unexpected{Error::illegalArgs};

	auto ptr = smarter::allocate_shared<UserIoChannel>(*kernelAlloc, CtorToken{},
			std::move(tag), std::move(descriptiveTag));

	auto outRing = KernelRing::create({Mode::reliable, Framing::stream, outRingSize});
	if(!outRing)
		return std::unexpected{outRing.error()};
	ptr->outRing_ = std::move(*outRing);
	ptr->outCore_ = protocols::ringbuf::ProducerCore{Mode::reliable,
			ptr->outRing_.producerHeader(), ptr->outRing_.consumerHeader(),
			ptr->outRing_.data(), outRingSize};

	if(inRingSize) {
		auto inRing = KernelRing::create({Mode::reliable, Framing::stream, inRingSize});
		if(!inRing)
			return std::unexpected{inRing.error()};
		ptr->inRing_ = std::move(*inRing);
		ptr->inCore_ = protocols::ringbuf::ConsumerCore{Mode::reliable,
				ptr->inRing_->producerHeader(), ptr->inRing_->consumerHeader(),
				ptr->inRing_->data(), inRingSize};
	}

	ptr->updateSpans_();
	return ptr;
}

UserIoChannel::UserIoChannel(CtorToken, frg::string<KernelAlloc> tag,
		frg::string<KernelAlloc> descriptiveTag)
: KernelIoChannel{std::move(tag), std::move(descriptiveTag)} { }

coroutine<frg::expected<Error>>
UserIoChannel::provide(smarter::shared_ptr<Stream, LanePolicy> conversation) {
	frg::array<ProvidedKernelRing, 2> rings{{
		{&outRing_, Role::consumer},
		{inRing_ ? &*inRing_ : nullptr, Role::producer},
	}};
	auto lanes = FRG_CO_TRY(co_await provideRings(std::move(conversation),
			{rings.data(), inRing_ ? size_t{2} : size_t{1}}));
	lanes_ = std::move(lanes);
	co_return {};
}

void UserIoChannel::updateSpans_() {
	auto writable = outCore_.writableSpan();
	updateWritableSpan({writable.data, writable.size});
	if(inRing_) {
		auto readable = inCore_.readableSpan();
		updateReadableSpan({readable.data, readable.size});
	}
}

void UserIoChannel::produceOutput(size_t n) {
	auto oldHead = outCore_.head();
	outCore_.produce(n);
	if(outCore_.needsKick(outRing_.consumerHeader(), oldHead))
		lanes_.sender->kick();
	updateSpans_();
}

void UserIoChannel::consumeInput(size_t n) {
	if(inCore_.consume(n))
		lanes_.sender->kick();
	updateSpans_();
}

coroutine<frg::expected<Error>> UserIoChannel::issueIo(IoFlags flags) {
	if((flags & ioProgressInput) && !inRing_)
		co_return Error::illegalState;

	if(flags & ioFlush) {
		if(outCore_.flush() && outCore_.needsKickAfterFlush(outRing_.consumerHeader()))
			lanes_.sender->kick();
	}
	if(!(flags & (ioProgressOutput | ioProgressInput)))
		co_return {};

	// Progress means that one of the spans grows beyond its current size.
	// The spans cannot grow if the output ring is already empty (or the input ring full).
	auto outTarget = outCore_.freeSize() + 1;
	auto inTarget = inRing_ ? inCore_.availableSize() + 1 : 0;
	bool awaitOutput = (flags & ioProgressOutput) && outTarget <= outRing_.params.size;
	bool awaitInput = (flags & ioProgressInput) && inTarget <= inRing_->params.size;
	if(!awaitOutput && !awaitInput)
		co_return {};

	while(true) {
		auto sequence = lanes_.receiver->sequence();

		bool wait = true;
		bool violation = false;
		if(awaitOutput && !outCore_.armSpace(outTarget, violation))
			wait = false;
		if(violation)
			co_return Error::protocolViolation;
		if(awaitInput && !inCore_.armData(inTarget, violation))
			wait = false;
		if(violation)
			co_return Error::protocolViolation;
		updateSpans_();
		if(!wait)
			co_return {};

		if(lanes_.receiver->closed())
			co_return Error::endOfLane;
		co_await lanes_.receiver->waitPast(sequence);
	}
}

} // namespace thor
