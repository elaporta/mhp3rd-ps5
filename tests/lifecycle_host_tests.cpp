// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>

extern "C" [[noreturn]] void catchReturnFromMain(int);
extern "C" [[noreturn]] void __wrap_exit(int);
namespace {
struct StopWaiting {};
int exit_result, requests, waits, stop_after, notifications;
bool valid;
std::string last_notification;
}
extern "C" int sceSystemServiceLoadExec(const char* path, const char* argv[]) {
    ++requests;
    valid = valid && std::strcmp(path, "exit") == 0 && argv == nullptr;
    return exit_result;
}
extern "C" int sceKernelSendNotificationRequest(std::uint32_t device, void* request, std::size_t size, int blocking) {
    ++notifications;
    valid = valid && device == 0 && size == 3120 && blocking == 0;
    if (size == 3120) last_notification = static_cast<char*>(request) + 45;
    return 0;
}
extern "C" int usleep(useconds_t delay) {
    valid = valid && delay == 100000;
    if (++waits == stop_after) throw StopWaiting{};
    return 0;
}
int main() {
    // Model asynchronous success and a refused closure request. Neither may
    // return to the CRT or call POSIX exit; both keep waiting for the system.
    for (auto finish : {catchReturnFromMain, __wrap_exit}) {
        for (int result : {0, -1}) {
            exit_result = result;
            requests = waits = notifications = 0;
            stop_after = result == 0 ? 3 : 101;
            valid = true;
            last_notification.clear();
            try { finish(3); } catch (const StopWaiting&) {}
            if (!valid || requests != 1 || waits != stop_after) return 1;
            if (result == 0 && notifications != 0) return 2;
            if (result != 0 && (notifications != 1 || last_notification.find("home screen") == std::string::npos)) return 3;
        }
    }
    std::puts("PASS system-exit request and safe waiting on asynchronous success/refusal");
}
