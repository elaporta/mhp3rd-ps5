// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_probe.hpp"
#if YAKUMO_WITH_PROFILE
#include "profile_probe.hpp"
#endif
#include <atomic>
#include <pthread.h>
#include <unistd.h>
#include <exception>

extern "C" [[noreturn]] void catchReturnFromMain(int);
namespace yakumo::ps5 {
namespace {
struct Watchdog {
    const TitlePaths* paths;
    const char* phase{"runtime"};
    std::atomic<bool> stop{false};
};
void* watch(void* argument) {
    auto& state = *static_cast<Watchdog*>(argument);
    // Independent of the C++ condition variable and clock currently under test.
    for (unsigned n = 0; n < 150; ++n) {
        if (state.stop.load(std::memory_order_acquire)) return nullptr;
        usleep(100000);
    }
    if (state.stop.load(std::memory_order_acquire)) return nullptr;
    Log emergency;
    emergency.attach(*state.paths);
    emergency.write("[%s-watchdog] FAIL 15-second deadline; runtime=FAIL; requesting system exit",state.phase);
    notify_user("Yakumo: runtime timeout. Preserve probe.log/klog; close from home screen if needed.");
    catchReturnFromMain(5);
}
}
RuntimeProbeResult protected_runtime_probe(const TitlePaths& paths, Log& log) {
    Watchdog state{&paths};
    pthread_t thread{};
    const int create = pthread_create(&thread, nullptr, watch, &state);
    if (create != 0) {
        log.write("[runtime-watchdog] FAIL pthread_create=%d runtime=FAIL", create);
        return {};
    }
    RuntimeProbeResult result{};
    try { result = runtime_probe(paths, log); }
    catch (const std::exception& error) { log.write("[runtime] FAIL boundary exception=%s", error.what()); }
    catch (...) { log.write("[runtime] FAIL boundary unknown exception"); }
    state.stop.store(true, std::memory_order_release);
    const int joined = pthread_join(thread, nullptr);
    if (joined != 0) {
        log.write("[runtime-watchdog] FAIL pthread_join=%d runtime=FAIL", joined);
        // The worker may still reference state. Never return and invalidate it.
        catchReturnFromMain(5);
    }
    log.write("[runtime-watchdog] joined=0 deadline=15s cleanup=PASS");
    return result;
}
#if YAKUMO_WITH_PROFILE
ProfileResult protected_profile_probe(const TitlePaths& paths,Log& log){
    Watchdog state{&paths,"profile"};pthread_t thread{};
    const int created=pthread_create(&thread,nullptr,watch,&state);
    if(created!=0){log.write("[profile-watchdog] FAIL create=%d",created);return {};}
    ProfileResult result;
    try{result.portable=profile_probe(paths,log);result.sdk=profile_native_errors(log);}catch(const std::exception& e){log.write("[profile] FAIL boundary %s",e.what());}catch(...){log.write("[profile] FAIL unknown exception");}
    state.stop=true;const int joined=pthread_join(thread,nullptr);
    if(joined!=0){log.write("[profile-watchdog] FAIL join=%d",joined);catchReturnFromMain(5);}
    log.write("[profile-watchdog] joined=0 deadline=15s cleanup=PASS");return result;
}
#endif

}
