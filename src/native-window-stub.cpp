// Window layer for platforms that do not have one yet.
//
// The host still runs headless here: everything but `gui open` works, and the
// error says plainly what is missing.
#include "native-window.h"

#include <clap/clap.h>

#include <chrono>
#include <thread>

namespace nch {

void setQuitHandler(std::function<void()>) {}

void prepareApplication() {}

std::unique_ptr<NativeWindow> createNativeWindow(uint32_t, uint32_t, const std::string &, std::string &error) {
	error = "this build has no window layer for this platform yet";
	return nullptr;
}

const char *nativeWindowApi() {
#if defined(_WIN32)
	return CLAP_WINDOW_API_WIN32;
#else
	return CLAP_WINDOW_API_X11;
#endif
}

void runApplicationLoop(const std::function<bool()> &tick, int intervalMs) {
	while (tick())
		std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
}

} // namespace nch
