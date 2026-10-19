// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_files.hpp"
#include "game_pad.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#ifndef YAKUMO_GAME_WEB
#define YAKUMO_GAME_WEB 0
#endif

int main(int argc,char** argv){try{
    namespace settings=mhp3rd::settings;
    const auto root=std::filesystem::temp_directory_path()/("yakumo-defaults-"+std::to_string(getpid()));
    std::filesystem::create_directories(root/"yakumo/config");
    yakumo::game_files::set_root(root);
    mhp3rd::install::set_data_directory_override(root/"yakumo/config");
    const auto check=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    auto& current=settings::current();
    const auto clean=settings::to_entries(current);
    check(current.internal_scale==6&&current.texture_pack&&current.frame_rate==settings::FrameRate::Fps60&&current.frame_rate_auto,"clean video defaults");
    check(current.fast_forward_speed==4&&current.fast_forward==mhp3rd::fast_forward::Mode::Hold&&!current.touch_controls,"clean native controls defaults");
    check(current.volume==100&&!current.mute&&current.perf==settings::PerfDisplay::Off,"clean audio and diagnostics are quiet");
    check(current.font==(root/"yakumo/assets/fonts/NotoSansCJKjp-Regular.otf").string(),"title-local default font");
    check(settings::to_entries(settings::defaults())==clean,"first load uses compiled defaults");
    settings::save();
    const auto saved=mhp3rd::install::read_settings_file(root/"yakumo/config");
    check(saved.at("video.fast_forward_speed")=="4", "save writes native fast-forward default");
    check(settings::to_entries(settings::from_entries(saved))==clean,"save and reload preserve defaults and preset");
    std::filesystem::remove(root/"yakumo/config/settings.ini");
    check(settings::to_entries(settings::from_entries({}))==clean,"deleted settings returns identical defaults");
    const auto existing=settings::from_entries({{"settings.version","2"},{"audio.volume","50"},{"video.internal_scale","2"},{"video.texture_pack","0"}});
    check(existing.volume==50&&existing.internal_scale==2&&!existing.texture_pack&&existing.fast_forward_speed==4,"explicit preferences survive; missing values use defaults");
    if(argc==2){
        std::ifstream input(argv[1]);check(bool(input),"reference settings available");
        settings::Entries reference;std::string line;
        while(std::getline(input,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            const auto split=line.find('=');
            if(split!=std::string::npos&&!line.starts_with('#'))reference[line.substr(0,split)]=line.substr(split+1);
        }
        // Identity, install paths and installer/UI history are not player defaults.
        for(const char* key:{"network.mac","text.font","video.keep_aspect","settings.version","video.internal_scale"})reference.erase(key);
        reference["video.fast_forward_speed"]="4";
        auto expected=settings::from_entries(reference);
        expected.font=current.font;
        const auto normalized=settings::to_entries(expected);
        unsigned compared{};
        for(const auto& [key,value]:normalized){
            if(reference.count(key)==0)continue;
            if(clean.at(key)!=value)throw std::runtime_error("reference differs: "+key+" clean="+clean.at(key)+" expected="+value);
            ++compared;
        }
        check(compared>130,"complete supplied player preference comparison");
        std::cout<<"Compared "<<compared<<" supplied preferences; private identity excluded\n";
    }
    std::filesystem::remove_all(root);
    std::cout<<"clean/default/save/reload/delete/reference contracts PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
