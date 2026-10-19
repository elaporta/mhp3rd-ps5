// SPDX-License-Identifier: GPL-3.0-or-later
// Actual projected Vulkan commands on controlled offscreen guest images.
#include "game_files.hpp"
#include "game_text_background.hpp"
#include "game_text_observer.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

int main(){try{
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("offscreen keyboard recovery");};
    const auto root=std::filesystem::temp_directory_path()/("yakumo-text-gpu-"+std::to_string(getpid()));
    std::filesystem::create_directories(root/"yakumo/config");
    yakumo::game_files::set_root(root);mhp3rd::install::set_data_directory_override(root/"yakumo/config");
    auto& settings=mhp3rd::settings::current();settings.internal_scale=1;settings.texture_pack=false;
    settings.frame_rate=mhp3rd::settings::FrameRate::Fps30;
    mhp3rd::gpu::VulkanRenderer renderer;std::string error;
    if(!renderer.initialize({},error))throw std::runtime_error(error);
    constexpr unsigned address=0x04000000,width=480,height=272;
    std::vector<std::uint8_t> live(width*height*4),pixels;unsigned w{},h{};
    const auto paint=[&](unsigned red,unsigned green){
        for(unsigned i=0;i<width*height;++i){live[4*i]=red;live[4*i+1]=green;live[4*i+2]=0;live[4*i+3]=255;}
        renderer.upload_frame(address,live.data(),width,height,width);renderer.present(address);
    };
    paint(200,0);renderer.hold_frame(true);yakumo::game_text_observer::background_requested=true;
    paint(0,0);
    check(renderer.read_frame(pixels,w,h)&&w==width&&h==height&&pixels[0]==200);
    check(renderer.read_frame(pixels,w,h,true)&&w==8&&h==8&&pixels.size()==256);
    check(yakumo::game_text_background::measure(pixels,w,h)->nonblack==0);
    yakumo::game_text_observer::background_requested=false;paint(0,0);paint(0,0);
    std::this_thread::sleep_for(std::chrono::milliseconds(110));renderer.recover_text_background();
    check(renderer.read_frame(pixels,w,h)&&pixels[0]==200); // Black live pixels must not release the red hold.
    yakumo::game_text_observer::background_requested=true;paint(0,0); // Immediate reopen retains red.
    check(renderer.read_frame(pixels,w,h)&&pixels[0]==200);
    paint(0,180);yakumo::game_text_observer::background_requested=false;paint(0,180);paint(0,180);
    std::this_thread::sleep_for(std::chrono::milliseconds(110));renderer.recover_text_background();
    check(renderer.read_frame(pixels,w,h)&&pixels[0]==0&&pixels[1]==180);
    renderer.shutdown();std::filesystem::remove_all(root);
    std::cout<<"actual offscreen GPU: 256-byte live probe, black rejection, retained reopen and green recovery PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
