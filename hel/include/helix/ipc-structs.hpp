#pragma once

#include <type_traits>
#include <frg/array.hpp>
#include <frg/tuple.hpp>

#include <hel.h>
#include <hel-syscalls.h>

#include <frg/macros.hpp>

namespace helix {

struct UniqueDescriptor {
	friend void swap(UniqueDescriptor &a, UniqueDescriptor &b) {
		using std::swap;
		swap(a._handle, b._handle);
	}

	UniqueDescriptor()
	: _handle(kHelNullHandle) { }
	
	UniqueDescriptor(const UniqueDescriptor &other) = delete;

	UniqueDescriptor(UniqueDescriptor &&other)
	: UniqueDescriptor() {
		swap(*this, other);
	}

	explicit UniqueDescriptor(HelHandle handle)
	: _handle(handle) { }

	~UniqueDescriptor() {
		if(_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(kHelThisUniverse, _handle));
	}

	explicit operator bool () const {
		return _handle != kHelNullHandle;
	}

	UniqueDescriptor &operator= (UniqueDescriptor other) {
		swap(*this, other);
		return *this;
	}

	HelHandle getHandle() const {
		return _handle;
	}

	void release() {
		_handle = kHelNullHandle;
	}

	UniqueDescriptor dup() const {
		HelHandle new_handle;
		HEL_CHECK(helTransferDescriptor(getHandle(), kHelThisUniverse, &new_handle));
		return UniqueDescriptor(new_handle);
	}

private:
	HelHandle _handle;
};

struct BorrowedDescriptor {
	BorrowedDescriptor()
	: _handle(kHelNullHandle) { }
	
	BorrowedDescriptor(const BorrowedDescriptor &other) = default;
	BorrowedDescriptor(BorrowedDescriptor &&other) = default;

	explicit BorrowedDescriptor(HelHandle handle)
	: _handle(handle) { }
	
	BorrowedDescriptor(const UniqueDescriptor &other)
	: BorrowedDescriptor(other.getHandle()) { }

	~BorrowedDescriptor() = default;

	BorrowedDescriptor &operator= (const BorrowedDescriptor &) = default;

	HelHandle getHandle() const {
		return _handle;
	}

	UniqueDescriptor dup() const {
		HelHandle new_handle;
		HEL_CHECK(helTransferDescriptor(getHandle(), kHelThisUniverse, &new_handle));
		return UniqueDescriptor(new_handle);
	}

private:
	HelHandle _handle;
};


} // namespace helix

namespace helix_ng {

using namespace helix;

struct OfferResult {
	OfferResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
};

struct AcceptResult {
	AcceptResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	UniqueDescriptor descriptor() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		_error = result->error;
		_descriptor = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
	UniqueDescriptor _descriptor;
};

struct ImbueCredentialsResult {
	ImbueCredentialsResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
};

struct ExtractCredentialsResult {
	ExtractCredentialsResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	char *credentials() {
		FRG_ASSERT(_valid);
		return _credentials;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelCredentialsResult *>(ptr);
		_error = result->error;
		memcpy(_credentials, result->credentials, 16);
		ptr = (char *)ptr + sizeof(HelCredentialsResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
	char _credentials[16];
};

struct SendBufferResult {
	SendBufferResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
};

struct RecvBufferResult {
	RecvBufferResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	size_t actualLength() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _length;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelLengthResult *>(ptr);
		_error = result->error;
		_length = result->length;
		ptr = (char *)ptr + sizeof(HelLengthResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
	size_t _length;
};

struct RecvInlineResult {
	RecvInlineResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void *data() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _data;
	}

	size_t length() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _length;
	}

	void parse(void *&ptr, ElementHandle element) {
		auto result = reinterpret_cast<HelInlineResult *>(ptr);
		_error = result->error;
		_length = result->length;
		_data = result->data;

		_element = element;

		ptr = (char *)ptr + sizeof(HelInlineResult)
			+ ((_length + 7) & ~size_t(7));
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
	ElementHandle _element;
	void *_data;
	size_t _length;
};

struct PushDescriptorResult {
	PushDescriptorResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
};

struct PullDescriptorResult {
	PullDescriptorResult() :_valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	UniqueDescriptor descriptor() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		_error = result->error;
		_descriptor = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		_valid = true;
	}

private:
	bool _valid;
	HelError _error;
	UniqueDescriptor _descriptor;
};


// --------------------------------------------------------------------
// Items
// --------------------------------------------------------------------

template <typename ...T>
struct Offer {
	frg::tuple<T...> nested_actions;
};

template <typename ...T>
struct Accept {
	frg::tuple<T...> nested_actions;
};

struct ImbueCredentials { };
struct ExtractCredentials { };

struct SendBuffer {
	const void *buf;
	size_t size;
};

struct RecvBuffer {
	void *buf;
	size_t size;
};

struct RecvInline { };

struct PushDescriptor {
	HelHandle handle;
};

struct PullDescriptor { };

// --------------------------------------------------------------------
// Construction functions
// --------------------------------------------------------------------

template <typename ...T>
inline auto offer(T &&...args) {
	return Offer<T...>{frg::make_tuple(args...)};
}

template <typename ...T>
inline auto accept(T &&...args) {
	return Accept<T...>{frg::make_tuple(args...)};
}

inline auto imbueCredentials() {
	return ImbueCredentials{};
}

inline auto extractCredentials() {
	return ExtractCredentials{};
}

inline auto sendBuffer(const void *data, size_t length) {
	return SendBuffer{data, length};
}

inline auto recvBuffer(void *data, size_t length) {
	return RecvBuffer{data, length};
}

inline auto recvInline() {
	return RecvInline{};
}

inline auto pushDescriptor(BorrowedDescriptor desc) {
	return PushDescriptor{desc.getHandle()};
}

inline auto pullDescriptor() {
	return PullDescriptor{};
}

// --------------------------------------------------------------------
// Item -> HelAction transformation
// --------------------------------------------------------------------

struct {
	template <typename T, typename ...Ts>
	auto operator() (T &&arg, Ts &&...args) {
		return frg::array_concat<HelAction>(
			createActionsArrayFor(true, std::forward<T>(arg)),
			this->operator()<Ts...>(std::forward<Ts>(args)...)
		);
	}

	template <typename T>
	auto operator() (T &&arg) {
		return createActionsArrayFor(false, std::forward<T>(arg));
	}

	auto operator() () {
		return frg::array<HelAction, 0>{};
	}
} chainActionArrays;

template <typename ...T>
inline auto createActionsArrayFor(bool chain, const Offer<T...> &o) {
	HelAction action{};
	action.type = kHelActionOffer;
	action.flags = (chain ? kHelItemChain : 0)
			| (std::tuple_size_v<decltype(o.nested_actions)> > 0 ? kHelItemAncillary : 0);

	return frg::array_concat<HelAction>(
		frg::array<HelAction, 1>{action},
		frg::apply(chainActionArrays, o.nested_actions)
	);
}

template <typename ...T>
inline auto createActionsArrayFor(bool chain, const Accept<T...> &o) {
	HelAction action{};
	action.type = kHelActionAccept;
	action.flags = (chain ? kHelItemChain : 0)
			| (std::tuple_size_v<decltype(o.nested_actions)> > 0 ? kHelItemAncillary : 0);

	return frg::array_concat<HelAction>(
		frg::array<HelAction, 1>{action},
		frg::apply(chainActionArrays, o.nested_actions)
	);
}

inline auto createActionsArrayFor(bool chain, const ImbueCredentials &) {
	HelAction action{};
	action.type = kHelActionImbueCredentials;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const ExtractCredentials &) {
	HelAction action{};
	action.type = kHelActionExtractCredentials;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const SendBuffer &item) {
	HelAction action{};
	action.type = kHelActionSendFromBuffer;
	action.flags = chain ? kHelItemChain : 0;
	action.buffer = const_cast<void *>(item.buf);
	action.length = item.size;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const RecvBuffer &item) {
	HelAction action{};
	action.type = kHelActionRecvToBuffer;
	action.flags = chain ? kHelItemChain : 0;
	action.buffer = item.buf;
	action.length = item.size;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const RecvInline &) {
	HelAction action{};
	action.type = kHelActionRecvInline;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const PushDescriptor &item) {
	HelAction action{};
	action.type = kHelActionPushDescriptor;
	action.flags = chain ? kHelItemChain : 0;
	action.handle = item.handle;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const PullDescriptor &) {
	HelAction action{};
	action.type = kHelActionPullDescriptor;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

// --------------------------------------------------------------------
// Item -> Result type transformation
// --------------------------------------------------------------------

template <typename ...T>
inline auto resultTypeTuple(Offer<T...> arg) {
	return frg::tuple_cat(frg::tuple<OfferResult>{}, resultTypeTuple(T{})...);
}

template <typename ...T>
inline auto resultTypeTuple(Accept<T...> arg) {
	return frg::tuple_cat(frg::tuple<AcceptResult>{}, resultTypeTuple(T{})...);
}

inline auto resultTypeTuple(ImbueCredentials arg) {
	return frg::tuple<ImbueCredentialsResult>{};
}

inline auto resultTypeTuple(ExtractCredentials arg) {
	return frg::tuple<ExtractCredentialsResult>{};
}

inline auto resultTypeTuple(SendBuffer arg) {
	return frg::tuple<SendBufferResult>{};
}

inline auto resultTypeTuple(RecvBuffer arg) {
	return frg::tuple<RecvBufferResult>{};
}

inline auto resultTypeTuple(RecvInline arg) {
	return frg::tuple<RecvInlineResult>{};
}

inline auto resultTypeTuple(PushDescriptor arg) {
	return frg::tuple<PushDescriptorResult>{};
}

inline auto resultTypeTuple(PullDescriptor arg) {
	return frg::tuple<PullDescriptorResult>{};
}

template <typename ...T>
inline auto createResultsTuple(T &&...args) {
	return frg::tuple_cat(resultTypeTuple(args)...);
}

} // namespace helix_ng
