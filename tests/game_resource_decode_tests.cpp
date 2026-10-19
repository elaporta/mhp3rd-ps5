// SPDX-License-Identifier: GPL-3.0-or-later
// Optional private-asset integration check; no textures are committed.
#include "game_files.hpp"
#include "gpu/texture_pack.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
int main(int argc,char** argv){
    if(argc!=2)return 2;
    namespace fs=std::filesystem;
    const auto root=fs::absolute(argv[1]);yakumo::game_files::set_root(root);
    const auto folder=root/"yakumo/textures/NPJB40001";
    std::string error;
    auto pack=mhp3rd::gpu::TexturePack::open(folder,"NPJB40001",error);
    if(!pack)throw std::runtime_error(error);
    unsigned count=0;
    for(const auto& entry:fs::directory_iterator(folder/"resource-maps/david-vz")) {
        mhp3rd::gpu::Replacement image;image.name="resource-maps/david-vz/"+entry.path().filename().string();image.file=entry.path();
        pack->request(image);
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(image.state==mhp3rd::gpu::Replacement::State::Queued&&std::chrono::steady_clock::now()<until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(image.state!=mhp3rd::gpu::Replacement::State::Decoded||image.pixels.empty())throw std::runtime_error("Resource map decode failed: "+image.name);
        std::cout<<image.name<<" "<<image.width<<"x"<<image.height<<" bytes="<<image.pixels.size()<<'\n';++count;
    }
    if(count!=8)throw std::runtime_error("Expected all eight resource maps");
}
