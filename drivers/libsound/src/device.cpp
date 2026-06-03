#include "sound.hpp"
#include <protocols/posix/procdata.hpp>

#include <protocols/mbus/client.hpp>
#include <protocols/fs/server.hpp>

#include <async/recurring-event.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>

#include <linux/types.h>
#include <sound/asound.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>

#include <print>
#include <functional>

namespace {

constexpr bool logRequests = true;
constexpr bool logWrites = false;

}

namespace alsa {

struct DeviceFile {
	static async::result<void>
	ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t sequence, int mask,
			async::cancellation_token cancellation);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);

	static async::result<helix::BorrowedDescriptor> accessMemory(void *object);

	static async::result<int> getFileFlags(void *object) {
		auto self = static_cast<DeviceFile *>(object);

		int flags = O_RDWR;
		if(self->nonBlock)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	static async::result<void> setFileFlags(void *object, int flags) {
		auto self = static_cast<DeviceFile *>(object);

		if(flags & ~O_NONBLOCK) {
			std::println(std::cout, "libsound: setFileFlags with unknown flags {:#x}", flags);
			co_return;
		}

		if(flags & O_NONBLOCK)
			self->nonBlock = true;
		else
		 	self->nonBlock = false;
	}

	static helix::UniqueLane serve(smarter::shared_ptr<DeviceFile> file);

	DeviceFile(sound::Device *device, bool non_block) : device{device}, nonBlock{non_block} {
		auto pageSize = getpagesize();

		size_t statusSize = (sizeof(snd_pcm_mmap_status) + pageSize - 1) & ~(pageSize - 1);
		size_t controlSize = (sizeof(snd_pcm_mmap_control) + pageSize - 1) & ~(pageSize - 1);

		HelHandle handle;
		HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW + controlSize, kHelAllocOnDemand, nullptr, &handle));

		memory = helix::UniqueDescriptor{handle};

		statusMapping = helix::Mapping{memory, SNDRV_PCM_MMAP_OFFSET_STATUS, statusSize};
		controlMapping = helix::Mapping{memory, SNDRV_PCM_MMAP_OFFSET_CONTROL, controlSize};

		status = static_cast<snd_pcm_mmap_status *>(statusMapping.get());
		control = static_cast<snd_pcm_mmap_control *>(controlMapping.get());

		*status = {};
		*control = {};
	}

	sound::Device *device;
	helix::UniqueDescriptor memory;
	helix::Mapping statusMapping;
	helix::Mapping controlMapping;
	snd_pcm_mmap_status *status;
	snd_pcm_mmap_control *control;
	bool nonBlock;
	snd_pcm_hw_params hwParams{};
	snd_pcm_sw_params swParams{};
	size_t frameSize{};
	uint32_t lastRemaining{};
	uint64_t eventSequence{};
	async::recurring_event eventBell;
};

struct Params {
	constexpr snd_mask &mask(uint32_t what) {
		return values.masks[what - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	}

	constexpr snd_interval &interval(uint32_t what) {
		return values.intervals[what - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	}

	constexpr void change(uint32_t what) {
		values.cmask |= 1U << what;
	}

	[[nodiscard]] constexpr bool intersect_interval(uint32_t what, snd_interval other) {
		auto &range = interval(what);

		bool changed = false;

		if (other.min > range.min) {
			range.min = other.min;
			range.openmin = other.openmin;
			changed = true;
		} else if (other.min == range.min) {
			changed |= other.openmin && !range.openmin;
			range.openmin |= other.openmin;
		}

		if (other.max < range.max) {
			range.max = other.max;
			range.openmax = other.openmax;
			changed = true;
		} else if (other.max == range.max) {
			changed |= other.openmax && !range.openmax;
			range.openmax |= other.openmax;
		}

		changed |= other.integer && !range.integer;
		range.integer |= other.integer;

		if (range.min > range.max ||
				(range.min == range.max && (range.openmin || range.openmax))) {
			changed |= !range.empty;
			range.empty = 1;
		} else {
			changed |= range.empty;
			range.empty = 0;
		}

		return changed;
	}

	snd_pcm_hw_params values;
};

struct Rule {
	template<size_t N>
	Rule(const uint32_t (&deps)[N], std::function<void (Params &params)> func)
			: func{std::move(func)} {
		for (auto dep : deps) {
			dependencies |= 1U << dep;
		}
	}

	uint32_t dependencies{};
	std::function<void (Params &params)> func;
};

namespace rules {

Rule sampleBitsRule{
	{SNDRV_PCM_HW_PARAM_FORMAT},
	[](Params &params) {
		auto &formats = params.mask(SNDRV_PCM_HW_PARAM_FORMAT);

		auto has_format = [&](uint32_t format) {
			return formats.bits[0] & (1U << format);
		};

		snd_interval constraint{};
		constraint.min = UINT32_MAX;
		constraint.integer = 1;

		if (has_format(SNDRV_PCM_FORMAT_S8)) {
			constraint.min = 8;
			constraint.max = 8;
		}
		if (has_format(SNDRV_PCM_FORMAT_S16_LE)) {
			constraint.min = std::min(constraint.min, 16U);
			constraint.max = std::max(constraint.max, 16U);
		}
		if (has_format(SNDRV_PCM_FORMAT_S32_LE)) {
			constraint.min = std::min(constraint.min, 32U);
			constraint.max = std::max(constraint.max, 32U);
		}

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_SAMPLE_BITS, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
		}
	}
};

Rule frameBitsRule{
	{SNDRV_PCM_HW_PARAM_SAMPLE_BITS, SNDRV_PCM_HW_PARAM_CHANNELS},
	[](Params &params) {
		auto &sampleBits = params.interval(SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
		auto &channels = params.interval(SNDRV_PCM_HW_PARAM_CHANNELS);

		snd_interval constraint{};
		constraint.integer = sampleBits.integer && channels.integer;
		constraint.openmin = sampleBits.openmin || channels.openmin;
		constraint.openmax = sampleBits.openmax || channels.openmax;
		constraint.min = sampleBits.min * channels.min;
		constraint.max = sampleBits.max * channels.max;

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_FRAME_BITS, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_FRAME_BITS);
		}
	}
};

constexpr uint32_t usInSecond = 1000 * 1000;

Rule periodTimeRule{
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		snd_interval constraint{};
		constraint.integer = 0;
		auto minMul = static_cast<uint64_t>(periodSize.min) * rate.min;
		auto maxMul = static_cast<uint64_t>(periodSize.max) * rate.max;

		constraint.min = minMul / usInSecond;
		constraint.openmin = minMul % usInSecond || periodSize.openmin || rate.openmin;
		constraint.max = maxMul / usInSecond;
		if (maxMul % usInSecond) {
			++constraint.max;
			constraint.openmax = 1;
		} else {
			constraint.openmax = periodSize.openmax || rate.openmax;
		}

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_TIME, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_PERIOD_TIME);
		}
	}
};

Rule periodSizeRule{
	{SNDRV_PCM_HW_PARAM_PERIOD_TIME, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &periodTime = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_TIME);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		snd_interval constraint{};
		constraint.integer = 0;
		auto minMul = static_cast<uint64_t>(periodTime.min) * rate.min;
		auto maxMul = static_cast<uint64_t>(periodTime.max) * rate.max;

		constraint.min = minMul / usInSecond;
		constraint.openmin = minMul % usInSecond || periodTime.openmin || rate.openmin;
		constraint.max = maxMul / usInSecond;
		if (maxMul % usInSecond) {
			++constraint.max;
			constraint.openmax = 1;
		} else {
			constraint.openmax = periodTime.openmax || rate.openmax;
		}

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		}
	}
};

Rule periodSizeFromBufferRule{
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIODS},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &periods = params.interval(SNDRV_PCM_HW_PARAM_PERIODS);

		snd_interval constraint{};
		constraint.integer = 0;

		constraint.min = bufferSize.min / periods.max;
		constraint.openmin = bufferSize.min % periods.max || bufferSize.openmin || periods.openmax;
		if (periods.min) {
			constraint.max = bufferSize.max / periods.min;
			if (bufferSize.max % periods.min) {
				++constraint.max;
				constraint.openmax = 1;
			} else {
				constraint.openmax = bufferSize.openmax || periods.openmin;
			}
		} else {
			constraint.max = UINT32_MAX;
			constraint.openmax = 0;
		}

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		}
	}
};

Rule periodBytesRule{
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_FRAME_BITS},
	[](Params &params) {
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		auto &frameBits = params.interval(SNDRV_PCM_HW_PARAM_FRAME_BITS);

		snd_interval constraint{};
		constraint.integer = periodSize.integer && frameBits.integer;
		constraint.openmin = periodSize.openmin || frameBits.openmin;
		constraint.openmax = periodSize.openmax || frameBits.openmax;
		constraint.min = periodSize.min * (frameBits.min / 8);
		constraint.max = periodSize.max * (frameBits.max / 8);

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
		}
	}
};

Rule periodsRule{
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIOD_SIZE},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);

		snd_interval constraint{};
		constraint.integer = 0;

		constraint.min = bufferSize.min / periodSize.max;
		constraint.openmin = bufferSize.min % periodSize.max || bufferSize.openmin || periodSize.openmax;
		if (periodSize.min) {
			constraint.max = bufferSize.max / periodSize.min;
			if (bufferSize.max % periodSize.min) {
				++constraint.max;
				constraint.openmax = 1;
			} else {
				constraint.openmax = bufferSize.openmax || periodSize.openmin;
			}
		} else {
			constraint.max = UINT32_MAX;
			constraint.openmax = 0;
		}

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_PERIODS, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_PERIODS);
		}
	}
};

Rule bufferTimeRule{
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_RATE},
	[](Params &params) {
		auto &bufferSize = params.interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		auto &rate = params.interval(SNDRV_PCM_HW_PARAM_RATE);

		snd_interval constraint{};
		constraint.integer = 0;
		auto minMul = static_cast<uint64_t>(bufferSize.min) * rate.min;
		auto maxMul = static_cast<uint64_t>(bufferSize.max) * rate.max;

		constraint.min = minMul / usInSecond;
		constraint.openmin = minMul % usInSecond || bufferSize.openmin || rate.openmin;
		constraint.max = maxMul / usInSecond;
		if (maxMul % usInSecond) {
			++constraint.max;
			constraint.openmax = 1;
		} else {
			constraint.openmax = bufferSize.openmax || rate.openmax;
		}

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_TIME, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_BUFFER_TIME);
		}
	}
};

Rule bufferSizeRule{
	{SNDRV_PCM_HW_PARAM_PERIODS, SNDRV_PCM_HW_PARAM_PERIOD_SIZE},
	[](Params &params) {
		auto &periods = params.interval(SNDRV_PCM_HW_PARAM_PERIODS);
		auto &periodSize = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_SIZE);

		snd_interval constraint{};
		constraint.integer = periods.integer && periodSize.integer;
		constraint.openmin = periods.openmin || periodSize.openmin;
		constraint.openmax = periods.openmax || periodSize.openmax;
		constraint.min = periods.min * periodSize.min;
		constraint.max = periods.max * periodSize.max;

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_SIZE, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
		}
	}
};

Rule bufferBytesRule{
	{SNDRV_PCM_HW_PARAM_PERIODS, SNDRV_PCM_HW_PARAM_PERIOD_BYTES},
	[](Params &params) {
		auto &periods = params.interval(SNDRV_PCM_HW_PARAM_PERIODS);
		auto &periodBytes = params.interval(SNDRV_PCM_HW_PARAM_PERIOD_BYTES);

		snd_interval constraint{};
		constraint.integer = periods.integer && periodBytes.integer;
		constraint.openmin = periods.openmin || periodBytes.openmin;
		constraint.openmax = periods.openmax || periodBytes.openmax;
		constraint.min = periods.min * periodBytes.min;
		constraint.max = periods.max * periodBytes.max;

		if (params.intersect_interval(SNDRV_PCM_HW_PARAM_BUFFER_BYTES, constraint)) {
			params.change(SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
		}
	}
};

std::pair<uint32_t, Rule*> rules[]{
	{SNDRV_PCM_HW_PARAM_SAMPLE_BITS, &sampleBitsRule},
	{SNDRV_PCM_HW_PARAM_FRAME_BITS, &frameBitsRule},
	{SNDRV_PCM_HW_PARAM_PERIOD_TIME, &periodTimeRule},
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &periodSizeRule},
	{SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &periodSizeFromBufferRule},
	{SNDRV_PCM_HW_PARAM_PERIOD_BYTES, &periodBytesRule},
	{SNDRV_PCM_HW_PARAM_PERIODS, &periodsRule},
	{SNDRV_PCM_HW_PARAM_BUFFER_TIME, &bufferTimeRule},
	{SNDRV_PCM_HW_PARAM_BUFFER_SIZE, &bufferSizeRule},
	{SNDRV_PCM_HW_PARAM_BUFFER_BYTES, &bufferBytesRule}
};

} // namespace rules

async::result<void> DeviceFile::ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<DeviceFile *>(object);

	if (id != managarm::fs::GenericIoctlRequest::message_id) {
		std::println(std::cout, "libsound: unknown device ioctl() message with ID {}", id);
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
		co_return;
	}

	auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
	assert(req);
	if (req->command() == SNDRV_PCM_IOCTL_INFO) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query pcm info");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		snd_pcm_info info{};
		// TODO: get the device index somehow
		info.device = 0;
		info.subdevice = 0;
		info.stream = self->device->type == sound::DeviceType::playback ? SNDRV_PCM_STREAM_PLAYBACK : SNDRV_PCM_STREAM_CAPTURE;
		// TODO: get the card index somehow
		info.card = 0;
		memcpy(info.id, "Managarm Sound Device", sizeof("Managarm Sound Device"));
		memcpy(info.name, "Managarm Sound Device", sizeof("Managarm Sound Device"));
		memcpy(info.subname, "subdevice #0", sizeof("subdevice #0"));
		info.dev_class = 0;
		info.dev_subclass = 0;
		info.subdevices_count = 1;
		info.subdevices_avail = 0;

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&info, sizeof(info))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_PVERSION) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: query pcm version");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);
		resp.set_snd_pversion(SNDRV_PROTOCOL_VERSION(2, 0, 18));

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_USER_PVERSION) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm user pversion {} is ignored", req->snd_pcm_user_pversion());
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_TTSTAMP) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm ttstamp {} is ignored", req->snd_pcm_ttstamp());
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HW_REFINE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: refine pcm hw params");
		}

		Params params{};

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&params.values, sizeof(params.values))
		);
		HEL_CHECK(recv_buffer.error());

		uint32_t rmask = params.values.rmask;
		uint32_t cmask = 0;

		while (true) {
			params.values.cmask = 0;

			for (auto [index, rule] : rules::rules) {
				if (rmask & rule->dependencies) {
					rule->func(params);
				}
			}

			rmask = params.values.cmask;
			if (rmask == 0) {
				break;
			}
			cmask |= rmask;
		}

		params.values.cmask = cmask;

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&params, sizeof(params))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HW_PARAMS) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: set pcm hw params");
		}

		snd_pcm_hw_params params;

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&params, sizeof(params))
		);
		HEL_CHECK(recv_buffer.error());

		// TODO: verify parameters

		self->hwParams = params;
		self->frameSize = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_FRAME_BITS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min / 8;

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&params, sizeof(params))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_SW_PARAMS) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: set pcm sw params");
		}

		snd_pcm_sw_params params;

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(&params, sizeof(params))
		);
		HEL_CHECK(recv_buffer.error());

		// TODO: verify parameters

		self->swParams = params;

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(&params, sizeof(params))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_PREPARE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm prepare");
		}

		if (self->status->state == SNDRV_PCM_STATE_OPEN) {
			auto &fmtBits = self->hwParams.masks[SNDRV_PCM_HW_PARAM_FORMAT].bits;

			auto checkFmt = [&](snd_pcm_format_t fmt) {
				return fmtBits[fmt / 32] & 1U << (fmt % 32);
			};

			sound::Format fmt{};

			if (checkFmt(SNDRV_PCM_FORMAT_S8) || checkFmt(SNDRV_PCM_FORMAT_U8)) {
				fmt = sound::Format::pcmS8;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S16_LE)) {
				fmt = sound::Format::pcmS16;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S20_LE)) {
				fmt = sound::Format::pcmS20;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S24_LE)) {
				fmt = sound::Format::pcmS24;
			} else if (checkFmt(SNDRV_PCM_FORMAT_S32_LE)) {
				fmt = sound::Format::pcmS32;
			} else {
				std::println(std::cout, "libsound: unsupported alsa format specified");
				assert(!"Unsupported format specified");
			}

			auto bufferSize = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_BUFFER_BYTES - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
			assert(bufferSize);
			assert(self->frameSize);
			std::println(std::cout, "libsound: buffer size {} frame size {}", bufferSize, self->frameSize);

			sound::StreamParameters streamParams{
				.sampleRate = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
				.channels = self->hwParams.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
				.bufferSize = bufferSize,
				.format = fmt
			};

			std::println(std::cout, "libsound: sample rate {}, channels {}", streamParams.sampleRate, streamParams.channels);

			sound::Stream *stream;
			if (self->device->type == sound::DeviceType::playback) {
				stream = self->device->card->findFreePlaybackStream();
				assert(stream);
			} else {
				stream = self->device->card->findFreeCaptureStream();
				assert(stream);
			}

			auto status = stream->setup(streamParams);
			assert(status);

			status = self->device->attachToStream(stream);
			assert(status);
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		self->status->state = SNDRV_PCM_STATE_PREPARED;

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_WRITEI_FRAMES) {
		auto frames = req->snd_pcm_frames();

		if constexpr (logWrites) {
			std::println(std::cout, "libsound: pcm writei frames {}", frames);
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_snd_frame_size(self->frameSize);

		auto [send_info_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_info_resp.error());

		std::vector<uint8_t> data(frames * self->frameSize);

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(data.data(), data.size())
		);
		HEL_CHECK(recv_buffer.error());

		assert(self->device->attachedStream);
		auto stream = self->device->attachedStream;

		assert(data.size() <= UINT32_MAX);
		uint32_t size = data.size();
		auto result = stream->queueData(data.data(), size);
		assert(result);
		size = result.value();

		if (stream->isPaused) {
			result = stream->getRemaining();
			assert(result);

			if (result.value() >= self->control->avail_min * self->frameSize) {
				std::println(std::cout, "libsound: starting stream (trip threshold {})", self->control->avail_min * self->frameSize);
				auto status = stream->play();
				assert(status);
				self->status->state = SNDRV_PCM_STATE_RUNNING;
			}
		}

		resp.set_snd_result(size / self->frameSize);
		resp.set_result(0);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_DROP) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm drop");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		if (self->status->state != SNDRV_PCM_STATE_OPEN) {
			assert(self->device->attachedStream);

			auto stream = self->device->attachedStream;
			auto status = stream->pause();
			assert(status);
			status = stream->clearData();
			assert(status);
		}

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if (req->command() == SNDRV_PCM_IOCTL_HW_FREE) {
		if constexpr (logRequests) {
			std::println(std::cout, "libsound: pcm hw free");
		}

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(0);

		if (auto stream = self->device->attachedStream) {
			auto status = stream->pause();
			assert(status);
			status = stream->stop();
			assert(status);
		}

		self->status->state = SNDRV_PCM_STATE_OPEN;

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else {
		std::println(std::cout, "Unknown ioctl() message with ID {}", req->command());
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
DeviceFile::pollWait(void *object, uint64_t sequence, int mask,
		async::cancellation_token cancellation) {
	(void) mask;
	(void) cancellation;

	auto self = static_cast<DeviceFile *>(object);

	if(sequence > self->eventSequence)
		co_return protocols::fs::Error::illegalArguments;

	// Wait until we surpass the input sequence.
	while(sequence == self->eventSequence)
		co_await self->eventBell.async_wait_if([&] { return sequence == self->eventSequence; });

	uint32_t bufferSize = self->device->attachedStream->params.bufferSize;
	auto result = self->device->attachedStream->getRemaining();
	assert(result);

	int s = 0;

	if(bufferSize - result.value() >= self->swParams.boundary)
		s |= EPOLLOUT;

	co_return protocols::fs::PollWaitResult{self->eventSequence, s};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
DeviceFile::pollStatus(void *object) {
	auto self = static_cast<DeviceFile *>(object);

	uint32_t bufferSize = self->device->attachedStream->params.bufferSize;
	auto result = self->device->attachedStream->getRemaining();
	assert(result);

	int s = 0;

	if(bufferSize - result.value() >= self->swParams.boundary)
		s |= EPOLLOUT;

	co_return protocols::fs::PollStatusResult{self->eventSequence, s};
}

async::result<helix::BorrowedDescriptor> DeviceFile::accessMemory(void *object) {
	auto self = static_cast<DeviceFile *>(object);
	co_return self->memory;
}

constexpr auto fileOperations = protocols::fs::FileOperations{
	.accessMemory = &DeviceFile::accessMemory,
	.ioctl = &DeviceFile::ioctl,
	.pollWait = &DeviceFile::pollWait,
	.pollStatus = &DeviceFile::pollStatus,
	.getFileFlags = &DeviceFile::getFileFlags,
	.setFileFlags = &DeviceFile::setFileFlags
};

helix::UniqueLane DeviceFile::serve(smarter::shared_ptr<DeviceFile> file) {
	helix::UniqueLane local_lane, remote_lane;
	std::tie(local_lane, remote_lane) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(
			std::move(local_lane), file, &fileOperations));
	return remote_lane;
}

async::detached serveDevice(sound::Device *device,
		helix::UniqueLane lane) {
	std::cout << "alsa device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		recv_req.reset();
		if (req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			auto file = smarter::make_shared<DeviceFile>(device,
					req.flags() & managarm::fs::OpenFlags::OF_NONBLOCK);
			auto remote_lane = DeviceFile::serve(file);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp, push_pt] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
		} else {
			throw std::runtime_error("Invalid serveDevice request!");
		}
	}
}

} // namespace alsa

namespace sound {

async::detached runDevice(Device *device) {
	mbus_ng::Properties soundDeviceDescriptor{
		{"unix.subsystem", mbus_ng::StringItem{"sound"}},
		{"drvcore.mbus-parent", mbus_ng::StringItem(std::to_string(device->parentId))},
		{"sound.type", mbus_ng::StringItem{device->type == sound::DeviceType::playback ? "playback" : "capture"}},
		{"class", mbus_ng::StringItem{"sound-device"}}
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
			"sound-device", soundDeviceDescriptor)).unwrap();

	std::println(std::cout, "libsound: Serving device");

	while (true) {
		auto [localLane, remoteLane] = helix::createStream();

		// If this fails, too bad!
		(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

		alsa::serveDevice(device, std::move(localLane));
	}
}

} // namespace sound
