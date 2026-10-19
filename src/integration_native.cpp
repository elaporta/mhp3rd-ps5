// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.hpp"
#include "integration_pad.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <exception>
#include <pthread.h>
#include <unistd.h>
#include <ctime>

extern "C" {
int scePadOpen(int,int,int,const void*);
int scePadReadState(int,void*);
int scePadClose(int);
int scePadGetControllerInformation(int,void*);
int sceAudioOutOpen(int,int,int,unsigned,unsigned,unsigned);
int sceAudioOutOutput(int,const void*);
int sceAudioOutClose(int);
void ps5_thread_stacks(unsigned*,unsigned*);
[[noreturn]] void catchReturnFromMain(int);
}
namespace yakumo::ps5 {
namespace {
std::uint64_t mono_us(){timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t)!=0)return 0;return std::uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;}
struct Watch {
    std::atomic<const TitlePaths*> paths{nullptr};std::atomic<bool> stop{false};
    std::atomic<IntegrationSession*> session{nullptr};
    // A lock protects lifetime while the watchdog reads session atomics.
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    std::uint64_t began{};
    pthread_t thread{};
};
Watch watch;
void* watchdog(void*) {
    for(;;){
        if(watch.stop.load())return nullptr;
        const auto now=mono_us();const char* reason=nullptr;
        if(now==0||now<watch.began||now-watch.began>240000000)reason="240-second boot deadline or monotonic failure";
        pthread_mutex_lock(&watch.mutex);
        if(auto* session=watch.session.load())for(unsigned worker=0;worker<4;++worker){
            const auto beat=session->heartbeat(worker);
            if(session->active(worker) && beat && now>=beat && now-beat>15000000)reason="worker stalled for fifteen seconds (including teardown)";
        }
        pthread_mutex_unlock(&watch.mutex);
        if(reason){Log emergency;if(const auto* paths=watch.paths.load())emergency.attach(*paths);
            emergency.write("[integration-watchdog] FAIL %s integration=FAIL title_log_available=%u",reason,watch.paths.load()!=nullptr);
            notify_user("Yakumo integration timeout. Preserve probe.log; close from Home if needed.");catchReturnFromMain(5);}
        usleep(100000);
    }
}
}
bool integration_boot_watch_start(const TitlePaths& paths,Log& log)noexcept {
    (void)paths;watch.paths=nullptr;watch.began=mono_us();watch.stop=false;
    const int rc=pthread_create(&watch.thread,nullptr,watchdog,nullptr);
    log.write("[integration-watchdog] create=%d total_deadline=240s worker_no_progress=15s",rc);
    return rc==0&&watch.began!=0;
}
void integration_boot_watch_ready(const TitlePaths& paths)noexcept {watch.paths.store(&paths);}
void integration_boot_watch_stop(Log& log)noexcept {
    watch.stop=true;const int rc=pthread_join(watch.thread,nullptr);
    log.write("[integration-watchdog] joined=%d",rc);if(rc!=0)catchReturnFromMain(5);
}
bool integration_probe(const TitlePaths& paths,Log& log)noexcept {
    bool pass=true;
    try{
        for(unsigned cycle=1;cycle<=2;++cycle){
            unsigned before=0,cached_before=0;ps5_thread_stacks(&before,&cached_before);
            auto devices=make_integration_devices();
            IntegrationSession session(paths,log,*devices,cycle);
            pthread_mutex_lock(&watch.mutex);watch.session=&session;pthread_mutex_unlock(&watch.mutex);
            struct Unwatch {~Unwatch(){pthread_mutex_lock(&watch.mutex);watch.session=nullptr;pthread_mutex_unlock(&watch.mutex);}} unwatch;
            log.write("[integration] BEGIN cycle=%u duration=60s simulation=30Hz modules=32 slots=2 checkpoints=6 pad_info_layout=PS4-reference-PS5-unverified",cycle);
            session.start();
            if(cycle==2)notify_user("Yakumo cycle 2: turn OFF the controller now, wait at least 5 seconds, turn it ON and press Cross within 30 seconds.");
            const IntegrationGraphicsHooks hooks{&session,
                [](void* s,IntegrationSnapshot& snap){return static_cast<IntegrationSession*>(s)->frame(snap);},
                [](void* s,bool ok){static_cast<IntegrationSession*>(s)->rendered(ok);}};
            const bool graphics=integration_graphics(paths,log,hooks);
            const auto result=session.finish(graphics);
            pthread_mutex_lock(&watch.mutex);watch.session=nullptr;pthread_mutex_unlock(&watch.mutex);
            unsigned after=0,cached_after=0;ps5_thread_stacks(&after,&cached_after);
            const bool stacks=before==after;
            log.write("[integration] cycle=%u stacks_live=%u->%u stacks_cached=%u->%u cleanup=%s (SDK pool, not RSS)",cycle,before,after,cached_before,cached_after,stacks?"PASS":"FAIL");
            pass=pass&&result.passed()&&stacks;
            if(!pass)break;
        }
    }catch(const std::exception& e){log.write("[integration] FAIL boundary exception=%s",e.what());pass=false;}
    catch(...){log.write("[integration] FAIL unknown boundary exception");pass=false;}
    // session pointer is cleared even on failure; never leave a dangling watch target.
    pthread_mutex_lock(&watch.mutex);watch.session=nullptr;pthread_mutex_unlock(&watch.mutex);
    return pass;
}
}
