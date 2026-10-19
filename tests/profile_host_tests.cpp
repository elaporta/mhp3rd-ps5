// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_probe.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
using namespace yakumo::ps5;
int main(){char temp[]="/tmp/yakumo-profile-XXXXXX";const char* root=mkdtemp(temp);if(!root)return 1;
    int status=0;try{TitlePaths paths;std::snprintf(paths.cache,sizeof(paths.cache),"%s",root);Log log;
        if(!profile_probe(paths,log))throw std::runtime_error("real profile suite failed");
        for(auto fault:{ProfileFault::Hle,ProfileFault::Texture,ProfileFault::Geometry,ProfileFault::Crypto,ProfileFault::Files,ProfileFault::Cleanup,ProfileFault::Omit})
            if(profile_probe(paths,log,fault))throw std::runtime_error("injected profile failure reached PASS");
        ge::Scene scene;std::array<std::uint32_t,4> texels;if(!profile_graphics_scene(scene,texels,log)||scene.draw_count!=11||scene.vertex_count!=66)throw std::runtime_error("real profile graphics bridge");
        const auto directory=std::filesystem::path(root)/"profile-probe";
        if(!std::filesystem::is_empty(directory))throw std::runtime_error("owned files leaked");
        {std::ofstream keep(directory/"keep.txt");keep<<"unrelated";}
        if(!profile_probe(paths,log)||std::filesystem::file_size(directory/"keep.txt")!=9)throw std::runtime_error("unrelated files changed");
        std::filesystem::remove(directory/"keep.txt");std::filesystem::remove(directory);std::filesystem::create_directory_symlink(root,directory);
        if(profile_probe(paths,log))throw std::runtime_error("nofollow directory bypass");
        std::puts("[host-profile] PASS real HLE/GE/textures/AES/CMAC/SFO/ISO/UTF8, owned checkpoints, corruption/failure/omission and symlink rejection");
    }catch(const std::exception& e){std::fprintf(stderr,"[host-profile] FAIL %s\n",e.what());status=1;}
    std::filesystem::remove_all(root);return status;
}
