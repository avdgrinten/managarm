#include "io-dispatcher.hpp"

namespace blockfs {

void FifoDispatcher::submit(IoRequest *rq) {
	fifo_.push_back(rq);
}

IoRequest *FifoDispatcher::peek() {
	return fifo_.empty() ? nullptr : fifo_.front();
}

IoRequest *FifoDispatcher::dispatch() {
	if(fifo_.empty())
		return nullptr;
	auto rq = fifo_.front();
	fifo_.pop_front();
	return rq;
}

} // namespace blockfs
