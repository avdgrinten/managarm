
#include <stdio.h>
#include <protocols/posix/procdata.hpp>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <print>
#include <vector>

#include <helix/ipc.hpp>

#include <core/cancel-events.hpp>
#include <core/clock.hpp>
#include <core/dispatch.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <bragi/helpers-std.hpp>
#include "fs.bragi.hpp"
#include "protocols/fs/common.hpp"

namespace protocols::fs {

namespace utils {

// overrides `ucred` if `so_passcred` is true and `ucred` does not already hold data
bool handleSoPasscred(bool so_passcred, struct ucred &ucred, pid_t process_pid, uid_t process_uid, gid_t process_gid) {
	if(so_passcred && ucred.pid == 0 && ucred.uid == 0 && ucred.gid == 0) {
		ucred.pid = process_pid;
		ucred.uid = process_uid;
		ucred.gid = process_gid;

		return true;
	}

	return false;
}

} // namespace utils

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

bool ostraceInit = false;
protocols::ostrace::Context ostContext{ostVocabulary};

async::result<void> initOstrace() {
	co_await ostContext.create();
}

CancelEventRegistry cancellationEvents;

struct HandleFileRequest {
	uint64_t id = 0;
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

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		if(file_ops->logRequests)
			std::cout << "handlePassThrough(): serving request of type " <<
				(int)req.req_type() << std::endl;

		if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
			if(!file_ops->seekAbs) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}
			auto result = co_await file_ops->seekAbs(file.get(), req.rel_offset());
			auto error = std::get_if<Error>(&result);

			managarm::fs::SvrResponse resp;
			if(error) {
				resp.set_error(*error | toFsError);
			} else {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_offset(std::get<int64_t>(result));
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::SEEK_REL) {
			if(!file_ops->seekRel) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}
			auto result = co_await file_ops->seekRel(file.get(), req.rel_offset());
			auto error = std::get_if<Error>(&result);

			managarm::fs::SvrResponse resp;
			if(error) {
				resp.set_error(*error | toFsError);
			} else {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_offset(std::get<int64_t>(result));
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
			if(!file_ops->seekEof) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}
			auto result = co_await file_ops->seekEof(file.get(), req.rel_offset());
			auto error = std::get_if<Error>(&result);

			managarm::fs::SvrResponse resp;
			if(error) {
				resp.set_error(*error | toFsError);
			} else {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_offset(std::get<int64_t>(result));
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::FLOCK) {
			if(!file_ops->flock) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}
			auto result = co_await file_ops->flock(file.get(), req.flock_flags());

			managarm::fs::SvrResponse resp;
			if(result == protocols::fs::Error::illegalArguments) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else if(result == protocols::fs::Error::wouldBlock) {
				resp.set_error(managarm::fs::Errors::WOULD_BLOCK);
			} else {
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
			if(!file_ops->accessMemory) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				co_return {};
			}

			auto memory = co_await file_ops->accessMemory(file.get());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_memory] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(memory)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_memory.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::FILE_POLL_STATUS) {
			if(!file_ops->pollStatus) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto resultOrError = co_await file_ops->pollStatus(file.get());
			if(!resultOrError) {
				managarm::fs::SvrResponse resp;
				resp.set_error(resultOrError.error() | toFsError);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto result = resultOrError.value();

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_sequence(std::get<0>(result));
			resp.set_status(std::get<1>(result));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_BIND) {
			auto [extract_creds, recv_addr] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::extractCredentials(),
				helix_ng::recvInline()
			);
			HEL_CHECK(extract_creds.error());
			HEL_CHECK(recv_addr.error());

			if(!file_ops->bind) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto error = co_await file_ops->bind(file.get(),
				extract_creds.credentials(),
				recv_addr.data(), recv_addr.length());
			recv_addr.reset();

			managarm::fs::SvrResponse resp;
			resp.set_error(error | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_CONNECT) {
			auto [extract_creds, recv_addr] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::extractCredentials(),
				helix_ng::recvInline()
			);
			HEL_CHECK(extract_creds.error());
			HEL_CHECK(recv_addr.error());

			if(!file_ops->connect) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto error = co_await file_ops->connect(file.get(),
				extract_creds.credentials(),
				recv_addr.data(), recv_addr.length());
			recv_addr.reset();

			managarm::fs::SvrResponse resp;
			resp.set_error(error | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_SOCKNAME) {
			if(!file_ops->sockname) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::NOT_A_SOCKET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			std::vector<char> addr;
			addr.resize(req.size());
			auto actual_length = co_await file_ops->sockname(file.get(),
					addr.data(), req.size());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_file_size(actual_length);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(addr.data(),
						std::min(size_t(req.size()), actual_length))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_PEERNAME) {
			if(!file_ops->peername) {
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
				co_return {};
			}

			std::vector<char> addr;
			addr.resize(req.size());
			auto result = co_await file_ops->peername(file.get(), addr.data(), req.size());

			if (!result) {
				managarm::fs::SvrResponse resp;
				resp.set_error(result.error() | toFsError);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			assert(result); // This can never fail (yet)
			auto actual_length = result.value();

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_file_size(actual_length);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(addr.data(),
						std::min(size_t(req.size()), actual_length))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_GET_FILE_FLAGS) {
			if(!file_ops->getFileFlags) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto flags = co_await file_ops->getFileFlags(file.get());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_flags(flags);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_SET_FILE_FLAGS) {
			if(!file_ops->setFileFlags) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			co_await file_ops->setFileFlags(file.get(), req.flags());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else if(req.req_type() == managarm::fs::CntReqType::PT_LISTEN) {
			if(!file_ops->listen) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto res = co_await file_ops->listen(file.get());

			managarm::fs::SvrResponse resp;
			resp.set_error(res | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		} else if (req.req_type() == managarm::fs::CntReqType::PT_ADD_SEALS) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto seals = co_await file_ops->addSeals(file.get(), req.seals());

			if(!seals) {
				switch(seals.error()) {
					case protocols::fs::Error::insufficientPermissions: {
						resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
						break;
					}
					default: {
						resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
						break;
					}
				}
				resp.set_seals(0);
			} else {
				resp.set_seals(seals.value());
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		} else if (req.req_type() == managarm::fs::CntReqType::PT_GET_SEALS) {
			managarm::fs::SvrResponse resp;

			if(!file_ops->getSeals) {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			auto result = co_await file_ops->getSeals(file.get());
			if(!result) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);
				resp.set_seals(0);
			} else {
				assert(result);
				auto seals = result.value();

				resp.set_seals(seals);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		} else {
			std::cout << "protocols/fs: Unexpected request "
				<< static_cast<unsigned int>(req.req_type())
				<< " in servePassthrough()" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::dismiss()
			);
			HEL_CHECK(dismiss.error());
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::RecvMsgRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);


		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->recvMsg) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		std::vector<char> buffer;
		buffer.resize(req.size());
		std::vector<char> addr;
		addr.resize(req.addr_size());

		auto result = co_await file_ops->recvMsg(file.get(),
			extract_creds.credentials(), req.flags(),
			buffer.data(), buffer.size(),
			addr.data(), addr.size(),
			req.ctrl_size());

		managarm::fs::RecvMsgReply resp;

		auto error = std::get_if<Error>(&result);
		if(error) {
			resp.set_error(*error | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);
		auto data = std::get<RecvData>(result);
		assert(data.ctrl.size() <= req.ctrl_size());
		resp.set_addr_size(data.addressLength);
		resp.set_ret_val(data.dataLength);
		resp.set_flags(data.flags);
		auto ser = resp.SerializeAsString();
		auto [send_resp, send_addr, send_data, send_ctrl] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(addr.data(), std::min(addr.size(), data.addressLength)),
			helix_ng::sendBuffer(buffer.data(), buffer.size()),
			helix_ng::sendBuffer(data.ctrl.data(), data.ctrl.size())
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_addr.error());
		HEL_CHECK(send_data.error());
		HEL_CHECK(send_ctrl.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::SendMsgRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		auto [recv_data, extract_creds, recv_addr] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(buffer.data(), buffer.size()),
			helix_ng::extractCredentials(),
			helix_ng::recvInline()
		);
		HEL_CHECK(recv_data.error());
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_addr.error());

		if(!file_ops->sendMsg) {
			managarm::fs::SendMsgReply resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		std::vector<uint32_t> files;
		struct ucred ucreds = {};

		if(req.has_cmsg_rights())
			files.assign(req.fds().cbegin(), req.fds().cend());

		if(req.has_cmsg_creds()) {
			ucreds.pid = req.creds_pid();
			ucreds.uid = req.creds_uid();
			ucreds.gid = req.creds_gid();
		}

		auto res = co_await file_ops->sendMsg(file.get(),
			extract_creds.credentials(), req.flags(),
			buffer.data(), recv_data.actualLength(),
			recv_addr.data(), recv_addr.length(),
			std::move(files), ucreds);
		recv_addr.reset();

		managarm::fs::SendMsgReply resp;

		if(!res) {
			resp.set_error(res.error() | toFsError);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_size(res.value());
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::IoctlRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);


		auto [recv_msg] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvInline()
		);

		HEL_CHECK(recv_msg.error());

		auto msg_preamble = bragi::read_preamble(recv_msg);

		if(!file_ops->ioctl) {
			std::cout << "protocols/fs: ioctl not supported" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
			co_return {};
		}

		co_await file_ops->ioctl(file.get(), msg_preamble.id(), std::move(recv_msg), helix::UniqueLane{conversation.dup()});
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::SetSockOpt &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);


		std::vector<char> optbuf;

		if(req.optlen()) {
			optbuf.resize(req.optlen());

			auto [recv_buf] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(optbuf.data(), optbuf.size())
			);
			HEL_CHECK(recv_buf.error());
		}

		managarm::fs::SvrResponse resp;

		if(!file_ops->setSocketOption) {
			std::cout << "protocols/fs: setsockopt not supported on socket" << std::endl;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		auto ret = co_await file_ops->setSocketOption(file.get(), req.layer(), req.number(), optbuf);
		if(!ret) {
			assert(ret.error() != protocols::fs::Error::none);
			resp.set_error(ret.error() | toFsError);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::GetSockOpt &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);


		auto [recv_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(recv_creds.error());

		std::vector<char> optbuf;
		optbuf.resize(req.optlen());

		managarm::fs::SvrResponse resp;

		if(!file_ops->getSocketOption) {
			std::cout << "protocols/fs: getsockopt not supported on socket" << std::endl;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		auto ret = co_await file_ops->getSocketOption(file.get(), recv_creds.credentials(),
			req.layer(), req.number(), optbuf);
		if(!ret) {
			assert(ret.error() != protocols::fs::Error::none);
			resp.set_error(ret.error() | toFsError);
			resp.set_size(0);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_size(optbuf.size());
		}

		auto [send_resp, send_buf] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(optbuf.data(), resp.size())
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_buf.error());
		logBragiReply(resp);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::ShutdownSocket &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		auto how = req.how();

		managarm::fs::SvrResponse resp;

		if(!file_ops->shutdown) {
			std::cout << "protocols/fs: shutdown not supported on this file" << std::endl;
			resp.set_error(managarm::fs::Errors::NOT_A_SOCKET);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		auto ret = co_await file_ops->shutdown(file.get(), how);
		resp.set_error(ret | toFsError);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::ReadEntriesRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		if(!file_ops->readEntries) {
			managarm::fs::ReadEntriesResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}
		auto result = co_await file_ops->readEntries(file.get());

		managarm::fs::ReadEntriesResponse resp;
		if(result) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_path(result->name);
			resp.set_ino(result->inode);
			resp.set_offset(result->offset);
		}else{
			resp.set_error(result.error());
		}

		auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{}));
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_tail.error());
		logBragiReply(resp);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CancelOperation &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void>,
			const FileOperations *) {
		id = preamble.id();
		logBragiRequest(req);

		auto cancellationId = req.cancellation_id();

		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		cancellationEvents.cancel(extract_creds.credentials(), cancellationId);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::FilePollRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->pollWait) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		frg::expected<Error, PollWaitResult> resultOrError = Error::internalError;

		{
			auto cancelEvent = cancellationEvents.event(extract_creds.credentials(), req.cancellation_id());
			if (!cancelEvent) {
				std::println("protocols/fs: possibly duplicate cancellation ID registered");
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			resultOrError = co_await file_ops->pollWait(file.get(),
				req.sequence(), req.event_mask(), cancelEvent);
		}

		if(!resultOrError) {
			managarm::fs::SvrResponse resp;
			resp.set_error(resultOrError.error() | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		auto result = resultOrError.value();

		managarm::fs::FilePollResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_sequence(std::get<0>(result));
		resp.set_edges(std::get<1>(result));

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::AcceptRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		if(!file_ops->accept) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(sendResp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		auto result = co_await file_ops->accept(file.get());

		managarm::fs::SvrResponse resp;
		if(result) {
			auto [ctrlLane, ptLane] = std::move(result.value());

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [sendResp, pushCtrl, pushPt] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(ctrlLane),
				helix_ng::pushDescriptor(ptLane)
			);
			HEL_CHECK(sendResp.error());
			HEL_CHECK(pushCtrl.error());
			HEL_CHECK(pushPt.error());
			logBragiSerializedReply(ser);
		} else {
			resp.set_error(result.error() | toFsError);

			auto ser = resp.SerializeAsString();
			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(sendResp.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::ReadRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->read) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		std::string data;
		data.resize(req.size());
		ReadResult res = std::unexpected{Error::internalError};

		{
			auto cancelEvent = cancellationEvents.event(extract_creds.credentials(), req.cancellation_id());
			if (!cancelEvent) {
				std::println("protocols/fs: possibly duplicate cancellation ID registered");
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiSerializedReply(ser);
				co_return {};
			}

			res = co_await file_ops->read(
				file.get(),
				extract_creds.credentials(),
				data.data(),
				req.size(),
				cancelEvent
			);
		}

		managarm::fs::SvrResponse resp;
		size_t size = 0;
		if(!res.has_value()) {
			resp.set_error(res.error() | toFsError);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			size = res.value();
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(data.data(), size)
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::PreadRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		auto [extract_creds] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials()
		);
		HEL_CHECK(extract_creds.error());

		if(!file_ops->pread) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		std::string data;
		data.resize(req.size());
		auto res = co_await file_ops->pread(file.get(), req.offset(), extract_creds.credentials(),
				data.data(), req.size());

		managarm::fs::SvrResponse resp;
		if (!res.has_value()) {
			resp.set_error(res.error() | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(data.data(), res.value())
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::WriteRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		auto [extract_creds, recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials(),
			helix_ng::recvBuffer(buffer.data(), buffer.size())
		);
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_buffer.error());

		if(!file_ops->write) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		auto res = co_await file_ops->write(file.get(), extract_creds.credentials(),
				buffer.data(), recv_buffer.actualLength());

		managarm::fs::SvrResponse resp;
		if(!res) {
			resp.set_error(res.error() | toFsError);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else{
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_size(res.value());

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::PwriteRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		std::vector<uint8_t> buffer;
		buffer.resize(req.size());

		auto [extract_creds, recv_buffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::extractCredentials(),
			helix_ng::recvBuffer(buffer.data(), buffer.size())
		);
		HEL_CHECK(extract_creds.error());
		HEL_CHECK(recv_buffer.error());

		if(!file_ops->pwrite) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		auto res = co_await file_ops->pwrite(file.get(), req.offset(), extract_creds.credentials(),
				buffer.data(), recv_buffer.actualLength());

		managarm::fs::SvrResponse resp;
		if(!res) {
			resp.set_error(res.error() | toFsError);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}else{
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_size(res.value());

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::TruncateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		if(!file_ops->truncate) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}
		auto result = co_await file_ops->truncate(file.get(), req.size());

		managarm::fs::SvrResponse resp;
		if(result) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}else{
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::FallocateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, smarter::shared_ptr<void> file,
			const FileOperations *file_ops) {
		id = preamble.id();
		logBragiRequest(req);

		if(!file_ops->fallocate) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}
		auto result = co_await file_ops->fallocate(file.get(), req.rel_offset(), req.size());

		managarm::fs::SvrResponse resp;

		if(result) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else if(result.error() == protocols::fs::Error::insufficientPermissions) {
			resp.set_error(managarm::fs::Errors::INSUFFICIENT_PERMISSIONS);
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
		co_return {};
	}

};

struct HandleNodeRequest {
	uint64_t id = 0;
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

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();
		logBragiRequest(req);

		if(req.req_type() == managarm::fs::CntReqType::NODE_GET_STATS) {
			assert(node_ops->getStats);
			auto result = co_await node_ops->getStats(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_file_size(result.fileSize);
			resp.set_num_links(result.linkCount);
			resp.set_mode(result.mode);
			resp.set_uid(result.uid);
			resp.set_gid(result.gid);
			resp.set_atime_secs(result.accessTime.tv_sec);
			resp.set_atime_nanos(result.accessTime.tv_nsec);
			resp.set_mtime_secs(result.dataModifyTime.tv_sec);
			resp.set_mtime_nanos(result.dataModifyTime.tv_nsec);
			resp.set_ctime_secs(result.anyChangeTime.tv_sec);
			resp.set_ctime_nanos(result.anyChangeTime.tv_nsec);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		} else if(req.req_type() == managarm::fs::CntReqType::NODE_SYMLINK) {
			std::string name;
			std::string target;
			name.resize(req.name_length());
			target.resize(req.target_length());

			auto [recvName, recvTarget] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(name.data(), name.size()),
				helix_ng::recvBuffer(target.data(), target.size())
			);
			HEL_CHECK(recvName.error());
			HEL_CHECK(recvTarget.error());

			auto result = co_await node_ops->symlink(node, std::move(name), std::move(target));

			if (result) {
				helix::UniqueLane local_lane, remote_lane;
				std::tie(local_lane, remote_lane) = helix::createStream();
				serveNode(std::move(local_lane), std::move(std::get<0>(result.value())), node_ops);

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_id(std::get<1>(result.value()));

				auto ser = resp.SerializeAsString();
				auto [sendResp, pushNode] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remote_lane)
				);
				HEL_CHECK(sendResp.error());
				HEL_CHECK(pushNode.error());
				logBragiSerializedReply(ser);
			}else{
				managarm::fs::SvrResponse resp;
				resp.set_error(result.error() | toFsError);

				auto ser = resp.SerializeAsString();
				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(sendResp.error());
				logBragiSerializedReply(ser);
			}
		} else if(req.req_type() == managarm::fs::CntReqType::NODE_READ_SYMLINK) {
			auto link = co_await node_ops->readSymlink(node);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, send_link] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(link.data(), link.size())
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_link.error());
			logBragiSerializedReply(ser);
		} else if(req.req_type() == managarm::fs::CntReqType::NODE_CHMOD) {
			co_await node_ops->chmod(node, req.mode());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		} else {
			throw std::runtime_error("libfs_protocol: Unexpected request type in serveNode");
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::UtimensatRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();
		logBragiRequest(req);

		std::optional<timespec> atime = std::nullopt;
		std::optional<timespec> mtime = std::nullopt;

		if(req.atime_update())
			atime = {static_cast<time_t>(req.atime_sec()), static_cast<long>(req.atime_nsec())};

		if(req.mtime_update())
			mtime = {static_cast<time_t>(req.mtime_sec()), static_cast<long>(req.mtime_nsec())};

		timespec ctime = {static_cast<time_t>(req.ctime_sec()), static_cast<long>(req.ctime_nsec())};

		co_await node_ops->utimensat(node, atime, mtime, ctime);

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::GetLinkRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->getLink(node, req.path());
		if(!result) {
			managarm::fs::SvrResponse resp;
			assert(result.error() == protocols::fs::Error::notDirectory);
			resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}

		if(std::get<0>(result.value())) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(std::get<0>(result.value())), node_ops);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(std::get<1>(result.value()));
			switch(std::get<2>(result.value())) {
			case FileType::directory:
				resp.set_file_type(managarm::fs::FileType::DIRECTORY);
				break;
			case FileType::regular:
				resp.set_file_type(managarm::fs::FileType::REGULAR);
				break;
			case FileType::symlink:
				resp.set_file_type(managarm::fs::FileType::SYMLINK);
				break;
			default:
				throw std::runtime_error("Unexpected file type");
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
			logBragiSerializedReply(ser);
		}else{
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::NodeTraverseLinksRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->traverseLinks(node, std::deque(req.path_segments().begin(), req.path_segments().end()));

		if (!result) {
			managarm::fs::NodeTraverseLinksResponse resp;
			if (result.error() == protocols::fs::Error::notDirectory) {
				resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);
			} else {
				assert(result.error() == protocols::fs::Error::fileNotFound);
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
			}

			auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_tail.error());
			logBragiReply(resp);
			co_return {};
		}

		auto [nodes, type, processedComponents] = result.value();

		managarm::fs::NodeTraverseLinksResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_links_traversed(processedComponents);
		switch(type) {
		case FileType::directory:
			resp.set_file_type(managarm::fs::FileType::DIRECTORY);
			break;
		case FileType::regular:
			resp.set_file_type(managarm::fs::FileType::REGULAR);
			break;
		case FileType::symlink:
			resp.set_file_type(managarm::fs::FileType::SYMLINK);
			break;
		default:
			throw std::runtime_error("Unexpected file type");
		}

		// TODO: this is a workaround for not being able to get the offer lane on the offer side
		helix::UniqueLane local_push, remote_push;
		std::tie(local_push, remote_push) = helix::createStream();

		for (auto &[_, id] : nodes) {
			resp.add_ids(id);
		}

		auto [send_resp, send_tail, push_desc] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(remote_push)
		);

		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(push_desc.error());
		logBragiReply(resp);

		for (auto &[node, _] : nodes) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(node), node_ops);

			auto [push_node] = co_await helix_ng::exchangeMsgs(
				local_push,
				helix_ng::pushDescriptor(remote_lane)
			);

			HEL_CHECK(push_node.error());
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::MkdirRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->mkdir(node, req.path(), req.uid(), req.gid(), req.mode());

		if (result) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(std::get<0>(result.value())), node_ops);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(std::get<1>(result.value()));

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
			logBragiSerializedReply(ser);
		}else{
			managarm::fs::SvrResponse resp;
			resp.set_error(result.error() | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::LinkRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->link(node, req.path(), req.fd());
		if(result) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(std::get<0>(result.value())), node_ops);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(std::get<1>(result.value()));
			switch(std::get<2>(result.value())) {
			case FileType::directory:
				resp.set_file_type(managarm::fs::FileType::DIRECTORY);
				break;
			case FileType::regular:
				resp.set_file_type(managarm::fs::FileType::REGULAR);
				break;
			case FileType::symlink:
				resp.set_file_type(managarm::fs::FileType::SYMLINK);
				break;
			default:
				throw std::runtime_error("Unexpected file type");
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
			logBragiSerializedReply(ser);
		}else{
			managarm::fs::SvrResponse resp;
			resp.set_error(result.error() | toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::UnlinkRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->unlink(node, req.path());
		managarm::fs::SvrResponse resp;
		if(!result) {
			resp.set_error(result.error() | toFsError);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::RmdirRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->rmdir(node, req.path());
		managarm::fs::SvrResponse resp;
		if(!result) {
			resp.set_error(result.error() | toFsError);
			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			logBragiSerializedReply(ser);
			co_return {};
		}
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::NodeOpenRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();
		logBragiRequest(req);


		auto result = co_await node_ops->open(node, req.write(), req.read(), req.append());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp, push_file, push_pt] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::pushDescriptor(std::get<0>(result)),
			helix_ng::pushDescriptor(std::get<1>(result))
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(push_file.error());
		HEL_CHECK(push_pt.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::ChownRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();
		logBragiRequest(req);

		co_await node_ops->chown(
		    node,
		    (req.uid() == ~0U) ? std::nullopt : std::optional{req.uid()},
		    (req.gid() == ~0U) ? std::nullopt : std::optional{req.gid()}
		);

		managarm::fs::ChownResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::ObstructLinkRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		co_await node_ops->obstructLink(node, req.link_name());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::GetLinkOrCreateRequest &&req, helix::BorrowedDescriptor conversation,
			bragi::preamble preamble, std::shared_ptr<void> node,
			const NodeOperations *node_ops) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		auto result = co_await node_ops->getLinkOrCreate(node, req.name(), req.mode(), req.exclusive(),
			req.uid(), req.gid());

		managarm::fs::GetLinkOrCreateResponse resp;
		if (result) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveNode(std::move(local_lane), std::move(std::get<0>(result.value())), node_ops);

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(std::get<1>(result.value()));
			switch(std::get<2>(result.value())) {
				case FileType::directory:
					resp.set_file_type(managarm::fs::FileType::DIRECTORY);
					break;
				case FileType::regular:
					resp.set_file_type(managarm::fs::FileType::REGULAR);
					break;
				case FileType::symlink:
					resp.set_file_type(managarm::fs::FileType::SYMLINK);
					break;
				default:
					throw std::runtime_error("Unexpected file type");
			}
			auto [send_resp, send_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_node.error());
		} else {
			resp.set_error(result.error() | toFsError);
			auto [send_resp, dismiss] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::dismiss()
			);
			HEL_CHECK(send_resp.error());
		}

		logBragiReply(resp);
		co_return {};
	}

};


} // anonymous namespace

async::result<void>
serveFile(helix::UniqueLane lane, void *file, const FileOperations *file_ops) {
	(void)file;
	(void)file_ops;
	while(true) {
		auto [accept] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept()
		);

		if(accept.error() == kHelErrEndOfLane)
			co_return;

		assert(!"No operations are defined yet for the non-passthrough protocol");
	}
}

async::result<void> servePassthrough(helix::UniqueLane lane,
		smarter::shared_ptr<void> file, const FileOperations *file_ops,
		async::cancellation_token cancellation) {
	if(!ostraceInit) {
		ostraceInit = true;
		co_await initOstrace();
	}

	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(lane.getHandle()));
	}};

	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest,
			managarm::fs::RecvMsgRequest,
			managarm::fs::SendMsgRequest,
			managarm::fs::IoctlRequest,
			managarm::fs::SetSockOpt,
			managarm::fs::GetSockOpt,
			managarm::fs::ShutdownSocket,
			managarm::fs::ReadEntriesRequest,
			managarm::fs::CancelOperation,
			managarm::fs::FilePollRequest,
			managarm::fs::AcceptRequest,
			managarm::fs::ReadRequest,
			managarm::fs::PreadRequest,
			managarm::fs::WriteRequest,
			managarm::fs::PwriteRequest,
			managarm::fs::TruncateRequest,
			managarm::fs::FallocateRequest
		>(lane, DetachHandlers{}, HandleFileRequest{}, file, file_ops);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "protocols/fs: dispatch error on passthrough lane" << std::endl;
			continue;
		}
	}
}

StatusPageProvider::StatusPageProvider() {
	// Allocate and map our stauts page.
	size_t page_size = 4096;
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), page_size, 0, nullptr, &handle));
	_memory = helix::UniqueDescriptor{handle};
	_mapping = helix::Mapping{_memory, 0, page_size};
}

void StatusPageProvider::update(uint64_t sequence, int status) {
	auto page = reinterpret_cast<protocols::fs::StatusPage *>(_mapping.get());

	// State the seqlock write.
	auto seqlock = __atomic_load_n(&page->seqlock, __ATOMIC_RELAXED);
	assert(!(seqlock & 1));
	__atomic_store_n(&page->seqlock, seqlock + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);

	// Perform the actual update.
	__atomic_store_n(&page->sequence, sequence, __ATOMIC_RELAXED);
	__atomic_store_n(&page->status, status, __ATOMIC_RELAXED);

	// Complete the seqlock write.
	__atomic_store_n(&page->seqlock, seqlock + 2, __ATOMIC_RELEASE);
}

async::detached serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest,
			managarm::fs::UtimensatRequest,
			managarm::fs::GetLinkRequest,
			managarm::fs::NodeTraverseLinksRequest,
			managarm::fs::MkdirRequest,
			managarm::fs::LinkRequest,
			managarm::fs::UnlinkRequest,
			managarm::fs::RmdirRequest,
			managarm::fs::NodeOpenRequest,
			managarm::fs::ChownRequest,
			managarm::fs::ObstructLinkRequest,
			managarm::fs::GetLinkOrCreateRequest
		>(lane, HandleNodeRequest{}, node, node_ops);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "protocols/fs: dispatch error on node lane" << std::endl;
			continue;
		}
	}
}

} // namespace protocols::fs
