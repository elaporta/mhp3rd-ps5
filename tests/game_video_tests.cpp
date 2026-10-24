// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_files.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "gpu/texture_pack.hpp"
#include "game_performance.hpp"
#include "game_audio.hpp"
#include "audio/audio_sink.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <source_location>
#include <thread>
#include <unistd.h>
#ifndef YAKUMO_GAME_WEB
#define YAKUMO_GAME_WEB 0
#endif
void check(bool ok,const std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("video settings assertion failed line="+std::to_string(at.line()));
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const std::string mode=argv[1];
    const auto root=std::filesystem::temp_directory_path()/("yakumo-video-test-"+std::to_string(getpid()));
    std::filesystem::create_directories(root/"yakumo/config");
    yakumo::game_files::set_root(root);
    mhp3rd::install::set_data_directory_override(root/"yakumo/config");
    const auto file=root/"yakumo/config/settings.ini";
    const std::string content=mode=="existing"?"settings.version=2\nvideo.internal_scale=4\nvideo.texture_pack=1\nvideo.frame_rate=30\nvideo.frame_rate_auto=0\nvideo.performance=off\ncustom.keep=sentinel\n":
        mode=="flat-x4"?"settings.version=2\nvideo.internal_scale = 4\nvideo.frame_rate=60\nvideo.frame_rate_auto=1\nvideo.texture_pack=0\nvideo.performance=log\ncustom.keep=sentinel\n":
        mode=="sectioned"?"settings.version=2\n[video]\ninternal_scale = 2\ntexture_pack=0\nframe_rate=30\n":
        mode=="invalid"?"settings.version=2\nvideo.internal_scale=bad\nvideo.frame_rate=999\n":"";
    if(!content.empty())std::ofstream(file)<<content;
    if(mode=="environment")setenv("MHP3RD_FRAME_RATE","45",1);
    const auto& s=mhp3rd::settings::current();
    if(mode=="existing") {
        check(s.internal_scale==4&&s.texture_pack&&s.frame_rate==mhp3rd::settings::FrameRate::Fps30&&!s.frame_rate_auto&&s.perf==mhp3rd::settings::PerfDisplay::Off);
        std::ifstream in(file);const std::string after((std::istreambuf_iterator<char>(in)),{});check(after==content);
    }else if(mode=="flat-x4") {
        check(s.internal_scale==4&&s.frame_rate==mhp3rd::settings::FrameRate::Fps60&&s.frame_rate_auto&&!s.texture_pack&&s.perf==mhp3rd::settings::PerfDisplay::Log);
        std::ifstream in(file);const std::string after((std::istreambuf_iterator<char>(in)),{});check(after==content);
    }else {
        check(s.internal_scale==6&&s.texture_pack&&s.frame_rate_auto&&s.crisp_text&&s.ui_textures==mhp3rd::settings::UiTextures::Off);
        check(s.frame_rate==(mode=="environment"?mhp3rd::settings::FrameRate::Fps45:mhp3rd::settings::FrameRate::Fps60));
    }
    check(mhp3rd::gpu::VulkanRenderer::textures_root()==root/"yakumo/textures");
    check(yakumo::game_performance::capped_rate(120,59.94)==59.94);
    check(yakumo::game_performance::capped_rate(120,119.88)==119.88);
    check(yakumo::game_performance::capped_rate(120,144)==120);
    check(yakumo::game_performance::capped_rate(120,0)==60);
    check(yakumo::game_performance::capped_rate(60,119.88)==60);
    check(yakumo::game_performance::capped_rate(45,60)==45);
    check(yakumo::game_performance::capped_rate(60,0)==60);
    check(yakumo::game_performance::capped_rate(120,std::numeric_limits<double>::quiet_NaN())==60);
    check(yakumo::game_performance::capped_rate(120,std::numeric_limits<double>::infinity())==60);
    check(yakumo::game_performance::capped_rate(-1,119.88)==30);
    if(mode=="audio-config") {
        using mhp3rd::settings::from_entries;
        const auto half=from_entries({{"audio.volume","50"},{"audio.mute","0"}});
        const auto maximum=from_entries({{"audio.volume","100"},{"audio.mute","0"}});
        const auto unsupported=from_entries({{"audio.volume","150"}});
        const auto muted=from_entries({{"audio.volume","100"},{"audio.mute","1"}});
        check(half.volume==50&&!half.mute&&maximum.volume==100&&unsupported.volume==100&&muted.mute);
        auto& player=mhp3rd::settings::current();
        auto& sink=mhp3rd::audio::AudioSink::instance();
        player.volume=half.volume;player.mute=half.mute;sink.refresh_settings();
        check(sink.output_gain()==.5f);
        check(std::abs(yakumo::game_audio::sample(32767,sink.output_gain(),false)-.499985f)<.000002f);
        player.volume=maximum.volume;sink.refresh_settings();check(sink.output_gain()==1.f);
        check(yakumo::game_audio::sample(32767,sink.output_gain(),false)==32767.f/32768.f);
        player.mute=muted.mute;sink.refresh_settings();check(sink.output_gain()==0.f);
    }
    if(mode=="decoding") {
        using namespace mhp3rd::gpu;
        const auto folder=root/"yakumo/textures/NPJB40001";
        std::filesystem::create_directories(folder);
        std::ofstream(folder/"textures.ini")<<"[options]\nhash=xxh64\n[hashes]\n000000000000000100000000=broken.png\n";
        std::ofstream(folder/"broken.png")<<"not a PNG";
        std::ofstream(folder/"runtime-case-index.txt")<<"broken.png\nmixed.png\n../config/settings.ini\n";
        const unsigned char png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,248,255,255,255,127,0,9,251,3,253,42,134,227,138,0,0,0,0,73,69,78,68,174,66,96,130};
        std::ofstream(folder/"mixed.png",std::ios::binary).write(reinterpret_cast<const char*>(png),sizeof(png));
        std::string error;auto pack=TexturePack::open(folder,"NPJB40001",error);
        if(!pack)throw std::runtime_error("Texture pack fixture open: "+error);
        std::array<Replacement,20> images;
        for(std::size_t i=0;i<images.size();++i) {
            images[i].name=i%2?"missing.png":"broken.png";
            images[i].file=folder/images[i].name;
            pack->request(images[i]);
        }
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<until) {
            bool done=true;for(auto& image:images)done&=image.state==Replacement::State::Failed;
            if(done)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for(auto& image:images)check(image.state==Replacement::State::Failed);
        check(yakumo::game_performance::failed==20);
        Replacement mixed;mixed.name="MiXeD.PNG";mixed.file=folder/mixed.name;
        pack->request(mixed);
        const auto decode_until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(mixed.state!=Replacement::State::Decoded&&std::chrono::steady_clock::now()<decode_until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(mixed.state==Replacement::State::Decoded&&mixed.width==1&&mixed.height==1&&mixed.pixels.size()==4);
        check(mixed.pixels[0]==255&&mixed.pixels[3]==255);
        pack.reset();
    }
    std::filesystem::remove_all(root);
    std::cout<<"native video settings "<<mode<<" PASS\n";
}
