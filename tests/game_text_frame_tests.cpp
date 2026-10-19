// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the projected GPU snapshot/hold implementation over controlled image
// contents. Real PS5 image layout/queue/compositor behavior remains a device gate.
#include <vulkan/vulkan.h>
#include "game_text_background.hpp"
#include <vector>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace yakumo::game_platform {
std::uint64_t test_now{};
std::uint64_t milliseconds(){return test_now;}
}
template<class T> T handle(std::uintptr_t value){return reinterpret_cast<T>(value);}
void check(bool ok){if(!ok)throw std::runtime_error("text background regression");}
using Pixels=std::array<unsigned,2>;
std::map<VkImage,Pixels> images;
unsigned copies{};
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,unsigned count,const VkImageCopy* copy){
    check(sl==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&dl==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&count==1);
    check(copy->extent.width==2&&copy->extent.height==1&&copy->extent.depth==1);
    images[dst]=images.at(src);++copies;
}
struct VulkanRenderer {
    struct Impl {
        struct Target {VkImage color{};VkDeviceMemory color_memory{};VkImageView color_view{};bool initialized{};};
        bool ready=true,holding=false,recording=true,newer_picture_valid=false,allocation_fails=false,text_hold_attempted=false;
        unsigned newer_picture{},presented_target=1,allocations{},ended{},one_time{},destroyed{};
        std::uint64_t frames{},text_capture_frame{};
        std::map<unsigned,Target> targets;
        std::array<Target,2> pictures{};
        Target held{};
        yakumo::game_text_background::Recovery text_recovery;
        VkExtent2D target_extent{2,1};
        VkCommandBuffer command_buffer=handle<VkCommandBuffer>(1);
        bool create_image(unsigned,unsigned,VkFormat,VkImageUsageFlags,VkImage& image,VkDeviceMemory&,VkImageView&,VkImageAspectFlags,std::string& error){
            ++allocations;if(allocation_fails){error="controlled failure";return false;}
            image=handle<VkImage>(3);images[image]={};return true;
        }
        void destroy_target(Target&){++destroyed;}
        void transition(VkCommandBuffer,VkImage,VkImageLayout,VkImageLayout){}
        void end_pass(){++ended;}
        void run_commands(const std::function<void(VkCommandBuffer)>& f){++one_time;f(command_buffer);}
        void cache_text_background();
    };
    Impl* impl_{};
    void hold_frame(bool);
    void recover_text_background();
    bool probe_fails{};
    bool read_frame(std::vector<std::uint8_t>& pixels,std::uint32_t& width,std::uint32_t& height,bool live){
        check(live);if(probe_fails)return false;
        width=height=8;pixels.assign(256,0);
        const auto pixel=images.at(impl_->targets.at(impl_->presented_target).color)[0];
        pixels[0]=pixel&255;pixels[1]=(pixel>>8)&255;pixels[2]=(pixel>>16)&255;
        return true;
    }
};
#include "game_text_frame.inc"
int main(){
    VulkanRenderer renderer;renderer.hold_frame(true); // No renderer yet.
    VulkanRenderer::Impl impl;renderer.impl_=&impl;
    impl.ready=false;renderer.hold_frame(true);impl.cache_text_background();check(!copies);impl.ready=true;
    renderer.hold_frame(true);check(!impl.holding&&!copies); // No image yet.
    renderer.hold_frame(false);
    const auto live=handle<VkImage>(1),completed=handle<VkImage>(2);
    images[live]={0x112233,0x445566};impl.targets[1].color=live;
    impl.cache_text_background();check(!copies);impl.targets[1].initialized=true;
    impl.allocation_fails=true;renderer.hold_frame(true);check(!impl.holding&&impl.destroyed==1&&!copies);
    for(unsigned i=0;i<120;++i)renderer.hold_frame(true);
    check(impl.allocations==1);renderer.hold_frame(false); // Failed hold never becomes an allocation/log loop.
    impl.allocation_fails=false;impl.cache_text_background();check(copies==1&&impl.ended==1&&impl.one_time==0);
    const auto visible=images[live];images[live]={0,0}; // Guest clears before OskInitStart.
    renderer.hold_frame(true);check(impl.holding&&images[impl.held.color]==visible&&copies==1);
    renderer.hold_frame(true);impl.cache_text_background();check(copies==1); // Keyboard buttons cannot overwrite it.
    renderer.hold_frame(false);check(impl.holding&&impl.text_recovery.active());
    renderer.hold_frame(false); // Repeated flips cannot restart the deadline.
    impl.frames+=2;yakumo::game_platform::test_now=100;
    renderer.recover_text_background();check(impl.holding); // Live frame is black, retained frame is bright.
    impl.frames=200;renderer.hold_frame(true);impl.cache_text_background();
    check(impl.holding&&!impl.text_recovery.active()&&images[impl.held.color]==visible&&copies==1);
    // Immediate reopen reuses even an old snapshot instead of capturing black.
    renderer.hold_frame(false);impl.frames+=2;yakumo::game_platform::test_now=200;
    renderer.probe_fails=true;renderer.recover_text_background();check(impl.holding);
    renderer.probe_fails=false;images[live]={0x123456,0xabcdef};
    yakumo::game_platform::test_now=300;renderer.recover_text_background();check(!impl.holding&&!impl.text_recovery.active());
    images[completed]={0xabcdef,0x123456};impl.pictures[0].color=completed;impl.newer_picture_valid=true;
    impl.recording=false;impl.cache_text_background();check(copies==2&&impl.one_time==1&&images[impl.held.color]==images[completed]);
    renderer.hold_frame(true);check(impl.holding&&images[impl.held.color]==images[completed]);
    renderer.hold_frame(false);impl.frames+=2;yakumo::game_platform::test_now=400;
    renderer.recover_text_background();check(!impl.holding&&impl.allocations==2&&impl.destroyed==1);
    images[completed]={0xaaaaff,0xbbbbff};impl.frames=impl.text_capture_frame+120;
    renderer.hold_frame(true);check(copies==2);
    renderer.hold_frame(false);impl.frames+=2;yakumo::game_platform::test_now=500;
    renderer.recover_text_background();check(!impl.holding);
    renderer.hold_frame(true);check(impl.holding&&copies==3&&images[impl.held.color]==images[completed]);
    renderer.hold_frame(false);images[live]={0,0};impl.frames+=2;
    yakumo::game_platform::test_now=1999;renderer.recover_text_background();check(impl.holding);
    renderer.hold_frame(false);yakumo::game_platform::test_now=2000;
    renderer.recover_text_background();check(!impl.holding&&!impl.text_recovery.active()); // Real black scenes have a finite hold.
    renderer.recover_text_background();renderer.impl_=nullptr;renderer.recover_text_background();
    std::cout<<"projected snapshot: black close, immediate reopen reuse, live-only recovery, timeout, allocation/error paths PASS\n";
}
