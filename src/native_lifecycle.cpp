// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cstdint>
#include <unistd.h>

extern "C" int sceSystemServiceLoadExec(const char*, const char*[]);
extern "C" int sceKernelSendNotificationRequest(std::uint32_t, void*, std::size_t, int);

namespace yakumo::ps5 {
void notify_user(const char* message) {
    // Public PS5_Vulkan demo_renderer.cpp at the locked revision describes this ABI.
    struct Notification { std::uint8_t reserved[45]; char text[3075]; } request{};
    static_assert(sizeof(request) == 3120);
    std::snprintf(request.text, sizeof(request.text), "%s", message);
    const int rc = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    Log log;
    log.write("[notification] rc=0x%08x message=%s", static_cast<unsigned>(rc), message);
}
}

namespace {
[[noreturn]] void finish(int status) {
    using namespace yakumo::ps5;
    Log log;
    log.write("[lifecycle] main status=%d; requesting system application exit", status);
    const int rc = sceSystemServiceLoadExec("exit", nullptr);
    log.write("[lifecycle] LoadExec rc=0x%08x; waiting for system closure", static_cast<unsigned>(rc));
    // Exit is asynchronous. Never fall back to POSIX exit/_exit: the failed
    // 0.1.0 binary reached SIGSYS through that path on the user's console.
    for (int i = 0; i < 100; ++i) usleep(100000);
    notify_user("Yakumo probe: system exit did not close the app. Close it from the PS5 home screen.");
    for (;;) usleep(100000);
}
}

// Override the pinned CRT's weak hook without editing its source. Wrapping exit
// also removes its otherwise unreachable POSIX exit import from the executable.
extern "C" [[noreturn]] void catchReturnFromMain(int status) { finish(status); }
extern "C" [[noreturn]] void __wrap_exit(int status) { finish(status); }
