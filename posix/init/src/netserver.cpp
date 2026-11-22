#include <assert.h>
#include <helix/ipc.hpp>

#include "netserver.hpp"

mbus_ng::EntityId findNetserver() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "netserver"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = async::run(enumerator.nextEvents(), helix::currentDispatcher).unwrap();
	assert(events.size() == 1);

	return events[0].id;
}
