#include <eir-internal/debug.hpp>

extern "C" void eirMain() {
	eir::infoLogger() << "Hello world from generic eirMain()" << frg::endlog;

	while(1) { }
}
