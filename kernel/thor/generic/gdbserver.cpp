#include <async/queue.hpp>
#include <frg/span.hpp>
#include <frg/vector.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/gdbserver.hpp>
#include <thor-internal/kernel-io.hpp>

namespace thor {

namespace {

enum class ProtocolError {
	none,
	unknownPacket,
	malformedPacket
};

enum class ResponseStage {
	none,
	responseReady,
	responseSent
};

void hexdump(frg::span<uint8_t> s) {
	for(size_t i = 0; i < s.size(); i += 8) {
		auto log = infoLogger();
		log << "   ";
		for(size_t j = 0; j < 8; ++j) {
			if(i + j >= s.size())
				log << "   ";
			else
				log << " " << frg::hex_fmt(s[i + j]);
		}
		log << "    |";
		for(size_t j = 0; j < 8; ++j) {
			if(i + j >= s.size())
				log << " ";
			else if(s[i + j] < 32 || s[i + j] >= 127)
				log << ".";
			else
				log << frg::char_fmt(s[i + j]);
		}
		log << "|";
		log << frg::endlog;
	}
}

bool is_hex(uint8_t h) {
	return (h >= 'a' && h <= 'f') || (h >= '0' && h <= '9');
}

unsigned int hex2int(uint8_t h) {
	if(h >= 'a' && h <= 'f')
		return 10 + h - 'a';
	assert(h >= '0' && h <= '9');
	return h - '0';
}

uint8_t int2hex(unsigned int v) {
	if(v < 10)
		return '0' + v;
	assert(v < 16);
	return 'a' + v - 10;
}

uint8_t computeCsum(frg::span<uint8_t> s) {
	uint8_t sum = 0;
	for(size_t i = 0; i < s.size(); ++i)
		sum += s[i];
	return sum;
}

struct GdbServer {
	GdbServer(smarter::shared_ptr<Thread, ActiveHandle> thread, frg::string_view path,
		smarter::shared_ptr<KernelIoChannel> channel)
	: thread_{std::move(thread)}, path_{path}, channel_{std::move(channel)} { }

	coroutine<frg::expected<Error>> run();

private:
	coroutine<frg::expected<ProtocolError>> handleRequest_();

	smarter::shared_ptr<Thread, ActiveHandle> thread_;
	frg::string_view path_;
	smarter::shared_ptr<KernelIoChannel> channel_;

	// Internal buffer for parsing / emitting packets.
	frg::vector<uint8_t, KernelAlloc> inBuffer_{*kernelAlloc};
	frg::vector<uint8_t, KernelAlloc> outBuffer_{*kernelAlloc};

	// Whether we are currently sending a response or not.
	ResponseStage responseStage_ = ResponseStage::none;
};

struct ParseView {
	ParseView() = default;

	ParseView(frg::span<uint8_t> bs)
	: bs_{bs} { }

	bool matchString(const char *s) {
		size_t n;
		for(n = 0; s[n]; ++n) {
			if(n == bs_.size())
				return false;
			if(bs_[n] != static_cast<uint8_t>(s[n]))
				return false;
		}
		bs_ = bs_.subspan(n);
		return true;
	}

	bool matchFullString(const char *s) {
		return matchString(s) && fullyConsumed();
	}

	bool splitDelimiter(ParseView &out, char c) {
		for(size_t n = 0; n < bs_.size(); ++n) {
			if(bs_[n] != static_cast<uint8_t>(c))
				continue;
			out = frg::span<uint8_t>{bs_.data(), n};
			bs_ = bs_.subspan(n + 1);
			return true;
		}
		return false;
	}

	bool parseHex64(uint64_t &out) {
		// Parse the first hex char.
		if(!bs_.size())
			return false;
		if(!is_hex(bs_[0]))
			return false;
		uint64_t v = hex2int(bs_[0]);

		// Parse additional hex chars.
		size_t n;
		for(n = 1; n < bs_.size(); ++n) {
			auto c = bs_[n];
			if(is_hex(c))
				v = (v << 4) | hex2int(c);
			else
				break;
		}

		bs_ = bs_.subspan(n);
		out = v;
		return true;
	}

	bool fullyConsumed() {
		return !bs_.size();
	}

private:
	frg::span<uint8_t> bs_;
};

struct EmitOverlay {
	EmitOverlay(frg::vector<uint8_t, KernelAlloc> *buf)
	: buf_{buf} { }

	// Append Enn which some commands use to indicate an error (with nn begin the error code).
	// Since error codes are not well-defined according to the GDB protocol documentation,
	// we simply send E01 here.
	void appendError() {
		buf_->push_back('E');
		buf_->push_back('0');
		buf_->push_back('1');
	}

	void appendString(const char *s) {
		for(size_t n = 0; s[n]; ++n)
			buf_->push_back(s[n]);
	}

	void appendHexByte(uint8_t b) {
		buf_->push_back(int2hex(b >> 4));
		buf_->push_back(int2hex(b & 0xF));
	}

	// Append a 32-bit integer in little-endian hex encoding.
	void appendLeHex32(uint32_t v) {
		for(int i = 0; i < 4; ++i) {
			uint8_t b = v >> (i * 8);
			buf_->push_back(int2hex(b >> 4));
			buf_->push_back(int2hex(b & 0xF));
		}
	}

	// Append a 64-bit integer in little-endian hex encoding.
	void appendLeHex64(uint64_t v) {
		for(int i = 0; i < 8; ++i) {
			uint8_t b = v >> (i * 8);
			buf_->push_back(int2hex(b >> 4));
			buf_->push_back(int2hex(b & 0xF));
		}
	}

	void appendBinary(frg::span<const uint8_t> b) {
		for(size_t i = 0; i < b.size(); ++i) {
			switch(b[i]) {
			case '}':
			case '$':
			case '#':
			case '*':
				buf_->push_back('}');
				buf_->push_back(b[i] ^ 0x20);
				break;
			default:
				buf_->push_back(b[i]);
			}
		}
	}

private:
	frg::vector<uint8_t, KernelAlloc> *buf_;
};

coroutine<frg::expected<Error>> GdbServer::run() {
	while(true) {
		if(responseStage_ == ResponseStage::responseReady) {
			// Send the packet.
			FRG_CO_TRY(co_await channel_->postOutput('$'));
			for(size_t i = 0; i < outBuffer_.size(); ++i)
				FRG_CO_TRY(co_await channel_->postOutput(outBuffer_[i]));
			FRG_CO_TRY(co_await channel_->postOutput('#'));

			auto csum = computeCsum({outBuffer_.data(), outBuffer_.size()});
			FRG_CO_TRY(co_await channel_->postOutput(int2hex(csum >> 4)));
			FRG_CO_TRY(co_await channel_->postOutput(int2hex(csum & 0xF)));
			FRG_CO_TRY(co_await channel_->flushOutput());
			responseStage_ = ResponseStage::responseSent;
		}

		auto firstByte = FRG_CO_TRY(co_await channel_->readInput());

		if(firstByte == '$') {
			inBuffer_.clear();

			// Process the bytes.
			while(true) {
				auto byte = FRG_CO_TRY(co_await channel_->readInput());
				if(byte == '#')
					break;
				inBuffer_.push_back(byte);
			}

			auto csumByte1 = FRG_CO_TRY(co_await channel_->readInput());
			auto csumByte2 = FRG_CO_TRY(co_await channel_->readInput());

			if(responseStage_ != ResponseStage::none) {
				infoLogger() << "thor, gdbserver: Ignoring ill-sequenced request" << frg::endlog;
				continue;
			}

			// Verify checksum.
			if(!is_hex(csumByte1) || !is_hex(csumByte2)) {
				infoLogger() << "thor, gdbserver: NACK due to missing checksum" << frg::endlog;
				FRG_CO_TRY(co_await channel_->writeOutput('-'));
				continue;
			}
			auto csum = (hex2int(csumByte1) << 4) | hex2int(csumByte2);
			auto expectedCsum = computeCsum({inBuffer_.data(), inBuffer_.size()});
			if(csum != expectedCsum) {
				infoLogger() << "thor, gdbserver: NACK due to checksum mismatch" << frg::endlog;
				FRG_CO_TRY(co_await channel_->writeOutput('-'));
				continue;
			}

			// Ack the packet.
			FRG_CO_TRY(co_await channel_->writeOutput('+'));

			auto outcome = co_await handleRequest_();
			if(!outcome) {
				if(outcome.error() == ProtocolError::unknownPacket) {
					infoLogger() << "thor, gdbserver: Unknown packet,"
							" dumping:" << frg::endlog;
				}else{
					assert(outcome.error() == ProtocolError::malformedPacket);
					infoLogger() << "thor, gdbserver: Remote violated procotol specification,"
							" dumping:" << frg::endlog;
				}
				hexdump({inBuffer_.data(), inBuffer_.size()});
			}

			responseStage_ = ResponseStage::responseReady;
		}else if(firstByte == '+') {
			if(responseStage_ == ResponseStage::responseSent) {
				outBuffer_.clear();
				responseStage_ = ResponseStage::none;
			}else{
				infoLogger() << "thor, gdbserver: Ignoring stray ACK" << frg::endlog;
			}
		}else if(firstByte == '-') {
			if(responseStage_ == ResponseStage::responseSent) {
				outBuffer_.clear();
				responseStage_ = ResponseStage::responseReady;
			}else{
				infoLogger() << "thor, gdbserver: Ignoring stray NACK" << frg::endlog;
			}
		}else{
			infoLogger() << "thor, gdbserver: Packet starts with unexpected byte: "
					<< frg::hex_fmt(firstByte) << frg::endlog;
		}
	}
}

coroutine<frg::expected<ProtocolError>> GdbServer::handleRequest_() {
	assert(outBuffer_.empty());
	ParseView req{{inBuffer_.data(), inBuffer_.size()}};
	EmitOverlay resp{&outBuffer_};

	if(req.matchString("H")) { // Set thread.
		// TODO: consider the argument (= thread ID).
		resp.appendString("OK");
	}else if(req.matchString("?")) { // Reason for stopping.
		if(!req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		resp.appendString("S0b");
	}else if(req.matchString("g")) { // Read registers.
		if(!req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		auto accessOutcome = thread_->accessRegisters([&](Executor *executor) {
#if defined(__x86_64__)
			resp.appendLeHex64(executor->general()->rax);
			resp.appendLeHex64(executor->general()->rbx);
			resp.appendLeHex64(executor->general()->rcx);
			resp.appendLeHex64(executor->general()->rdx);
			resp.appendLeHex64(executor->general()->rsi);
			resp.appendLeHex64(executor->general()->rdi);
			resp.appendLeHex64(executor->general()->rbp);
			resp.appendLeHex64(*executor->sp());
			resp.appendLeHex64(executor->general()->r8);
			resp.appendLeHex64(executor->general()->r9);
			resp.appendLeHex64(executor->general()->r10);
			resp.appendLeHex64(executor->general()->r11);
			resp.appendLeHex64(executor->general()->r12);
			resp.appendLeHex64(executor->general()->r13);
			resp.appendLeHex64(executor->general()->r14);
			resp.appendLeHex64(executor->general()->r15);
			resp.appendLeHex64(*executor->ip());
			resp.appendLeHex32(*executor->rflags());
			resp.appendLeHex32(*executor->cs());
			resp.appendLeHex32(*executor->ss());
			resp.appendLeHex32(*executor->ss()); // DS
			resp.appendLeHex32(*executor->ss()); // ES
			resp.appendLeHex32(executor->general()->clientFs);
			resp.appendLeHex32(executor->general()->clientGs);
			for(int i = 0; i < 8; ++i) // 8 FPU registers.
				for(int j = 0; j < 10; ++j) // 80 bits in size.
					resp.appendString("xx");
			for(int i = 0; i < 8; ++i) // 8 FPU control registers.
				for(int j = 0; j < 4; ++j)
					resp.appendString("xx");
#elif defined (__aarch64__)
			for (int i = 0; i < 31; i++)
				resp.appendLeHex64(executor->general()->x[i]);
			resp.appendLeHex64(executor->general()->sp);
			resp.appendLeHex64(executor->general()->elr);
			resp.appendLeHex32(executor->general()->spsr);
#elif defined(__riscv) && __riscv_xlen == 64
			warningLogger() << "GDB server is unimplemented for RISC-V" << frg::endlog;
#else
#	error Unknown architecture
#endif
		});
		if(!accessOutcome)
			resp.appendError();
	}else if(req.matchString("m")) { // Read memory.
		uint64_t address;
		uint64_t length;
		if(!req.parseHex64(address)
				|| !req.matchString(",")
				|| !req.parseHex64(length)
				|| !req.fullyConsumed())
			co_return ProtocolError::malformedPacket;

		frg::vector<uint8_t, KernelAlloc> mem{*kernelAlloc};
		mem.resize(length);
		auto actualLength = co_await thread_->getAddressSpace()->readPartialSpace(
				address, mem.data(), length);

		for(size_t i = 0; i < actualLength; ++i)
			resp.appendHexByte(mem[i]);
	}else if(req.matchString("q")) { // General query.
		if(req.matchString("Supported")) {
			resp.appendString("qXfer:exec-file:read+;");
			resp.appendString("qXfer:features:read+;");
		}else if(req.matchString("Xfer")) {
			ParseView object, annex;
			uint64_t offset, length;
			if(!req.matchString(":")
					|| !req.splitDelimiter(object, ':')
					|| !req.matchString("read:") // TODO: Support writes.
					|| !req.splitDelimiter(annex, ':')
					|| !req.parseHex64(offset)
					|| !req.matchString(",")
					|| !req.parseHex64(length))
				co_return ProtocolError::malformedPacket;

			frg::optional<frg::span<const uint8_t>> s;
			// buffer to hold the constructed absolute path if path_ is relative
			// needs to outlive the span s
			frg::vector<uint8_t, KernelAlloc> absolutePath{*kernelAlloc};

			if(object.matchFullString("exec-file")) {
				// TODO: consider the annex (= process ID).
				if(path_.size() > 0 && path_[0] == '/') {
					s = frg::span<const uint8_t>{reinterpret_cast<const uint8_t *>(path_.data()),
						path_.size()};
				} else {
					// Absolute path.
					absolutePath.resize(path_.size() + 1);
					absolutePath[0] = '/';
					memcpy(absolutePath.data() + 1, path_.data(), path_.size());
					s = frg::span<const uint8_t>{absolutePath.data(), absolutePath.size()};
				}
			}else if(object.matchFullString("features") && annex.matchFullString("target.xml")) {
				const char *xml = "<target version=\"1.0\">"
#if defined(__x86_64__)
					"<architecture>i386:x86-64</architecture>"
#elif defined(__aarch64__)
					"<architecture>aarch64</architecture>"
#elif defined(__riscv) && __riscv_xlen == 64
					"<architecture>riscv64</architecture>"
#else
#	error Unknown architecture
#endif
					"</target>";
				s = frg::span<const uint8_t>{reinterpret_cast<const uint8_t *>(xml),
						strlen(xml)};
			}

			if(s) {
				if(offset >= s->size()) {
					// End-of-object (offset beyond object size).
					resp.appendString("l");
				}else if(offset + length >= s->size()) {
					// End-of-object.
					resp.appendString("l");
					resp.appendBinary(s->subspan(offset));
				}else{
					// More data available.
					resp.appendString("m");
					resp.appendBinary(s->subspan(offset, length));
				}
			}
		}else{
			co_return ProtocolError::unknownPacket;
		}
	}else if(req.matchString("v")) { // Multi-letter requests.
		if(req.matchString("MustReplyEmpty")) {
			// Must be handled like unknown v packets (but do not complain).
			req = {}; // Fully consume the packet.
		}else{
			co_return ProtocolError::unknownPacket;
		}
	}else{
		co_return ProtocolError::unknownPacket;
	}

	co_return {};
}

} // anonymous namespace

static bool launched = false;

void launchGdbServer(smarter::shared_ptr<Thread, ActiveHandle> thread,
		frg::string_view path, smarter::borrowed_ptr<WorkQueue> wq) {
	if(launched)
		return;
	launched = true;
	infoLogger() << "thor: Launching gdbserver once an I/O channel is available" << frg::endlog;

	spawnOnWorkQueue(*kernelAlloc, wq.lock(),
			[] (smarter::shared_ptr<Thread, ActiveHandle> thread,
					frg::string_view path) -> coroutine<void> {
		auto channel = co_await solicitIoChannel("kernel-gdbserver");
		infoLogger() << "thor: Launching gdbserver on I/O channel "
				<< channel->descriptiveTag() << frg::endlog;

		auto svr = frg::construct<GdbServer>(*kernelAlloc,
				std::move(thread), path, std::move(channel));
		auto outcome = co_await svr->run();
		if(!outcome)
			infoLogger() << "thor: Internal error in gdbserver" << frg::endlog;
	}(std::move(thread), path));
}

} // namespace thor
