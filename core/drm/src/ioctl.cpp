#include <libdrm/drm_fourcc.h>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include "fs.bragi.hpp"
#include <helix/ipc.hpp>
#include "posix.bragi.hpp"
#include <protocols/ostrace/ostrace.hpp>
#include <print>

#include <core/clock.hpp>
#include "core/drm/core.hpp"
#include "core/drm/debug.hpp"
#include "core/drm/property.hpp"

namespace drm_core {

static constexpr auto primeFileOperations = protocols::fs::FileOperations{
	.seekAbs = &drm_core::PrimeFile::seekAbs,
	.seekRel = &drm_core::PrimeFile::seekRel,
	.seekEof = &drm_core::PrimeFile::seekEof,
	.accessMemory = &drm_core::PrimeFile::accessMemory,
	.pollWait = &drm_core::PrimeFile::pollWait,
	.pollStatus = &drm_core::PrimeFile::pollStatus,
};

}

namespace {

constinit protocols::ostrace::Event ostEvtRequest{"fs.request"};
constinit protocols::ostrace::UintAttribute ostAttrRequest{"request"};
constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
constinit protocols::ostrace::BragiAttribute ostBragi{managarm::fs::protocol_hash};

protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtRequest,
	ostAttrRequest,
	ostAttrTime,
	ostBragi,
};

protocols::ostrace::Context ostContext{ostVocabulary};

}

async::detached drm_core::File::pageFlipEvent(std::unique_ptr<drm_core::Configuration> config,
		drm_core::File *self, uint64_t cookie, uint32_t crtc_id) {
	co_await config->waitForCompletion();
	self->_retirePageFlip(cookie, crtc_id);
}

async::detached drm_core::File::pageFlipEvent(std::unique_ptr<drm_core::Configuration> config,
		drm_core::File *self, uint64_t cookie, std::unordered_set<uint32_t> crtc_ids) {
	co_await config->waitForCompletion();
	for(auto id : crtc_ids)
		self->_retirePageFlip(cookie, id);
}

struct drm_core::File::HandleIoctl {
	uint32_t id;
	timespec requestTimestamp = {};

	template <typename T>
	void logBragiRequest(T &req) {
		if(!ostContext.isActive())
			return;

		requestTimestamp = clk::getTimeSinceBoot();
		std::string reqHead;
		std::string reqTail;
		reqHead.resize(req.size_of_head());
		reqTail.resize(req.size_of_tail());
		bragi::limited_writer headWriter{reqHead.data(), reqHead.size()};
		bragi::limited_writer tailWriter{reqTail.data(), reqTail.size()};
		auto headOk = req.encode_head(headWriter);
		auto tailOk = req.encode_tail(tailWriter);
		assert(headOk);
		assert(tailOk);
		ostContext.emitWithTimestamp(
			ostEvtRequest,
			(requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec,
			ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			ostBragi({reinterpret_cast<uint8_t *>(reqHead.data()), reqHead.size()}, {reinterpret_cast<uint8_t *>(reqTail.data()), reqTail.size()})
		);
	}

	void logBragiReply(auto &resp) {
		if(!ostContext.isActive())
			return;

		auto ts = clk::getTimeSinceBoot();
		std::string replyHead;
		std::string replyTail;
		replyHead.resize(resp.size_of_head());
		replyTail.resize(resp.size_of_tail());
		bragi::limited_writer headWriter{replyHead.data(), replyHead.size()};
		bragi::limited_writer tailWriter{replyTail.data(), replyTail.size()};
		auto headOk = resp.encode_head(headWriter);
		auto tailOk = resp.encode_tail(tailWriter);
		assert(headOk);
		assert(tailOk);
		ostContext.emitWithTimestamp(
			ostEvtRequest,
			(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
			ostAttrRequest(id),
			ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			ostBragi({reinterpret_cast<uint8_t *>(replyHead.data()), replyHead.size()}, {reinterpret_cast<uint8_t *>(replyTail.data()), replyTail.size()})
		);
	}

	void logBragiSerializedReply(std::string &ser) {
		if(!ostContext.isActive())
			return;

		auto ts = clk::getTimeSinceBoot();
		ostContext.emitWithTimestamp(
			ostEvtRequest,
			(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
			ostAttrRequest(id),
			ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			ostBragi({reinterpret_cast<uint8_t *>(ser.data()), ser.size()}, {})
		);
	}

	async::result<std::expected<void, DispatchError>> operator() (
		managarm::fs::GenericIoctlRequest &&req,
		helix::BorrowedDescriptor conversation,
		bragi::preamble,
		drm_core::File *self
	) {
		logBragiRequest(req);
		if(req.command() == DRM_IOCTL_VERSION) {
			managarm::fs::GenericIoctlReply resp;

			auto version = self->_device->driverVersion();
			auto info = self->_device->driverInfo();

			resp.set_drm_version_major(std::get<0>(version));
			resp.set_drm_version_minor(std::get<1>(version));
			resp.set_drm_version_patchlevel(std::get<2>(version));

			resp.set_drm_driver_name(std::get<0>(info));
			resp.set_drm_driver_desc(std::get<1>(info));
			resp.set_drm_driver_date(std::get<2>(info));

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_GET_CAP) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GET_CAP()");

			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (req.drm_capability() == DRM_CAP_TIMESTAMP_MONOTONIC) {
				resp.set_drm_value(1);
				if (logDrmRequests)
					std::println("\tCAP_TIMESTAMP_MONOTONIC supported");
			} else if (req.drm_capability() == DRM_CAP_DUMB_BUFFER) {
				resp.set_drm_value(1);
				if (logDrmRequests)
					std::println("\tCAP_DUMB_BUFFER supported");
			} else if (req.drm_capability() == DRM_CAP_CRTC_IN_VBLANK_EVENT) {
				resp.set_drm_value(1);
				if (logDrmRequests)
					std::println("\tCAP_CRTC_IN_VBLANK_EVENT supported");
			} else if (req.drm_capability() == DRM_CAP_CURSOR_WIDTH) {
				resp.set_drm_value(self->_device->getCursorWidth());
				if (logDrmRequests)
					std::println("\tCAP_CURSOR_WIDTH supported");
			} else if (req.drm_capability() == DRM_CAP_CURSOR_HEIGHT) {
				resp.set_drm_value(self->_device->getCursorHeight());
				if (logDrmRequests)
					std::println("\tCAP_CURSOR_HEIGHT supported");
			} else if (req.drm_capability() == DRM_CAP_PRIME) {
				resp.set_drm_value(DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT);
				if (logDrmRequests)
					std::println("\tCAP_PRIME supported");
			} else if (req.drm_capability() == DRM_CAP_ADDFB2_MODIFIERS) {
				resp.set_drm_value(self->_device->getAddFb2ModifiersSupport());
				if (logDrmRequests)
					std::println(
						"\tCAP_ADDFB2_MODIFIERS {}supported", resp.drm_value() ? "" : "un"
					);
			} else {
				std::println("\tUnknown capability {}", req.drm_capability());
				resp.set_drm_value(0);
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETRESOURCES) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GETRESOURCES()");

			auto &crtcs = self->_device->getCrtcs();
			for(size_t i = 0; i < crtcs.size(); i++) {
				resp.add_drm_crtc_ids(crtcs[i]->id());
				if (logDrmRequests)
					std::println("\tCRTC {}", crtcs[i]->id());
			}

			auto &encoders = self->_device->getEncoders();
			for(size_t i = 0; i < encoders.size(); i++) {
				resp.add_drm_encoder_ids(encoders[i]->id());
				if (logDrmRequests)
					std::println("\tEncoder {}", encoders[i]->id());
			}

			auto &connectors = self->_device->getConnectors();
			for(size_t i = 0; i < connectors.size(); i++) {
				resp.add_drm_connector_ids(connectors[i]->id());
				if (logDrmRequests)
					std::println("\tConnector {}", connectors[i]->id());
			}

			auto &fbs = self->getFrameBuffers();
			for(size_t i = 0; i < fbs.size(); i++) {
				resp.add_drm_fb_ids(fbs[i]->id());
				if (logDrmRequests)
					std::println("\tFB {}", fbs[i]->id());
			}

			auto max_width = self->_device->getMaxWidth();
			auto max_height = self->_device->getMaxHeight();

			if(!max_width || !max_height) {
				std::println("\e[33mcore/drm: driver-supplied max width/height is empty, defaulting to 16384x16384\e[39m");
				max_width = 16384;
				max_height = 16384;
			}

			resp.set_drm_min_width(self->_device->getMinWidth());
			resp.set_drm_max_width(max_width);
			resp.set_drm_min_height(self->_device->getMinHeight());
			resp.set_drm_max_height(max_height);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETCONNECTOR) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GETCONNECTOR()");

			auto obj = self->_device->findObject(req.drm_connector_id());
			assert(obj);
			auto conn = obj->asConnector();
			assert(conn);

			auto psbl_enc = conn->getPossibleEncoders();
			for(size_t i = 0; i < psbl_enc.size(); i++) {
				resp.add_drm_encoders(psbl_enc[i]->id());
			}

			// TODO: check if we're current master
			if(req.drm_max_modes() == 0)
				co_await conn->probe();

			resp.set_drm_encoder_id(conn->currentEncoder() ? conn->currentEncoder()->id() : 0);
			resp.set_drm_connector_type(conn->connectorType());
			resp.set_drm_connector_type_id(0);
			resp.set_drm_connection(conn->getCurrentStatus()); // DRM_MODE_CONNECTED
			resp.set_drm_mm_width(conn->getPhysicalWidth());
			resp.set_drm_mm_height(conn->getPhysicalHeight());
			resp.set_drm_subpixel(conn->getSubpixel());
			resp.set_drm_num_modes(conn->modeList().size());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto assignments = conn->getAssignments(self->_device);

			for(auto ass : assignments) {
				resp.add_drm_obj_property_ids(ass.property->id());

				if(std::holds_alternative<IntPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);
				} else if(std::holds_alternative<EnumPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);
				} else if(std::holds_alternative<BlobPropertyType>(ass.property->propertyType())) {
					if(ass.blobValue) {
						resp.add_drm_obj_property_values(ass.blobValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				} else if(std::holds_alternative<ObjectPropertyType>(ass.property->propertyType())) {
					if(ass.objectValue) {
						resp.add_drm_obj_property_values(ass.objectValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				}

				if (logDrmRequests) {
					std::println("\tproperty {} '{}' = {}",
						std::to_underlying(ass.property->id()),
						ass.property->name(),
						resp.drm_obj_property_values(resp.drm_obj_property_values_size() - 1)
					);
				}
			}

			auto [send_resp, send_list] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(conn->modeList().data(), std::min(static_cast<size_t>(req.drm_max_modes()), conn->modeList().size()) * sizeof(drm_mode_modeinfo))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_list.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETENCODER) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GETENCODER([{}])", req.drm_encoder_id());

			auto obj = self->_device->findObject(req.drm_encoder_id());
			assert(obj);
			auto enc = obj->asEncoder();
			assert(enc);
			resp.set_drm_encoder_type(enc->getEncoderType());
			resp.set_drm_crtc_id(enc->currentCrtc() ? enc->currentCrtc()->id() : 0);

			uint32_t crtc_mask = 0;
			for(auto crtc : enc->getPossibleCrtcs()) {
				crtc_mask |= 1 << crtc->index;
			}
			resp.set_drm_possible_crtcs(crtc_mask);

			uint32_t clone_mask = 0;
			for(auto clone : enc->getPossibleClones()) {
				clone_mask |= 1 << clone->index;
			}
			resp.set_drm_possible_clones(clone_mask);

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETPLANE) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GETPLANE({})", req.drm_plane_id());

			resp.set_drm_encoder_type(0);

			auto obj = self->_device->findObject(req.drm_plane_id());
			assert(obj);
			auto plane = obj->asPlane();
			assert(plane);

			uint32_t crtc_mask = 0;
			for(auto crtc : plane->getPossibleCrtcs()) {
				crtc_mask |= 1 << crtc->index;
			}
			resp.set_drm_possible_crtcs(crtc_mask);

			auto crtc = plane->drmState()->crtc;
			if(crtc != nullptr) {
				resp.set_drm_crtc_id(crtc->id());
				if (logDrmRequests)
					std::println("\tCRTC {}", crtc->id());
			} else {
				resp.set_drm_crtc_id(0);
			}

			auto fb = plane->getFrameBuffer();
			if(fb != nullptr) {
				resp.set_drm_fb_id(fb->id());
				if (logDrmRequests)
					std::println("\tFB {}", fb->id());
			} else {
				resp.set_drm_fb_id(0);
			}

			resp.set_drm_gamma_size(0);
			resp.set_drm_format_types(plane->getFormats().size());

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp, send_formats] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(plane->getFormats().data(),
					std::min(size_t{req.drm_format_types()}, plane->getFormats().size()) * sizeof(uint32_t))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_formats.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_CREATE_DUMB) {
			managarm::fs::GenericIoctlReply resp;

			auto pair = self->_device->createDumb(req.drm_width(), req.drm_height(), req.drm_bpp());
			auto handle = co_await self->createHandle(pair.first);
			resp.set_drm_handle(handle);

			resp.set_drm_pitch(pair.second);
			resp.set_drm_size(pair.first->getSize());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (logDrmRequests)
				std::println("core/drm: CREATE_DUMB({}x{}) -> <{}>",
					req.drm_width(),
					req.drm_height(),
					resp.drm_handle()
				);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETFB2) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GETFB2({})", req.drm_fb_id());

			auto obj = self->_device->findObject(req.drm_fb_id());
			if(!obj || !obj->asFrameBuffer()) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				auto fb = obj->asFrameBuffer();
				resp.set_drm_width(fb->getWidth());
				resp.set_drm_height(fb->getHeight());
				resp.set_pixel_format(fb->format());
				resp.set_modifier(fb->getModifier());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_ADDFB) {
			managarm::fs::GenericIoctlReply resp;

			auto bo = self->resolveHandle(req.drm_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			auto fourcc = convertLegacyFormat(req.drm_bpp(), req.drm_depth());
			auto fb = self->_device->createFrameBuffer(buffer, req.drm_width(), req.drm_height(),
					fourcc, req.drm_pitch(), DRM_FORMAT_MOD_LINEAR);
			self->attachFrameBuffer(fb);
			resp.set_drm_fb_id(fb->id());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (logDrmRequests)
				std::println("core/drm: ADDFB({}x{}, pitch {}) -> [{}]",
					req.drm_width(),
					req.drm_height(),
					req.drm_pitch(),
					fb->id()
				);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_ADDFB2) {
			managarm::fs::GenericIoctlReply resp;

			auto bo = self->resolveHandle(req.drm_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			auto modifier = req.drm_flags() & DRM_MODE_FB_MODIFIERS ? req.drm_modifier() : DRM_FORMAT_MOD_LINEAR;

			auto fb = self->_device->createFrameBuffer(buffer, req.drm_width(), req.drm_height(),
					req.drm_fourcc(), req.drm_pitch(), modifier);
			self->attachFrameBuffer(fb);
			resp.set_drm_fb_id(fb->id());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (logDrmRequests)
				std::println("core/drm: ADDFB2({}x{}, pitch {}) -> [{}]",
					req.drm_width(),
					req.drm_height(),
					req.drm_pitch(),
					fb->id()
				);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_RMFB) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: RMFB([{}])", req.drm_fb_id());

			auto obj = self->_device->findObject(req.drm_fb_id());
			assert(obj);
			auto fb = obj->asFrameBuffer();
			assert(fb);
			self->detachFrameBuffer(fb);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_MAP_DUMB) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: MAP_DUMB(<{}>)", req.drm_handle());

			auto bo = self->resolveHandle(req.drm_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			resp.set_drm_offset(buffer->getMapping());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETCRTC) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: GETCRTC([{}])", req.drm_crtc_id());

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto obj = self->_device->findObject(req.drm_crtc_id());
			drm_mode_modeinfo mode_info;
			memset(&mode_info, 0, sizeof(mode_info));

			if(obj) {
				auto crtc = obj->asCrtc();
				assert(crtc);

				if(crtc->drmState()->mode) {
					memcpy(&mode_info, crtc->drmState()->mode->data(), sizeof(drm_mode_modeinfo));
					resp.set_drm_mode_valid(1);
					resp.set_drm_x(crtc->primaryPlane()->drmState()->src_x);
					resp.set_drm_y(crtc->primaryPlane()->drmState()->src_y);
					/* TODO: wire up gamma once we support that */
					resp.set_drm_gamma_size(0);
					auto fb = crtc->primaryPlane()->drmState()->fb;
					resp.set_drm_fb_id(fb ? fb->id() : 0);
				}else{
					memset(&mode_info, 0, sizeof(drm_mode_modeinfo));
					resp.set_drm_mode_valid(0);
				}
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto [send_resp, send_mode] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(&mode_info, sizeof(drm_mode_modeinfo))
			);

			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_mode.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_SETCRTC) {
			std::vector<char> mode_buffer;
			mode_buffer.resize(sizeof(drm_mode_modeinfo));

			if (logDrmRequests)
				std::println("core/drm: SETCRTC()");

			auto [recv_buffer] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::recvBuffer(mode_buffer.data(), sizeof(drm_mode_modeinfo))
			);
			HEL_CHECK(recv_buffer.error());

			managarm::fs::GenericIoctlReply resp;

			auto obj = self->_device->findObject(req.drm_crtc_id());
			assert(obj);
			auto crtc = obj->asCrtc();
			assert(crtc);

			std::vector<drm_core::Assignment> assignments;
			if(req.drm_mode_valid()) {
				auto mode_blob = self->_device->registerBlob(std::move(mode_buffer));
				auto fb = self->_device->findObject(req.drm_fb_id());
				assert(fb);

				assignments.push_back(Assignment::withInt(crtc->sharedModeObject(), self->_device->activeProperty(), true));
				assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), mode_blob));
				assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->srcWProperty(), fb->asFrameBuffer()->getWidth() << 16));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->srcHProperty(), fb->asFrameBuffer()->getHeight() << 16));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->srcXProperty(), 0));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->srcYProperty(), 0));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->crtcWProperty(), fb->asFrameBuffer()->getWidth()));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->crtcHProperty(), fb->asFrameBuffer()->getHeight()));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->crtcXProperty(), 0));
				assignments.push_back(Assignment::withInt(crtc->primaryPlane()->sharedModeObject(), self->_device->crtcYProperty(), 0));

				for(auto connectorId : req.drm_connector_ids()) {
					auto con = self->_device->findObject(connectorId);
					assert(con);

					assignments.push_back(Assignment::withModeObj(con, self->_device->crtcIdProperty(), crtc->sharedModeObject()));
				}
			}else{
				assignments.push_back(Assignment::withInt(crtc->sharedModeObject(), self->_device->activeProperty(), false));
				assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), nullptr));
			}

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();
			auto valid = config->capture(assignments, state);
			assert(valid);
			config->commit(std::move(state));

			co_await config->waitForCompletion();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_PAGE_FLIP) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: PAGE_FLIP()");

			auto obj = self->_device->findObject(req.drm_crtc_id());
			assert(obj);
			auto crtc = obj->asCrtc();
			assert(crtc);

			std::vector<drm_core::Assignment> assignments;

			auto fb = self->_device->findObject(req.drm_fb_id());
			assert(fb);
			assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
			assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->crtcIdProperty(), crtc->sharedModeObject()));

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();
			auto valid = config->capture(assignments, state);
			assert(valid);
			config->commit(std::move(state));

			if(req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT)
				self->pageFlipEvent(std::move(config), self, req.drm_cookie(), crtc->id());

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_DIRTYFB) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: DIRTYFB()");

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto obj = self->_device->findObject(req.drm_fb_id());
			if(!obj) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				auto fb = obj->asFrameBuffer();
				assert(fb);
				fb->notifyDirty();
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_CURSOR) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: MODE_CURSOR()");

			auto crtc_obj = self->_device->findObject(req.drm_crtc_id());
			assert(crtc_obj);
			auto crtc = crtc_obj->asCrtc();

			auto cursor_plane = crtc->cursorPlane();

			if (cursor_plane == nullptr) {
				resp.set_error(managarm::fs::Errors::NO_BACKING_DEVICE);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				co_return {};
			}

			std::vector<Assignment> assignments;
			if (req.drm_flags() == DRM_MODE_CURSOR_BO) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				auto bo = self->resolveHandle(req.drm_handle());
				auto width = req.drm_width();
				auto height = req.drm_height();

				assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcWProperty(), width << 16));
				assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcHProperty(), height << 16));

				if (bo) {
					auto fb = self->_device->createFrameBuffer(bo->sharedBufferObject(), width, height, DRM_FORMAT_ARGB8888, width * 4, DRM_FORMAT_MOD_LINEAR);
					assert(fb);
					assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
				} else {
					assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), nullptr));
				}
			}else if (req.drm_flags() == DRM_MODE_CURSOR_MOVE) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				auto x = req.drm_x();
				auto y = req.drm_y();

				assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcXProperty(), x));
				assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcYProperty(), y));
			}else{
				printf("\e[35mcore/drm: invalid request whilst handling DRM_IOCTL_MODE_CURSOR\e[39m\n");
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();
			auto valid = config->capture(assignments, state);
			assert(valid);
			config->commit(std::move(state));

			co_await config->waitForCompletion();

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_CURSOR2) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: MODE_CURSOR2()");

			auto crtc_obj = self->_device->findObject(req.drm_crtc_id());
			assert(crtc_obj);
			auto crtc = crtc_obj->asCrtc();

			auto cursor_plane = crtc->cursorPlane();

			if(cursor_plane == nullptr) {
				resp.set_error(managarm::fs::Errors::NO_BACKING_DEVICE);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				co_return {};
			}

			std::vector<Assignment> assignments;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			auto bo = self->resolveHandle(req.drm_handle());
			auto width = req.drm_width();
			auto height = req.drm_height();

			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcWProperty(), width << 16));
			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcHProperty(), height << 16));

			if(bo) {
				auto fb = self->_device->createFrameBuffer(bo->sharedBufferObject(), width, height, DRM_FORMAT_ARGB8888, width * 4, DRM_FORMAT_MOD_LINEAR);
				assert(fb);
				assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
			} else {
				assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), nullptr));
			}

			auto x = req.drm_x();
			auto y = req.drm_y();

			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcXProperty(), x));
			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcYProperty(), y));

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();
			auto valid = config->capture(assignments, state);
			assert(valid);
			config->commit(std::move(state));

			co_await config->waitForCompletion();

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_DESTROY_DUMB){
			if (logDrmRequests)
				std::println("core/drm: DESTROY_DUMB({})", req.drm_handle());

			self->_buffers.erase(req.drm_handle());
			self->_allocator.free(req.drm_handle());

			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_SET_CLIENT_CAP) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: SET_CLIENT_CAP()");

			if(req.drm_capability() == DRM_CLIENT_CAP_STEREO_3D) {
				std::println("\e[31mcore/drm: DRM client cap for stereo 3D unsupported\e[39m");
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else if(req.drm_capability() == DRM_CLIENT_CAP_UNIVERSAL_PLANES) {
				self->universalPlanes = true;
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else if(req.drm_capability() == DRM_CLIENT_CAP_ATOMIC) {
				self->atomic = true;
				self->universalPlanes = true;
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else if(req.drm_capability() == DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT) {
				std::println("\e[31mcore/drm: DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT is stubbed\e[39m");
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				std::println("\e[31mcore/drm: Attempt to set unknown client capability {}\e[39m", req.drm_capability());
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
			managarm::fs::GenericIoctlReply resp;

			auto obj = self->_device->findObject(req.drm_obj_id());
			assert(obj);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (logDrmRequests)
				std::println("core/drm: GETPROPERTIES([{}])", req.drm_obj_id());

			for(auto ass : obj->getAssignments(self->_device)) {
				resp.add_drm_obj_property_ids(ass.property->id());

				if(std::holds_alternative<IntPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);

					if (logDrmRequests)
						std::println("\t{} -> int {}", ass.property->name(), ass.intValue);
				} else if(std::holds_alternative<EnumPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);

					if (logDrmRequests) {
						auto enuminfo = ass.property->enumInfo();
						std::string enum_name = "<invalid>";
						if(enuminfo.contains(ass.intValue)) {
							enum_name = enuminfo.at(ass.intValue);
						}
						std::println("\t{} -> enum {} ({})", ass.property->name(), enum_name, ass.intValue);
					}
				} else if(std::holds_alternative<BlobPropertyType>(ass.property->propertyType())) {
					if(ass.blobValue) {
						resp.add_drm_obj_property_values(ass.blobValue->id());
						if (logDrmRequests)
							std::println("\t{} -> blob [{}]", ass.property->name(), ass.blobValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				} else if(std::holds_alternative<ObjectPropertyType>(ass.property->propertyType())) {
					if(ass.objectValue) {
						resp.add_drm_obj_property_values(ass.objectValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				}
			}

			if(!resp.drm_obj_property_ids_size()) {
				std::println("\e[31mcore/drm: No properties found for object [{}]\e[39m", req.drm_obj_id());
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETPROPERTY) {
			managarm::fs::GenericIoctlReply resp;

			uint32_t prop_id = req.drm_property_id();
			auto prop = self->_device->getProperty(prop_id);

			if (logDrmRequests) {
				std::string prop_name = (prop) ? prop->name() : "<invalid>";
				std::println("core/drm: GETPROPERTY({} [{}])", prop_name, prop_id);
			}

			if(prop) {
				if(std::holds_alternative<IntPropertyType>(prop->propertyType())) {
					uint32_t type = prop->flags() & (DRM_MODE_PROP_LEGACY_TYPE | DRM_MODE_PROP_EXTENDED_TYPE);
					switch(type) {
						case DRM_MODE_PROP_RANGE: {
							resp.add_drm_property_vals(prop->rangeMin());
							resp.add_drm_property_vals(prop->rangeMax());
							break;
						}
						case DRM_MODE_PROP_SIGNED_RANGE: {
							resp.add_drm_property_vals(prop->signedRangeMin());
							resp.add_drm_property_vals(prop->signedRangeMax());
							break;
						}
						default: {
							std::println("core/drm: int property type {} is unhandled by DRM_IOCTL_MODE_GETPROPERTY", type);
							break;
						}
					}
				} else if(std::holds_alternative<ObjectPropertyType>(prop->propertyType())) {
					resp.add_drm_property_vals(prop->typeFlags());
				} else if(std::holds_alternative<EnumPropertyType>(prop->propertyType())) {
					for(auto &[value, name] : prop->enumInfo()) {
						resp.add_drm_enum_value(value);
						resp.add_drm_enum_name(name);
					}
				}

				resp.set_drm_property_name(prop->name());
				resp.set_drm_property_flags(prop->flags());

				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_SETPROPERTY) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: SETPROPERTY()");

			std::vector<drm_core::Assignment> assignments;

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();

			auto mode_obj = self->_device->findObject(req.drm_obj_id());
			assert(mode_obj);

			auto prop = self->_device->getProperty(req.drm_property_id());
			assert(prop);
			auto value = req.drm_property_value();
			auto prop_type = prop->propertyType();


			if(std::holds_alternative<IntPropertyType>(prop_type)) {
				assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
			} else if(std::holds_alternative<EnumPropertyType>(prop_type)) {
				assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
			} else if(std::holds_alternative<BlobPropertyType>(prop_type)) {
				auto blob = self->_device->findBlob(value);
				assignments.push_back(Assignment::withBlob(mode_obj, prop.get(), blob));
			} else if(std::holds_alternative<ObjectPropertyType>(prop_type)) {
				auto obj = self->_device->findObject(value);
				assignments.push_back(Assignment::withModeObj(mode_obj, prop.get(), obj));
			}

			auto valid = config->capture(assignments, state);
			assert(valid);

			config->commit(std::move(state));
			co_await config->waitForCompletion();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETPLANERESOURCES) {
			managarm::fs::GenericIoctlReply resp;

			auto &crtcs = self->_device->getCrtcs();
			for(size_t i = 0; i < crtcs.size(); i++) {
				resp.add_drm_plane_res(crtcs[i]->primaryPlane()->id());

				if(crtcs[i]->cursorPlane()) {
					resp.add_drm_plane_res(crtcs[i]->cursorPlane()->id());
				}
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);

			if (logDrmRequests)
				std::println("core/drm: GETPLANERESOURCES()");

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_GETPROPBLOB) {
			managarm::fs::GenericIoctlReply resp;

			auto blob = self->_device->findBlob(req.drm_blob_id());

			if (logDrmRequests)
				std::println("core/drm: GETPROPBLOB([{}]{})",
					req.drm_blob_id(),
					(!blob) ? " (invalid)" : ""
				);

			if(blob) {
				resp.set_drm_property_blob_size(blob->size());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(blob ? blob->data() : nullptr,
					std::min(blob ? blob->size() : 0, size_t{req.drm_blob_size()}))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_CREATEPROPBLOB) {
			std::vector<char> blob_data;
			blob_data.resize(req.drm_blob_size());

			auto [recv_buffer] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::recvBuffer(blob_data.data(), sizeof(drm_mode_modeinfo))
			);
			HEL_CHECK(recv_buffer.error());

			managarm::fs::GenericIoctlReply resp;

			if(!req.drm_blob_size()) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				auto blob = self->_device->registerBlob(std::move(blob_data));

				resp.set_drm_blob_id(blob->id());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			if (logDrmRequests)
				std::println("core/drm: CREATEPROPBLOB() -> [{}]", resp.drm_blob_id());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_DESTROYPROPBLOB) {
			managarm::fs::GenericIoctlReply resp;

			if(!self->_device->deleteBlob(req.drm_blob_id())) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			if (logDrmRequests)
				std::println("core/drm: DESTROYPROPBLOB([{}])", req.drm_blob_id());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_MODE_ATOMIC) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: ATOMIC()");

			size_t prop_count = 0;
			std::vector<drm_core::Assignment> assignments;

			std::unordered_set<uint32_t> crtc_ids;

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();

			if(!self->atomic || req.drm_flags() & ~DRM_MODE_ATOMIC_FLAGS || ((req.drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY) && (req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT))) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				goto send;
			}

			for(size_t i = 0; i < req.drm_obj_ids_size(); i++) {
				auto mode_obj = self->_device->findObject(req.drm_obj_ids(i));
				assert(mode_obj);

				if (logDrmRequests) {
					switch(mode_obj->type()) {
						case ObjectType::crtc:
							std::println("\tCRTC (ID {})", mode_obj->id());
							break;
						case ObjectType::connector:
							std::println("\tConnector (ID {})", mode_obj->id());
							break;
						case ObjectType::encoder:
							std::println("\tEncoder (ID {})", mode_obj->id());
							break;
						case ObjectType::frameBuffer:
							std::println("\tFB (ID {})", mode_obj->id());
							break;
						case ObjectType::plane:
							std::println("\tPlane (ID {})", mode_obj->id());
							break;
					}
				}

				if(mode_obj->type() == ObjectType::crtc) {
					crtc_ids.insert(mode_obj->id());
				}else if(mode_obj->type() == ObjectType::plane) {
					auto crtc = mode_obj->asPlane()->drmState()->crtc;
					if(crtc)
						crtc_ids.insert(crtc->id());
				}else if(mode_obj->type() == ObjectType::connector) {
					auto crtc = mode_obj->asConnector()->drmState()->crtc;
					if(crtc)
						crtc_ids.insert(crtc->id());
				}

				for(size_t j = 0; j < req.drm_prop_counts(i); j++) {
					auto prop = self->_device->getProperty(req.drm_props(prop_count + j));
					assert(prop);
					auto value = req.drm_prop_values(prop_count + j);

					auto prop_type = prop->propertyType();

					if(std::holds_alternative<IntPropertyType>(prop_type)) {
						assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
						if (logDrmRequests)
							std::println("\t\t{} = {} (int)", prop->name(), value);
					} else if(std::holds_alternative<EnumPropertyType>(prop_type)) {
						assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
						if (logDrmRequests)
							std::println("\t\t{} = {} {} (enum)", prop->name(), value, prop->enumInfo().at(value));
					} else if(std::holds_alternative<BlobPropertyType>(prop_type)) {
						auto blob = self->_device->findBlob(value);

						assignments.push_back(Assignment::withBlob(mode_obj, prop.get(), blob));
						if (logDrmRequests)
							std::println("\t\t{} = {} (blob)", prop->name(), blob ? std::to_string(blob->id()) : "<none>");
					} else if(std::holds_alternative<ObjectPropertyType>(prop_type)) {
						auto obj = self->_device->findObject(value);

						assignments.push_back(Assignment::withModeObj(mode_obj, prop.get(), obj));
						if (logDrmRequests)
							std::println("\t\t{} = {} (modeobject)", prop->name(), obj ? std::to_string(obj->id()) : "<none>");
					}
				}

				prop_count += req.drm_prop_counts(i);
			}

			{
				auto valid = config->capture(assignments, state);
				assert(valid);
			}

			if(!(req.drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY)) {
				if (logDrmRequests)
					std::println("\tCommitting configuration ...");
				config->commit(std::move(state));
				if(!(req.drm_flags() & DRM_MODE_ATOMIC_NONBLOCK))
					co_await config->waitForCompletion();
			}

			if(req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT) {
				self->pageFlipEvent(std::move(config), self, req.drm_cookie(), std::move(crtc_ids));
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);

		send:
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.command() == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: PRIME_HANDLE_TO_FD(<{}>)", req.drm_prime_handle());

			// Extract the credentials of the calling thread in order to locate it in POSIX for attaching the file
			auto [proc_creds] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(proc_creds.error());

			auto bo = self->resolveHandle(req.drm_prime_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			// Create the lane used for serving the PRIME fd
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream(true);
			auto file = smarter::make_shared<drm_core::PrimeFile>(bo->getMemory().first, bo->getSize());

			// Start serving the file
			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), file, &drm_core::primeFileOperations));

			// Request POSIX to register our file as a passthrough file, while giving out a fd we can pass back to our client
			managarm::posix::ServeFdRequest fd_req;
			auto proc_cred_str = proc_creds.credentials();
			fd_req.set_passthrough_credentials(proc_cred_str);

			auto fd_ser = fd_req.SerializeAsString();
			auto [offer, send_req, send_handle, recv_resp] = co_await helix_ng::exchangeMsgs(
				self->_device->_posixLane,
				helix_ng::offer(
					helix_ng::sendBuffer(fd_ser.data(), fd_ser.size()),
					helix_ng::pushDescriptor(helix::BorrowedDescriptor(remote_lane), kHelRightInvoke | kHelRightManage),
					helix_ng::recvInline())
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(send_handle.error());
			HEL_CHECK(recv_resp.error());

			managarm::posix::ServeFdResponse posix_resp;
			posix_resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			recv_resp.reset();

			char creds_data[16];
			// 'export' the object so that we can locate it from other threads, too
			HEL_CHECK(helGetCredentials(remote_lane.getHandle(), 0, creds_data));
			helix_ng::Credentials creds{{creds_data}};

			if(self->exportBufferObject(req.drm_prime_handle(), creds)) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_drm_prime_fd(posix_resp.fd());
				if (logDrmRequests)
					std::println("\t-> {{}}", posix_resp.fd());
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.command() == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
			managarm::fs::GenericIoctlReply resp;

			if (logDrmRequests)
				std::println("core/drm: PRIME_FD_TO_HANDLE({{can't resolve credentials yet}})");

			// extract the credentials of the lane that served the PRIME fd, as this is keying our maps that keep track of it
			auto [creds] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(creds.error());

			// 'import' the BufferObject while returning or creating the DRM handle that references it
			helix_ng::Credentials credentials = creds.credentials();
			auto [bo, handle] = co_await self->importBufferObject(credentials);

			if(bo) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_drm_prime_handle(handle);
				if (logDrmRequests)
					std::println("\t-> <{}>", handle);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else{
			std::println("\e[31mcore/drm: Unknown ioctl() with ID {}\e[39m", req.command());

			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
			co_return {};
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>> operator() (
		managarm::fs::DrmIoctlGemCloseRequest &&req,
		helix::BorrowedDescriptor conversation,
		bragi::preamble,
		drm_core::File *self
	) {
		logBragiRequest(req);
		managarm::fs::DrmIoctlGemCloseReply resp;

		if (logDrmRequests)
			std::println("core/drm: DRM_IOCTL_GEM_CLOSE({})", req.handle());

		self->_buffers.erase(req.handle());

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
			co_return {};
	}
};

async::result<void>
drm_core::File::ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	if(!ostContext.isInitialized())
		co_await ostContext.create();

	auto self = static_cast<drm_core::File *>(object);

	HandleIoctl visitor{id};

	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest,
		managarm::fs::DrmIoctlGemCloseRequest
	>(conversation, std::move(msg), visitor, self);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}
