#include <initgraph.hpp>
#include <eir-internal/debug.hpp>

namespace eir {

struct GlobalInitHooks final : initgraph::Hooks {
	void preActivate(initgraph::Node *node) override {
		infoLogger() << "eir: Running " << node->displayName() << frg::endlog;
	}

	void onUnreached() override {
		infoLogger() << "eir: initgraph has cycles" << frg::endlog;

		while(1) { }
	}
};

constinit GlobalInitHooks globalInitHooks;
constinit initgraph::Engine globalInitEngine{&globalInitHooks};

initgraph::Stage *getMemoryDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-discovered"};
	return &s;
}

initgraph::Stage *getMemorySetUpStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-set-up"};
	return &s;
}

initgraph::Stage *getKernelAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.kernel-available"};
	return &s;
}

initgraph::Stage *getBootInfoBuildableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.boot-info-buildable"};
	return &s;
}

struct GlobalCtorTest {
	GlobalCtorTest() {
		infoLogger() << "Hello world from global ctor" << frg::endlog;
	}
};

GlobalCtorTest globalCtorTest;

using InitializerPtr = void (*)();
extern "C" InitializerPtr __init_array_start[];
extern "C" InitializerPtr __init_array_end[];

extern "C" void eirRunConstructors() {
	infoLogger() << "There are "
			<< (__init_array_end - __init_array_start) << " constructors" << frg::endlog;
	for(InitializerPtr *p = __init_array_start; p != __init_array_end; ++p)
			(*p)();
}

extern "C" void eirMain() {
	infoLogger() << "Hello world from generic eirMain()" << frg::endlog;

	globalInitEngine.run();

	while(1) { }
}

} // namespace eir
