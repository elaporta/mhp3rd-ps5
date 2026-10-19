// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include "game_performance.hpp"
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace psprecomp { using Error=std::runtime_error; }
namespace perf {
using Clock=std::chrono::steady_clock;
enum class Stall { Acquire, Present };
inline void add_wait_time(Clock::duration,Stall) {}
}
template<class T> T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
static unsigned acquires{}, submits{}, presents{}, waits{}, signals{}, index{};
static VkSemaphore waited{}, signalled{}, presented_wait{};
static VkResult acquire_rc=VK_SUCCESS, submit_rc=VK_SUCCESS, present_rc=VK_SUCCESS;
static std::uint64_t acquire_timeout{};
namespace yakumo::game_platform { void count_present() {} }
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice,VkSwapchainKHR,std::uint64_t t,VkSemaphore,VkFence,std::uint32_t* out) {
    ++acquires; acquire_timeout=t; *out=1; return acquire_rc;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue,std::uint32_t,const VkSubmitInfo* s,VkFence) {
    ++submits; waits=s->waitSemaphoreCount; signals=s->signalSemaphoreCount;
    if(waits) waited=*s->pWaitSemaphores;
    if(signals) signalled=*s->pSignalSemaphores;
    return submit_rc;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue,const VkPresentInfoKHR* p) {
    ++presents; index=*p->pImageIndices; presented_wait=*p->pWaitSemaphores; return present_rc;
}
}
struct Fixture {
    VkDevice device=handle<VkDevice>(1);
    VkQueue queue=handle<VkQueue>(2);
    VkSurfaceKHR surface=handle<VkSurfaceKHR>(3);
    VkSwapchainKHR swapchain=handle<VkSwapchainKHR>(4);
    std::optional<std::uint32_t> acquired_image;
    bool native_present_succeeded{},swapchain_dirty{},swapchain_check{},recording=true,overlay_visible=false;
    bool gpu_timer_used=true;
    struct Slot { bool gpu_timer_pending{}; };
    std::array<Slot,1> slots{};
    unsigned slot{}, rebuilds{}, blits{}, overlays{},segments{};
    std::vector<VkImage> swapchain_images{handle<VkImage>(10),handle<VkImage>(11)};
    std::vector<VkSemaphore> render_finished{handle<VkSemaphore>(20),handle<VkSemaphore>(21)};
    VkSemaphore acquire_semaphore(VkCommandBuffer) { return handle<VkSemaphore>(30); }
    void end_gpu_segment(VkCommandBuffer) { ++segments; }
    void recreate_swapchain() { ++rebuilds; swapchain_dirty=false; }
    void transition(VkCommandBuffer,VkImage,VkImageLayout,VkImageLayout) {}
    void record_game_blit(VkCommandBuffer,VkImage,VkImage) { ++blits; }
    void record_overlay(VkCommandBuffer,VkImage) { ++overlays; }
    unsigned frame_batch(VkCommandBuffer c,std::array<VkCommandBuffer,2>& batch) { batch[0]=c;return 1; }
    void submit_and_present(VkCommandBuffer commands,VkFence fence,VkImage source,bool game_frame,bool main_frame) {
#include "game_performance_present.inc"
    }
    void run(bool main=true) { submit_and_present(handle<VkCommandBuffer>(40),handle<VkFence>(41),handle<VkImage>(42),true,main); }
};
void check(bool pass) { if(!pass)throw std::runtime_error("presentation assertion failed"); }
void reset() {
    acquires=submits=presents=waits=signals=0;
    waited=signalled=presented_wait=VK_NULL_HANDLE;
    acquire_rc=submit_rc=present_rc=VK_SUCCESS;
    yakumo::game_performance::submitted=0;
}
template<class F> void fails(F f) { bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught); }
int main() {
    check(yakumo::game_performance::valid_cache_profile("hd-x4_cold1"));
    for(const auto& value:{"", "../other", "a/b", "A", "a.b"})check(!yakumo::game_performance::valid_cache_profile(value));
    check(!yakumo::game_performance::valid_cache_profile(std::string(33,'a')));
    yakumo::game_performance::require_upload_finished(VK_SUCCESS);
    for(auto rc:{VK_TIMEOUT,VK_ERROR_DEVICE_LOST}) fails([&]{yakumo::game_performance::require_upload_finished(rc);});
    reset(); Fixture prior;prior.acquired_image=0;prior.swapchain_dirty=true;prior.run(false);
    check(!acquires&&!prior.rebuilds&&submits==1&&presents==1&&index==0&&!prior.acquired_image);
    check(waits==1&&signals==1&&waited==handle<VkSemaphore>(30)&&signalled==handle<VkSemaphore>(20)&&presented_wait==signalled);
    check(prior.recording&&prior.segments==0&&prior.native_present_succeeded);
    reset();Fixture normal;normal.overlay_visible=true;normal.run();
    check(acquires==1&&acquire_timeout==5'000'000'000ull&&index==1&&normal.overlays==1);
    check(!normal.recording&&normal.slots[0].gpu_timer_pending&&normal.segments==1);
    for(auto rc:{VK_TIMEOUT,VK_NOT_READY,VK_ERROR_OUT_OF_DATE_KHR}) {
        reset();Fixture f;acquire_rc=rc;f.run();
        check(submits==1&&!presents&&!waits&&!signals&&!f.native_present_succeeded);
        check(f.swapchain_dirty==(rc==VK_ERROR_OUT_OF_DATE_KHR));
        if(f.swapchain_dirty) { acquire_rc=VK_SUCCESS;f.run();check(f.rebuilds==1&&presents==1); }
    }
    reset();Fixture sub;acquire_rc=VK_SUBOPTIMAL_KHR;sub.run();check(sub.swapchain_check&&sub.native_present_succeeded);
    reset();Fixture out;present_rc=VK_ERROR_OUT_OF_DATE_KHR;out.run();check(out.swapchain_dirty&&!out.native_present_succeeded&&!yakumo::game_performance::submitted);
    reset();Fixture ps;present_rc=VK_SUBOPTIMAL_KHR;ps.run();check(ps.swapchain_check&&ps.native_present_succeeded);
    reset();Fixture lost;acquire_rc=VK_ERROR_DEVICE_LOST;fails([&]{lost.run();});check(!submits&&!presents);
    reset();Fixture bad_submit;submit_rc=VK_ERROR_DEVICE_LOST;fails([&]{bad_submit.run();});check(!presents);
    reset();Fixture bad_present;present_rc=VK_ERROR_DEVICE_LOST;fails([&]{bad_present.run();});check(!bad_present.native_present_succeeded);
    reset();Fixture host;host.surface=VK_NULL_HANDLE;host.run();check(!acquires&&!presents&&submits==1&&!waits);
    std::cout<<"presentation acquisition/synchronization/recovery cases PASS\n";
}
