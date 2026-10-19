// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_files.hpp"
#include "game_platform.hpp"
#include "text_input_service.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
namespace fs=std::filesystem;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){
    char temp[]="/tmp/yakumo-host-pad-XXXXXX";const auto* made=mkdtemp(temp);if(!made)return 1;
    fs::path root=made;
    try{
        fs::create_directories(root/"yakumo/config");yakumo::game_files::set_root(root);
        setenv("YAKUMO_HOST_PAD","1",1);
        auto sample=[](){return yakumo::game_platform::sample_pad();};
        check(sample().buttons==0&&sample().analog_x==128,"absent host source stays neutral");
        const auto name=root/"yakumo/config/host-pad.txt";
        {std::ofstream(name)<<"8\n";}
        check(sample().buttons==8,"host Start reaches the ordinary guest pad");
        {
            using namespace yakumo::text_input;
            struct Fake:Backend {
                bool running{};
                std::optional<Failure> open(const Request&)override{running=true;return {};}
                std::optional<Result> poll()override{running=false;return Result{0,Outcome::Cancelled,{},{}};}
                void cancel()override{}
                bool active()const override{return running;}
            };
            static Fake backend;
            auto& input=service();check(input.set_backend(backend),"renderer gate fixture backend");
            mhp3rd::gpu::VulkanRenderer renderer;renderer.sample_pad();
            check(renderer.pad().buttons==8,"renderer has an actual cached guest action");
            const auto id=input.submit(Request{}).id;
            check(renderer.pad().buttons==0,"opening keyboard neutralizes cached pad before next sample");
            input.tick();check(input.take(id)->outcome==Outcome::Cancelled,"renderer fixture closes");
            check(renderer.pad().buttons==0,"cached action remains blocked through drain");
            input.suppress_sample(true);
        }
        {std::ofstream(name)<<"40\n";}
        check(sample().buttons==0x40,"host Down reaches the ordinary guest pad");
        {std::ofstream(name)<<"10000\n";}
        bool rejected=false;try{sample();}catch(const std::exception&){rejected=true;}check(rejected,"invalid host mask rejects");
        fs::remove(name);fs::create_symlink(root/"yakumo/config/outside",name);
        rejected=false;try{sample();}catch(const std::exception&){rejected=true;}check(rejected,"host source symlink rejects");
        unsetenv("YAKUMO_HOST_PAD");check(sample().buttons==0,"host source is opt-in");
        fs::remove_all(root);std::cout<<"headless pad: neutral, actual guest masks, invalid data and containment PASS\n";return 0;
    }catch(const std::exception& e){fs::remove_all(root);std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
