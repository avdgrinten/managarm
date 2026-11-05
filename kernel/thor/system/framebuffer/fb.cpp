
#include <render-text.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/mbus.hpp>

#include <thor-internal/framebuffer/fb.hpp>
#include <thor-internal/framebuffer/boot-screen.hpp>

#include <hw.frigg_bragi.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

namespace thor {

THOR_DEFINE_ELF_NOTE(framebufferNote){elf_note_type::framebuffer, {}};

void publishFreestandingFb(FbInfo *associatedFrameBuffer);

// ------------------------------------------------------------------------
// window handling
// ------------------------------------------------------------------------

constexpr size_t fontHeight = 16;
constexpr size_t fontWidth = 8;

struct FbDisplay final : TextDisplay {
	FbDisplay(void *ptr, unsigned int width, unsigned int height, size_t pitch)
	: _width{width}, _height{height}, _pitch{pitch / sizeof(uint32_t)} {
		assert(!(pitch % sizeof(uint32_t)));
		setWindow(ptr);
		_clearScreen(defaultBg);
	}

	void setWindow(void *ptr) {
		_window = reinterpret_cast<uint32_t *>(ptr);
	}

	size_t getWidth() override;
	size_t getHeight() override;

	void setChars(unsigned int x, unsigned int y,
			const char *c, int count, int fg, int bg) override;
	void setBlanks(unsigned int x, unsigned int y, int count, int bg) override;

private:
	void _clearScreen(uint32_t rgb_color);

	volatile uint32_t *_window;
	unsigned int _width;
	unsigned int _height;
	size_t _pitch;
};

size_t FbDisplay::getWidth() {
	return _width / fontWidth;
}

size_t FbDisplay::getHeight() {
	return _height / fontHeight;
}

void FbDisplay::setChars(unsigned int x, unsigned int y,
		const char *c, int count, int fg, int bg) {
	renderChars((void *)_window, _pitch, x, y, c, count, fg, bg,
			std::integral_constant<int, fontWidth>{},
			std::integral_constant<int, fontHeight>{});
}

void FbDisplay::setBlanks(unsigned int x, unsigned int y, int count, int bg) {
	auto bg_rgb = (bg < 0) ? defaultBg : rgbColor[bg];

	auto dest_line = _window + y * fontHeight * _pitch + x * fontWidth;
	for(size_t i = 0; i < fontHeight; i++) {
		auto dest = dest_line;
		for(int k = 0; k < count; k++) {
			for(size_t j = 0; j < fontWidth; j++)
				*dest++ = bg_rgb;
		}
		dest_line += _pitch;
	}
}

void FbDisplay::_clearScreen(uint32_t rgb_color) {
	auto dest_line = _window;
	for(size_t i = 0; i < _height; i++) {
		auto dest = dest_line;
		for(size_t j = 0; j < _width; j++)
			*dest++ = rgb_color;
		dest_line += _pitch;
	}
}

namespace {
	frg::manual_box<FbInfo> bootInfo;
	frg::manual_box<FbDisplay> bootDisplay;
	frg::manual_box<BootScreen> bootScreen;

	struct BootFbDevice final : KernelDevice {
		BootFbDevice()
		: KernelDevice{"boot-framebuffer"} { }

	private:
		void claim() override {
			disableLogHandler(bootScreen.get());
		}
	};

	frg::manual_box<BootFbDevice> bootFbDevice;
}

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type, void *early_window) {
	bootInfo.initialize();
	auto fb_info = bootInfo.get();
	fb_info->address = address;
	fb_info->pitch = pitch;
	fb_info->width = width;
	fb_info->height = height;
	fb_info->bpp = bpp;
	fb_info->type = type;

	// Without a framebuffer, there is nothing to display the boot screen on.
	if(!address)
		return;

	// Initialize the framebuffer with a lower-half window.
	bootDisplay.initialize(early_window,
			fb_info->width, fb_info->height, fb_info->pitch);
	bootScreen.initialize(bootDisplay.get());

	enableLogHandler(bootScreen.get());
}

void transitionBootFb() {
	if(!bootInfo->address) {
		infoLogger() << "thor: No boot framebuffer" << frg::endlog;
		return;
	}

	auto windowSize = (bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1);
	auto window = KernelVirtualMemory::global().allocate(windowSize);
	for(size_t pg = 0; pg < windowSize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(VirtualAddr(window) + pg,
				bootInfo->address + pg, page_access::write, CachingMode::writeCombine);

	// Transition to the kernel mapping window.
	bootDisplay->setWindow(window);

	assert(!(bootInfo->address & (kPageSize - 1)));
	auto memoryOutcome = HardwareMemory::create(
			bootInfo->address & ~(kPageSize - 1),
			(bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1),
			CachingMode::writeCombine);
	if(!memoryOutcome)
		panicLogger() << "thor: Failed to create hardware memory" << frg::endlog;
	bootInfo->memory = std::move(*memoryOutcome);

	// The device is registered only here since its resources live on the kernel heap.
	bootFbDevice.initialize();
	bootFbDevice->addResource({KernelDeviceResourceKind::mmio,
			bootInfo->address, bootInfo->height * bootInfo->pitch});
	bootFbDevice->setFramebuffer({bootInfo->pitch, bootInfo->width,
			bootInfo->height, bootInfo->bpp, bootInfo->type});
	registerKernelDevice(bootFbDevice.get());
	bootInfo->device = bootFbDevice.get();

	publishFreestandingFb(bootInfo.get());
}

namespace {

struct FbMbusNode final : private KernelBusObject {
	FbMbusNode(FbInfo *associatedFrameBuffer)
	: associatedFrameBuffer{associatedFrameBuffer} { }

	void run(enable_detached_coroutine) {
		Properties properties;

		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "framebuffer"));
		properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "graphics"));

		auto ret = co_await createObject("freestanding-fb", std::move(properties));
		assert(ret);
	}

	coroutine<frg::expected<Error>> handleRequest(smarter::shared_ptr<Stream, LanePolicy> lane) override {
		auto [acceptError, conversation] = co_await accept(lane);
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await recvBuffer(conversation);
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if(preamble.error())
			co_return Error::protocolViolation;

		auto sendResponse = [] (smarter::shared_ptr<Stream, LanePolicy> &conversation,
				managarm::hw::SvrResponse<KernelAlloc> &&resp) -> coroutine<frg::expected<Error>> {
			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
				resp.head_size};

			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
				resp.size_of_tail()};

			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

			auto respHeadError = co_await sendBuffer(conversation, std::move(respHeadBuffer));

			if(respHeadError != Error::success)
				co_return respHeadError;

			auto respTailError = co_await sendBuffer(conversation, std::move(respTailBuffer));

			if(respTailError != Error::success)
				co_return respTailError;

			co_return frg::success;
		};


		if(preamble.id() == bragi::message_id<managarm::hw::ClaimDeviceRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::ClaimDeviceRequest>(reqBuffer, *kernelAlloc);

			if (!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			claimKernelDevice(associatedFrameBuffer->device);

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
		} else if(preamble.id() == bragi::message_id<managarm::hw::GetFbInfoRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::GetFbInfoRequest>(reqBuffer, *kernelAlloc);
			auto fb = associatedFrameBuffer;

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_fb_pitch(fb->pitch);
			resp.set_fb_width(fb->width);
			resp.set_fb_height(fb->height);
			resp.set_fb_bpp(fb->bpp);
			resp.set_fb_type(fb->type);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
		}else if(preamble.id() == bragi::message_id<managarm::hw::AccessFbMemoryRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::AccessFbMemoryRequest>(reqBuffer, *kernelAlloc);
			auto fb = associatedFrameBuffer;

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

			auto descriptor = AnyDescriptor::make<DescriptorType::memoryView>(
				fb->memory, kHelRightRead | kHelRightWrite | kHelRightAssign | kHelRightProvision | kHelRightPin
			);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

			auto descError = co_await pushDescriptor(conversation, std::move(descriptor));
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else{
			infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
			co_await dismiss(conversation);
		}

		co_return frg::success;
	}

	FbInfo *associatedFrameBuffer;
};

} // namespace anonymous

void publishFreestandingFb(FbInfo *associatedFrameBuffer) {
	infoLogger() << "thor: Publishing freestanding mbus node for framebuffer" << frg::endlog;
	auto node = frg::construct<FbMbusNode>(*kernelAlloc, associatedFrameBuffer);
	node->run(enable_detached_coroutine{WorkQueue::generalQueue().lock()});
}

} // namespace thor

