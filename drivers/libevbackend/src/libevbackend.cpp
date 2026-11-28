#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/reboot.h>
#include <algorithm>
#include <deque>
#include <iostream>

#include <async/result.hpp>
#include <async/oneshot-event.hpp>
#include <helix/ipc.hpp>
#include <libevbackend.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include <frg/std_compat.hpp>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>

#include "fs.bragi.hpp"
#include "hw.bragi.hpp"
#include "libevbackend.hpp"

namespace libevbackend {

bool logConfiguration = false;
bool logCodes = false;
bool logRequests = false;

namespace {

constexpr bool isMultitouchCode(int code) {
	return code >= ABS_MT_FIRST && code <= ABS_MT_LAST;
}

constexpr void setBit(std::span<uint8_t> data, unsigned int bit) {
	assert(bit / 8 < data.size_bytes());
	data[bit / 8] |= (1 << (bit % 8));
}

async::detached issueReset() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "pm-interface"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	auto pmLane = (co_await entity.getRemoteLane()).unwrap();

	managarm::hw::RebootRequest hwRequest;
	hwRequest.set_cmd(RB_AUTOBOOT);

	auto [offer, hwSendResp, hwResp] = co_await helix_ng::exchangeMsgs(
		pmLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(hwRequest, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(hwSendResp.error());
	HEL_CHECK(hwResp.error());

	auto preamble = bragi::read_preamble(hwResp);
	assert(!preamble.error());

	auto resp = *bragi::parse_head_only<managarm::hw::SvrResponse>(hwResp);
	hwResp.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	bragi::limited_reader reader{tailBuffer.data(), tailBuffer.size()};
	auto ok = resp.decode_tail(reader);
	assert(ok);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	throw std::runtime_error("Return from PM_RESET request");
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// File implementation.
// ----------------------------------------------------------------------------

async::result<protocols::fs::ReadResult>
File::read(void *object, helix_ng::CredentialsView, void *buffer, size_t max_size,
		async::cancellation_token ct) {
	auto self = static_cast<File *>(object);

	// Make sure that we can at least write the SYN_DROPPED packet.
	if(max_size < sizeof(input_event))
		co_return std::unexpected{protocols::fs::Error::illegalArguments};

	if(self->_nonBlock && self->_pending.empty() && !self->_overflow)
		co_return std::unexpected{protocols::fs::Error::wouldBlock};

	while (self->_pending.empty() && !self->_overflow) {
		if (!co_await self->_statusBell.async_wait(ct))
			co_return std::unexpected{protocols::fs::Error::interrupted};
	}

	if(self->_overflow) {
		struct timespec now;
		if(clock_gettime(self->_clockId, &now))
			throw std::runtime_error("clock_gettime() failed");

		input_event uev;
		memset(&uev, 0, sizeof(input_event));
		uev.time.tv_sec = now.tv_sec;
		uev.time.tv_usec = now.tv_nsec / 1000;
		uev.type = EV_SYN;
		uev.code = SYN_DROPPED;
		memcpy(reinterpret_cast<char *>(buffer), &uev, sizeof(input_event));

		// Reset the overflow flag.
		self->_pending.clear();
		self->_overflow = false;

		co_return sizeof(input_event);
	}else{
		size_t written = 0;
		while(!self->_pending.empty()
				&& written + sizeof(input_event) <= max_size) {
			auto evt = self->_pending.front();
			self->_pending.pop_front();
			if(self->_pending.empty())
				self->_statusPage.update(self->_currentSeq, 0);

			input_event uev;
			memset(&uev, 0, sizeof(input_event));
			uev.time.tv_sec = evt.timestamp.tv_sec;
			uev.time.tv_usec = evt.timestamp.tv_nsec / 1000;
			uev.type = evt.type;
			uev.code = evt.code;
			uev.value = evt.value;
			memcpy(reinterpret_cast<char *>(buffer) + written, &uev, sizeof(input_event));
			written += sizeof(input_event);
		}

		assert(written);
		co_return written;
	}
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
File::pollWait(void *object, uint64_t past_seq, int mask,
		async::cancellation_token c) {
	auto self = static_cast<File *>(object);
	int edges = 0;

	assert(past_seq <= self->_currentSeq);

	while(true) {
		edges = 0;
		if (self->_currentSeq > past_seq)
			edges |= EPOLLIN | EPOLLRDNORM;

		if (edges & mask)
			break;

		if (!co_await self->_statusBell.async_wait(c))
			break;
	}

	co_return protocols::fs::PollWaitResult{
		self->_currentSeq,
		edges & mask
	};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
File::pollStatus(void *object) {
	auto self = static_cast<File *>(object);

	co_return protocols::fs::PollStatusResult{
		self->_currentSeq,
		self->_pending.empty() ? 0 : EPOLLIN | EPOLLRDNORM
	};
}

struct File::HandleIoctl {
	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, File *self) {
		if(req.command() == EVIOCGBIT(0, 0)) {
			assert(req.size());
			if(logRequests)
				std::cout << "EVIOCGBIT()" << std::endl;

			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto chunk = std::min(size_t(req.size()), self->_device->_typeBits.size());
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(self->_device->_typeBits.data(), chunk)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.command() == EVIOCGBIT(1, 0)) {
			assert(req.size());
			if(logRequests)
				std::cout << "EVIOCGBIT(" << req.input_type() << ")" << std::endl;

			managarm::fs::GenericIoctlReply resp;

			std::pair<const uint8_t *, size_t> p;
			if(req.input_type() == EV_KEY) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {self->_device->_keyBits.data(), self->_device->_keyBits.size()};
			}else if(req.input_type() == EV_REL) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {self->_device->_relBits.data(), self->_device->_relBits.size()};
			}else if(req.input_type() == EV_ABS) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {self->_device->_absBits.data(), self->_device->_absBits.size()};
			}else{
				resp.set_error(managarm::fs::Errors::SUCCESS);
				p = {nullptr, 0};
			}

			auto ser = resp.SerializeAsString();
			auto chunk = std::min(size_t(req.size()), p.second);
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(p.first, chunk)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.command() == EVIOCSCLOCKID) {
			managarm::fs::GenericIoctlReply resp;

			// TODO: Does this setting affect already queued events in Linux?
			switch(req.input_clock()) {
			case CLOCK_REALTIME:
			case CLOCK_MONOTONIC:
				self->_clockId = req.input_clock();
				resp.set_error(managarm::fs::Errors::SUCCESS);
				break;
			default:
				assert(!"Clock is not supported in libevbackend");
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req.command() == EVIOCGABS(0)) {
			managarm::fs::GenericIoctlReply resp;
			if(logRequests)
				std::cout << "EVIOCGABS(" << req.input_type() << ")" << std::endl;

			assert(static_cast<size_t>(req.input_type())
					< self->_device->_absoluteSlots.size());
			auto slot = &self->_device->_absoluteSlots[req.input_type()];
			resp.set_input_value(slot->value);
			resp.set_input_min(slot->minimum);
			resp.set_input_max(slot->maximum);
			resp.set_input_fuzz(0);
			resp.set_input_flat(0);
			resp.set_input_resolution(slot->resolution.value_or(0));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else{
			std::cout << "Unknown ioctl() with ID " << std::to_string(req.command()) << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		co_return {};
	}

	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::EvioGetNameRequest &&, helix::BorrowedDescriptor conversation, bragi::preamble, File *self) {
		managarm::fs::EvioGetNameReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_name(self->_device->name_);

		auto [send_head, send_tail] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_head.error());
		HEL_CHECK(send_tail.error());

		co_return {};
	}

	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::EvioGetIdRequest &&, helix::BorrowedDescriptor conversation, bragi::preamble, File *self) {
		managarm::fs::EvioGetIdReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_bustype(self->_device->busType_);
		resp.set_vendor(self->_device->vendor_);
		resp.set_product(self->_device->product_);
		resp.set_version(1);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());

		co_return {};
	}

	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::EvioGetMultitouchSlotsRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, File *self) {
		managarm::fs::EvioGetMultitouchSlotsReply resp;

		for(auto &e : self->_device->_mtState) {
			resp.add_values(e.second.abs.at(req.code() - ABS_MT_FIRST));
		}

		assert(resp.values_size() < maxMultitouchSlots);

		for(size_t i = 0; i < (maxMultitouchSlots - resp.values_size()); i++) {
			if(req.code() == ABS_MT_TRACKING_ID)
				resp.add_values(-1);
			else
				resp.add_values(0);
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto [send_head, send_tail] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_head.error());
		HEL_CHECK(send_tail.error());

		co_return {};
	}
};

async::result<void>
File::ioctl(void *object, uint32_t, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<File *>(object);


	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest,
		managarm::fs::EvioGetNameRequest,
		managarm::fs::EvioGetIdRequest,
		managarm::fs::EvioGetMultitouchSlotsRequest
	>(conversation, std::move(msg), HandleIoctl{}, self);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

async::result<int> File::getFileFlags(void *object) {
	auto self = static_cast<File *>(object);
	int flags = O_RDWR;
	if(self->_nonBlock)
		flags |= O_NONBLOCK;
	co_return flags;
}

async::result<void> File::setFileFlags(void *object, int flags) {
	auto self = static_cast<File *>(object);
	if (flags & ~O_NONBLOCK) {
		std::cout << "libevbackend: setFileFlags on evdev event file called with unknown flags" << std::endl;
	}
	if (flags & O_NONBLOCK)
		self->_nonBlock = true;
	else
		self->_nonBlock = false;
	co_return;
}


constexpr auto fileOperations = protocols::fs::FileOperations{
	.read = &File::read,
	.ioctl = &File::ioctl,
	.pollWait = &File::pollWait,
	.pollStatus = &File::pollStatus,
	.getFileFlags = &File::getFileFlags,
	.setFileFlags = &File::setFileFlags
};

helix::UniqueLane File::serve(smarter::shared_ptr<File> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(
			std::move(local_lane), file, &fileOperations));
	return remote_lane;
}

File::File(EventDevice *device, bool non_block)
: _device{device}, _currentSeq{1}, _nonBlock{non_block}, _clockId{CLOCK_MONOTONIC} {
	_statusPage.update(_currentSeq, 0);
}

File::~File() {
	// TODO: This should probably be done in an explicit handleClose().
	_device->files_.erase(_device->files_.iterator_to(this));
}

// ----------------------------------------------------------------------------
// EventDevice implementation.
// ----------------------------------------------------------------------------

struct HandleDevice {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, EventDevice *device) {
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			auto file = smarter::make_shared<File>(device,
					req.flags() & managarm::fs::OpenFlags::OF_NONBLOCK);
			device->files_.push_back(file.get());
			auto remote_lane = File::serve(file);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FileCaps::FC_STATUS_PAGE);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_pt, push_page] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage),
				helix_ng::pushDescriptor(file->_statusPage.getMemory(), kHelRightRead | kHelRightAssign)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		}else{
			throw std::runtime_error("Invalid serveDevice request!");
		}
		co_return {};
	}
};

async::detached serveDevice(std::shared_ptr<EventDevice> device,
		helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest
		>(lane, HandleDevice{}, device.get());
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "libevbackend: dispatch error" << std::endl;
			continue;
		}
	}
}

EventDevice::EventDevice(std::string name, uint16_t bustype, uint16_t vendor, uint16_t product)
: name_{std::move(name)}, busType_{bustype}, vendor_{vendor}, product_{product} {
	memset(_typeBits.data(), 0, _typeBits.size());
	memset(_keyBits.data(), 0, _keyBits.size());
	memset(_relBits.data(), 0, _relBits.size());
	memset(_absBits.data(), 0, _absBits.size());

	// Linux advertises EV_SYN for every input device.
	setBit(_typeBits, EV_SYN);

	memset(_currentKeys.data(), 0, _currentKeys.size());
	memset(_absoluteSlots.data(), 0, _absoluteSlots.size() * sizeof(AbsoluteSlot));
}

void EventDevice::setAbsoluteDetails(int code, int minimum, int maximum) {
	assert(static_cast<size_t>(code) < _absoluteSlots.size());
	_absoluteSlots[code].minimum = minimum;
	_absoluteSlots[code].maximum = maximum;
}

void EventDevice::setResolution(int code, int res) {
	assert(static_cast<size_t>(code) < _absoluteSlots.size());
	_absoluteSlots[code].resolution = res;
}

void EventDevice::enableEvent(int type, int code) {
	if(logConfiguration)
		std::cout << "drivers/libevbackend: Enabling event " << type << "." << code
				<< std::endl;

	if(type == EV_KEY) {
		setBit(_keyBits, code);
	}else if(type == EV_REL) {
		setBit(_relBits, code);
	}else if(type == EV_ABS) {
		setBit(_absBits, code);
	}else{
		throw std::runtime_error("Unexpected event type");
	}
	setBit(_typeBits, type);
}

void EventDevice::emitEvent(int type, int code, int value) {
	auto getBit = [] (uint8_t *array, size_t length, unsigned int bit) -> bool {
		assert(bit / 8 < length);
		return array[bit / 8] & (1 << (bit % 8));
	};
	auto putBit = [] (uint8_t *array, size_t length, unsigned int bit, bool value) {
		assert(bit / 8 < length);
		array[bit / 8] &= ~(1 << (bit % 8));
		array[bit / 8] |= (((int)value) << (bit % 8));
	};

	auto currentMtSlot = [this]() -> int & {
		return _absoluteSlots[ABS_MT_SLOT].value;
	};

	// Filter out events that do not update the device state.
	if(type == EV_KEY && getBit(_currentKeys.data(), _currentKeys.size(), code) == value)
		return;
	if(type == EV_REL && !value)
		return;
	if(type == EV_ABS && isMultitouchCode(code)) {
		// ABS_MT_SLOT is treated like global state, while other ABS_MT_* values are local to a slot
		if(code == ABS_MT_SLOT && _absoluteSlots[code].value == value)
			return;

		if(_mtState.contains(currentMtSlot()) && code != ABS_MT_SLOT) {
			assert(std::cmp_greater(_mtState.at(currentMtSlot()).abs.size(), (code - ABS_MT_FIRST)));
			if(_mtState.at(currentMtSlot()).abs.at(code - ABS_MT_FIRST) == value)
				return;
		}
	} else if(type == EV_ABS && value == _absoluteSlots[code].value) {
		return;
	}

	// Update the device state.
	if(type == EV_KEY) {
		putBit(_currentKeys.data(), _currentKeys.size(), code, value);
	}else if(type == EV_ABS) {
		if(isMultitouchCode(code)) {
			if(code == ABS_MT_SLOT)
				currentMtSlot() = value;

			if(!_mtState.contains(currentMtSlot())) {
				_mtState.insert({currentMtSlot(), {}});
			}

			if(code == ABS_MT_TRACKING_ID && value == -1) {
				auto removed_items = _mtState.erase(currentMtSlot());
				assert(removed_items == 1);
			} else {
				if(code == ABS_MT_TRACKING_ID && value != -1)
					_mtState.at(currentMtSlot()).userTrackingId = value;

				assert(_mtState.contains(currentMtSlot()));
				_mtState.at(currentMtSlot()).abs.at(code - ABS_MT_FIRST) = value;
			}
		} else {
			_absoluteSlots[code].value = value;
		}
	}

	// Handle magic key sequences in the driver.  This ensure that all devices implement
	// the same magic keys. It is also more reliable than implementing this in a second process.
	static bool resetSent = false;
	if(!resetSent
			&& getBit(_currentKeys.data(), _currentKeys.size(), KEY_LEFTCTRL)
			&& getBit(_currentKeys.data(), _currentKeys.size(), KEY_LEFTALT)
			&& getBit(_currentKeys.data(), _currentKeys.size(), KEY_DELETE)) {
		std::cout << "drivers/libevbackend: Issuing CTRL+ALT+DEL reset" << std::endl;
		issueReset();
		resetSent = true;
	}

	_staged.push_back(StagedEvent{type, code, value});
}

void EventDevice::notify() {
	if(_staged.empty())
		return;

	for(auto file : files_) {
		if(file->_overflow)
			continue;

		struct timespec now;
		if(clock_gettime(file->_clockId, &now))
			throw std::runtime_error("clock_gettime() failed");

		if(file->_pending.size() > 1024) {
			file->_overflow = true;
			continue;
		}

		if(logCodes)
			for(StagedEvent evt : _staged)
				std::cout << "[" << now.tv_sec << "." << (now.tv_nsec / 1'000'000)
						<< "] Event type: " << evt.type << ", code: " << evt.code
						<< ", value: " << evt.value << std::endl;

		for(StagedEvent evt : _staged)
			file->_pending.push_back(PendingEvent{evt.type, evt.code, evt.value, now});
		file->_currentSeq++;
		file->_statusPage.update(file->_currentSeq, EPOLLIN | EPOLLRDNORM);
		file->_statusBell.raise();
	}
	_staged.clear();
}

} // namespace libevbackend
