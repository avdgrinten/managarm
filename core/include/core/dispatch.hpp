#pragma once

#include <expected>
#include <vector>

#include <async/result.hpp>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>
#include <helix/ipc.hpp>

enum class DispatchError {
	none, ///< No error
	shutdown, ///< The IPC lane was shut down (e.g. due to a client exit)
	ipcError, ///< An IPC error occured
	malformedMessage, ///< The received message could not be decoded
};

// Helper function for dispatchRequests().
template <typename Message, typename Visitor, typename ...Args>
async::result<DispatchError>
doDispatch(helix_ng::RecvInlineResult &recvHead, helix::BorrowedDescriptor conversation, bragi::preamble preamble, Visitor &&visitor, Args &&...args) {
	// Eagerly reset() after parsing to free up space in IPC queue.
	auto maybeMessage = bragi::parse_head_only<Message>(recvHead);
	recvHead.reset();
	if (!maybeMessage)
		co_return DispatchError::malformedMessage;

	auto res = co_await visitor(
		std::move(*maybeMessage), conversation, preamble,
		std::forward<Args>(args)...);
	if (!res)
		co_return res.error();
	co_return DispatchError::none;
}

// Helper function for dispatchRequest(); clients detect unsupported requests by the dismissal.
inline async::result<DispatchError>
dismissUnknownRequest(helix::BorrowedDescriptor conversation) {
	auto [dismiss] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
	if (dismiss.error() != kHelErrNone)
		co_return DispatchError::ipcError;
	co_return DispatchError::malformedMessage;
}

//! Tag type to select the handler-detaching overload of dispatchRequest().
struct DetachHandlers {};

// Helper: detached handler coroutine that owns the conversation lane.
template <typename Message, typename Visitor, typename ...Args>
async::detached
doDispatchDetach_(helix::UniqueDescriptor conversation, Message message,
                  bragi::preamble preamble, Visitor visitor, Args ...args) {
	co_await visitor(std::move(message), helix::BorrowedDescriptor{conversation},
	                 preamble, std::move(args)...);
}

// Helper function for the DetachHandlers overload of dispatchRequest().
template <typename Message, typename Visitor, typename ...Args>
DispatchError
doDispatchDetach(helix_ng::RecvInlineResult &recvHead,
                 helix::UniqueDescriptor conversation,
                 bragi::preamble preamble, Visitor &&visitor, Args &&...args) {
	// Eagerly reset() after parsing to free up space in IPC queue.
	auto maybeMessage = bragi::parse_head_only<Message>(recvHead);
	recvHead.reset();
	if (!maybeMessage)
		return DispatchError::malformedMessage;
	doDispatchDetach_<Message>(std::move(conversation), std::move(*maybeMessage),
	                           preamble, std::forward<Visitor>(visitor),
	                           std::forward<Args>(args)...);
	return DispatchError::none;
}

//! Receive and dispatch the incoming message to the provided handler.
//!
//! This function accepts a new conversation lane, receives a Bragi message head,
//! and invokes the given function object with the message, conversation lane, preamble,
//! and any extra arguments specified.
//!
//! The function object is expected to be callable with all possible message
//! types. Handling different message types can be done by having multiple
//! overloads for operator() for each message type.
//!
//! For messages that have a tail, the handler should call dispatchTail() to
//! receive and parse it.
//!
//! @param[in] lane
//! Lane on which to accept.
//! @param[in] visitor
//! Visitor function object invoked to handle the message.
//! @param[in] args
//! Extra arguments to pass when invoking message handler.
template <typename ...Messages, typename Visitor, typename ...Args>
async::result<std::expected<void, DispatchError>>
dispatchRequest(helix::BorrowedLane lane, Visitor &&visitor, Args &&...args) {
	auto [accept, recvHead] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::accept(
			helix_ng::recvInline()
		)
	);

	// Note: only accept can ever return in DispatchError::shutdown.
	//       Failures in any other message item are always protocol violations (and hence ipcError).
	if (accept.error() == kHelErrLaneShutdown || accept.error() == kHelErrEndOfLane)
		co_return std::unexpected(DispatchError::shutdown);
	if (accept.error() != kHelErrNone)
		co_return std::unexpected(DispatchError::ipcError);
	if (recvHead.error() != kHelErrNone)
		co_return std::unexpected(DispatchError::ipcError);

	auto conversation = accept.descriptor();

	auto preamble = bragi::read_preamble(recvHead);

	if (preamble.error())
		co_return std::unexpected(DispatchError::malformedMessage);

	DispatchError err = DispatchError::none;

	bool found = (
		(
			preamble.id() == bragi::message_id<Messages>
			? (err = co_await doDispatch<Messages>(recvHead, std::move(conversation), preamble,
				std::forward<Visitor>(visitor), std::forward<Args>(args)...), true)
			: false
		) || ...
	);
	if (!found)
		co_return std::unexpected(co_await dismissUnknownRequest(conversation));
	if (err != DispatchError::none)
		co_return std::unexpected(err);
	co_return {};
}

//! dispatchRequest() overload that detaches each handler coroutine.
//!
//! The accept/recv/preamble steps are awaited normally. Once the matching message
//! type is found and parsed, the handler is launched as a detached coroutine and
//! this function returns immediately, allowing the caller to accept the next
//! request before the previous handler has completed.
//!
//! Ownership of the accepted conversation lane is transferred to the detached
//! handler. The visitor and extra arguments must be move-constructible.
//!
//! @param[in] lane
//! Lane on which to accept.
//! @param[in] visitor
//! Visitor function object invoked to handle the message (taken by value).
//! @param[in] args
//! Extra arguments to pass when invoking message handler (taken by value).
template <typename ...Messages, typename Visitor, typename ...Args>
async::result<std::expected<void, DispatchError>>
dispatchRequest(helix::BorrowedLane lane, DetachHandlers, Visitor visitor, Args ...args) {
	auto [accept, recvHead] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::accept(
			helix_ng::recvInline()
		)
	);

	// Note: only accept can ever return in DispatchError::shutdown.
	//       Failures in any other message item are always protocol violations (and hence ipcError).
	if (accept.error() == kHelErrLaneShutdown || accept.error() == kHelErrEndOfLane)
		co_return std::unexpected(DispatchError::shutdown);
	if (accept.error() != kHelErrNone)
		co_return std::unexpected(DispatchError::ipcError);
	if (recvHead.error() != kHelErrNone)
		co_return std::unexpected(DispatchError::ipcError);

	auto conversation = accept.descriptor();

	auto preamble = bragi::read_preamble(recvHead);

	if (preamble.error())
		co_return std::unexpected(DispatchError::malformedMessage);

	DispatchError err = DispatchError::none;

	bool found = (
		(
			preamble.id() == bragi::message_id<Messages>
			? (err = doDispatchDetach<Messages>(recvHead, std::move(conversation), preamble,
				std::move(visitor), std::move(args)...), true)
			: false
		) || ...
	);
	if (!found)
		co_return std::unexpected(co_await dismissUnknownRequest(conversation));
	if (err != DispatchError::none)
		co_return std::unexpected(err);
	co_return {};
}

//! dispatchRequest() overload that takes an already received RecvInlineResult.
template <typename ...Messages, typename Visitor, typename ...Args>
async::result<std::expected<void, DispatchError>>
dispatchRequest(helix::BorrowedLane conversation, helix_ng::RecvInlineResult recvHead, Visitor &&visitor, Args &&...args) {
	auto preamble = bragi::read_preamble(recvHead);

	if (preamble.error())
		co_return std::unexpected(DispatchError::malformedMessage);

	DispatchError err = DispatchError::none;

	bool found = (
		(
			preamble.id() == bragi::message_id<Messages>
			? (err = co_await doDispatch<Messages>(recvHead, conversation, preamble,
				std::forward<Visitor>(visitor), std::forward<Args>(args)...), true)
			: false
		) || ...
	);
	if (!found)
		co_return std::unexpected(DispatchError::malformedMessage);
	if (err != DispatchError::none)
		co_return std::unexpected(err);
	co_return {};
}

//! Receive the tail of a message and decode it into an existing message.
//!
//! Optionally receives additional message items alongside the tail in a single
//! exchangeMsgs call. The results of the additional items are returned on success.
//!
//! @param[in] conversation
//! Lane on which to receive the tail.
//! @param[in] preamble
//! The preamble (containing the tail size) obtained from dispatchRequest.
//! @param[in,out] msg
//! The message object into which the tail will be decoded.
//! @param[in] items
//! Additional message items to receive alongside the tail.
template <typename Message, typename ...Items>
async::result<std::expected<decltype(helix_ng::createResultsTuple(std::declval<Items>()...)), DispatchError>>
dispatchTail(Message &msg, helix::BorrowedDescriptor conversation, bragi::preamble preamble, Items &&...items) {
	using ExtraResults = decltype(helix_ng::createResultsTuple(std::declval<Items>()...));

	std::vector<char> tail(preamble.tail_size());
	auto results = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::recvBuffer(tail.data(), tail.size()),
		std::forward<Items>(items)...
	);

	bool anyError = [&]<std::size_t ...Is>(std::index_sequence<Is...>) {
		return ((results.template get<Is>().error() != kHelErrNone) || ...);
	}(std::make_index_sequence<1 + sizeof...(Items)>{});
	if (anyError)
		co_return std::unexpected(DispatchError::ipcError);

	bragi::limited_reader reader{tail.data(), tail.size()};
	if (!msg.decode_tail(reader))
		co_return std::unexpected(DispatchError::malformedMessage);

	co_return [&]<std::size_t ...Is>(std::index_sequence<Is...>) -> ExtraResults {
		return ExtraResults{std::move(results.template get<Is + 1>())...};
	}(std::make_index_sequence<std::tuple_size_v<ExtraResults>>{});
}
