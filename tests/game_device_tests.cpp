// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the new native game backend at the real SDK ABI boundary.
#include "game_platform.hpp"
#include "integration_pad.hpp"
#include "game_pad.hpp"
#include "game_audio.hpp"
#include "text_input_service.hpp"
#include "settings/settings.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
namespace {
std::atomic<unsigned> outputs{};
std::atomic<bool> pcm_valid{true};
std::atomic<int> output_rc{256};
std::atomic<float> last_left{},last_right{};
int connected=1, info_rc=0;
std::uint32_t buttons=0x4000;
unsigned char left_trigger{},right_trigger{};
unsigned char left_x{255};
unsigned char right_x{128},right_y{128};
bool bad_guard=false;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
extern "C" {
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance,const char*) { return nullptr; }
int sceUserServiceInitialize(const void*){return 0;}
int sceUserServiceGetInitialUser(int* p){*p=42;return 0;}
int scePadInit(){return 0;}
int scePadOpen(int user,int type,int index,const void*){check(user==42&&type==0&&index==0,"pad open identity");return 7;}
int scePadReadState(int h,void* out){check(h==7,"pad read handle");std::memset(out,0,512);std::memcpy(out,&buttons,4);auto* p=static_cast<unsigned char*>(out);p[4]=left_x;p[5]=128;p[6]=right_x;p[7]=right_y;p[8]=left_trigger;p[9]=right_trigger;return 0;}
int scePadClose(int h){check(h==7,"pad close handle");return 0;}
int scePadGetControllerInformation(int h,void* out){check(h==7,"pad info handle");yakumo::ps5::IntegrationPadInformation i{};i.connected=connected;i.density=44;i.resolution_x=1920;i.resolution_y=942;std::memcpy(out,&i,sizeof(i));if(bad_guard)static_cast<unsigned char*>(out)[512]=0;return info_rc;}
int sceAudioOutInit(){return 0;}
int sceAudioOutOpen(int user,int type,int index,unsigned grain,unsigned rate,unsigned format){check(user==255&&type==0&&index==0&&grain==256&&rate==48000&&format==4,"audio ABI");return 9;}
int sceAudioOutOutput(int h,const void* data){
    if(h!=9||reinterpret_cast<std::uintptr_t>(data)%64)pcm_valid=false;
    const auto* p=static_cast<const float*>(data);
    for(unsigned i=0;i<256;++i)if(!std::isfinite(p[2*i])||!std::isfinite(p[2*i+1])||
        std::abs(p[2*i])>1||std::abs(p[2*i+1])>1||
        std::abs(p[2*i]-p[0])>1e-6||std::abs(p[2*i+1]+p[0])>1e-6)pcm_valid=false;
    last_left=p[0];last_right=p[1];
    ++outputs;std::this_thread::sleep_for(std::chrono::milliseconds(1));return output_rc;
}
int sceAudioOutClose(int h){check(h==9,"audio close handle");return 0;}
}
int main(){try{
    namespace platform=yakumo::game_platform;
    check(platform::initialize_devices(),"device initialization");
    auto& s=mhp3rd::settings::current();s.chord_window=0;
    {
        using namespace mhp3rd::input;
        auto custom=s.controls;
        custom.pad[static_cast<std::size_t>(Action::Triangle)]={single(pad(PadInput::RightTrigger)),single(pad(PadInput::North))};
        Combo combo;combo.buttons=0x300;combo.pad={chord(pad(PadInput::LeftTrigger),pad(PadInput::West))};custom.combos.push_back(combo);
        custom.pad[static_cast<std::size_t>(Action::Cross)]={single(pad(PadInput::Touchpad)),single(pad(PadInput::South))};
        custom.combos.push_back(Combo{});custom.combos.back().buttons=0x4000;
        custom.combos.back().pad={chord(pad(PadInput::Back),pad(PadInput::West))};
        const auto effective=yakumo::game_pad::native_layout(custom);
        check(effective.pad[static_cast<std::size_t>(Action::Triangle)][0]==single(pad(PadInput::North)),"custom trigger attack removed; other binding retained");
        check(effective.combos.back().pad[0].empty(),"trigger combo cannot leak a second action");
        check(effective.combos[effective.combos.size()-2].pad[0].empty(),"reserved trigger combo removed");
        check(effective.pad[static_cast<std::size_t>(Action::Cross)][0]==single(pad(PadInput::South)),"saved touchpad alias cannot leak a guest control");
        check(!custom.combos.back().pad[0].empty(),"saved source controls preserved");
    }
    {
        using namespace mhp3rd::input;
        const std::array<Action,5> assignments={Action::FastForward,Action::Select,Action::HideHud,
            Action::L,Action::R};
        auto extra=yakumo::game_pad::assigned_actions(assignments,{true,false,false,false,false});
        check(extra.state.fast_forward&&!extra.state.buttons,"L2 assignment emits fast-forward independently of guest buttons");
        extra=yakumo::game_pad::assigned_actions(assignments,{false,true,false,false,false});
        check(extra.state.buttons==1&&!extra.state.fast_forward,"PS5 assignment emits PSP Select");
        extra=yakumo::game_pad::assigned_actions(assignments,{false,false,true,false,false});
        check(extra.state.hide_hud&&!extra.state.fast_forward,"touchpad assignment toggles HUD action only");
        extra=yakumo::game_pad::assigned_actions(assignments,{false,false,false,true,true});
        check(extra.state.buttons==0x300,"shoulder assignments provide PSP L and R together");
    }
    auto p=platform::sample_pad();check((p.buttons&0x4000)&&p.analog_x==255,"actual Cross binding and stick");
    buttons=0x400;p=platform::sample_pad();check((p.buttons&0x100)!=0,"actual left shoulder binding");
    buttons=0x800;p=platform::sample_pad();check(p.buttons==0x200,"R1 preserves PSP R");
    buttons=0;right_trigger=63;p=platform::sample_pad();check(p.buttons==0,"R2 below configured 25 percent threshold");
    right_trigger=64;p=platform::sample_pad();check(p.buttons==1,"R2 crosses configured 25 percent threshold");
    s.trigger=.5f;right_trigger=127;p=platform::sample_pad();check(p.buttons==0,"R2 respects a changed threshold");
    s.trigger=.25f;
    s.trigger=1.f;right_trigger=254;p=platform::sample_pad();check(!p.buttons,"full-travel threshold is not reached early");
    right_trigger=255;p=platform::sample_pad();check(p.buttons==1,"100 percent threshold still activates at full travel");
    s.trigger=.25f;
    right_trigger=255;p=platform::sample_pad();check(p.buttons==1&&!p.fast_forward,"R2 is Select without R");
    right_trigger=0;left_trigger=255;p=platform::sample_pad();check(p.buttons==0&&p.fast_forward,"L2 is fast-forward without L");
    left_trigger=0;p=platform::sample_pad();check(!p.fast_forward&&p.buttons==0,"release stops fast-forward");
    {
        using namespace mhp3rd::input;
        const auto original=s.controls;
        s.controls.pad[static_cast<unsigned>(Action::TriangleCircle)]={single(pad(PadInput::LeftStick))};
        buttons=2;p=platform::sample_pad();check((p.buttons&0x3000)==0x3000,"a remap after the first sample is effective");
        buttons=0;s.controls=original;
        s.controls.pad[static_cast<unsigned>(Action::ItemLeft)]={single(pad(PadInput::LeftStick))};
        buttons=2;p=platform::sample_pad();
        check(p.native_input_ready && (p.native_lead_requested&0x8100)==0x8100,"mapped item scroll requests upstream L-before-item sequencing");
        buttons=0;s.controls=original;
        right_x=255;right_y=128;p=platform::sample_pad();check(p.right_x==255&&p.right_y==128,"native horizontal camera axis");
        s.invert_camera_x=s.invert_camera_y=true;
        p=platform::sample_pad();check(p.right_x==1&&p.right_y==128,"horizontal camera inversion");
        right_x=128;right_y=0;p=platform::sample_pad();check(p.right_x==128&&p.right_y==255,"vertical camera inversion");
        right_x=255;right_y=0;
        s.invert_camera_x=s.invert_camera_y=false;s.right_stick=mhp3rd::settings::RightStick::Off;
        p=platform::sample_pad();check(p.right_x==128&&p.right_y==128,"disabled camera is centered");
        s.right_stick=mhp3rd::settings::RightStick::DPad;s.right_stick_zone=.5f;
        p=platform::sample_pad();check((p.buttons&0x30)==0x30&&p.right_x==128,"right stick D-pad directions");
        right_x=180;right_y=128;p=platform::sample_pad();check(!p.buttons,"right stick below D-pad threshold");
        s.right_stick_zone=.1f;p=platform::sample_pad();check(p.buttons==0x20,"changed D-pad threshold takes effect");
        s.right_stick_zone=1.f;right_x=255;p=platform::sample_pad();check(p.buttons==0x20,"full-travel D-pad threshold remains reachable");
        s.right_stick=mhp3rd::settings::RightStick::Camera;s.right_stick_zone=.5f;right_x=right_y=128;
        s.controls.pad[static_cast<unsigned>(Action::CameraLeft)]={single(pad(PadInput::LeftStick))};
        buttons=2;p=platform::sample_pad();check(p.right_x==1,"mapped camera direction reaches the native camera");
        buttons=0;s.controls=original;
    }
    buttons=0x100000;p=platform::sample_pad();check(p.buttons==0&&p.hide_hud&&!p.fast_forward,"whole touchpad hides HUD without Select");
    p=platform::sample_pad();check(!p.hide_hud&&!p.buttons,"held touchpad cannot toggle repeatedly");
    buttons=0;p=platform::sample_pad();check(!p.hide_hud,"release does not toggle");
    buttons=0x100000;p=platform::sample_pad();check(p.hide_hud,"second press shows HUD");
    info_rc=-3;bool rejected=false;try{platform::sample_pad();}catch(const std::exception&){rejected=true;}check(rejected,"API error cannot be a disconnect");info_rc=0;
    bad_guard=true;rejected=false;try{platform::sample_pad();}catch(const std::exception&){rejected=true;}check(rejected,"ABI overwrite rejected");bad_guard=false;
    connected=0;left_trigger=255;p=platform::sample_pad();check(p.buttons==0&&p.analog_x==128&&!p.fast_forward,"disconnect neutralization including fast-forward");
    left_trigger=0;buttons=0;connected=1;std::this_thread::sleep_for(std::chrono::milliseconds(510));
    buttons=0x100000;p=platform::sample_pad();check(p.buttons==0&&!p.fast_forward&&!p.hide_hud,"held touchpad at reconnect requires release");
    buttons=0;p=platform::sample_pad();check(!p.hide_hud,"reconnect release arms the next press");
    buttons=0x100000;p=platform::sample_pad();check(p.hide_hud&&!p.buttons,"fresh touchpad after reconnect");
    {
        using namespace yakumo::text_input;
        struct Fake:Backend{
            bool running{},done{};
            std::optional<Failure> open(const Request&)override{running=true;return {};}
            std::optional<Result> poll()override{if(!done)return {};running=false;return Result{0,Outcome::Confirmed,"Test",{}};}
            void cancel()override{done=true;}
            bool active()const override{return running;}
        } backend;
        auto& input=service();check(input.set_backend(backend),"device fixture backend");
        mhp3rd::fast_forward::Switch fast;
        const mhp3rd::fast_forward::Guards guards{true,false,false};
        check(fast.update(true,mhp3rd::fast_forward::Mode::Toggle,guards),"toggle acceleration starts before keyboard");
        const auto id=input.submit(Request{}).id;
        check(!fast.update(false,mhp3rd::fast_forward::Mode::Toggle,
            yakumo::game_pad::fast_forward_guards(guards,input.blocks_input())),"latched acceleration pauses during keyboard capture");
        buttons=0x104000;left_trigger=255;p=platform::sample_pad();
        check(!p.buttons&&!p.fast_forward&&!p.hide_hud&&p.analog_x==128,"keyboard captures buttons, HUD toggle, L2 and stick");
        backend.done=true;input.tick();check(input.take(id)->text=="Test","device fixture confirmation");
        p=platform::sample_pad();check(!p.buttons&&!p.fast_forward,"closing confirm cannot leak");
        check(!fast.update(false,mhp3rd::fast_forward::Mode::Toggle,
            yakumo::game_pad::fast_forward_guards(guards,input.blocks_input())),"latched acceleration waits for physical release");
        buttons=0x80000000;left_trigger=0;left_x=128;p=platform::sample_pad();check(!p.buttons&&!input.blocks_input(),"physical release restores control; unmapped SDK status bit is not a held game button");
        check(fast.update(false,mhp3rd::fast_forward::Mode::Toggle,
            yakumo::game_pad::fast_forward_guards(guards,input.blocks_input())),"toggle resumes after capture and release like the upstream menu");
        buttons=0x4000;p=platform::sample_pad();check(p.buttons&0x4000,"fresh press after keyboard works");
        buttons=0x100000;p=platform::sample_pad();check(p.hide_hud&&!p.buttons,"fresh touchpad after keyboard works");
        // The injected backend must outlive any later singleton access.
        static Fake idle;check(input.set_backend(idle),"restore stable idle backend");
    }
    platform::audio_gain(1);
    check(platform::audio_start([](std::int16_t* out,unsigned n){for(unsigned i=0;i<n;++i){out[i*2]=8192;out[i*2+1]=-8192;}}),"PCM device opens");
    for(unsigned i=0;i<100&&outputs<3;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(outputs>=3&&pcm_valid&&platform::audio_running(),"aligned rational PCM and positive SDK returns");
    const auto await_audio=[&](float amplitude){
        for(unsigned i=0;i<1000;++i){
            if(std::abs(last_left.load()-amplitude)<1e-6&&std::abs(last_right.load()+amplitude)<1e-6)return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Native audio output did not reach requested volume/pause");
    };
    await_audio(.25f); // 8192 / 32768, no hidden 0.06 attenuation.
    platform::audio_gain(.5f);await_audio(.125f);
    platform::audio_gain(0);await_audio(0);
    platform::audio_gain(1);await_audio(.25f);
    platform::audio_pause(true);await_audio(0);
    platform::audio_pause(false);await_audio(.25f);
    platform::audio_gain(NAN);await_audio(0);
    platform::audio_gain(1);await_audio(.25f);
    const auto live=yakumo::game_audio::telemetry.snapshot();
    check(live.valid&&live.output_blocks>0&&live.levels.peak==.25&&live.levels.outside_range==0&&live.last_result==256,"output levels available before shutdown");
    output_rc=-17;
    for(unsigned i=0;i<100&&platform::audio_error()==0;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(platform::audio_error()==-17&&!platform::audio_running(),"negative audio error is sticky");
    platform::shutdown_devices();
    std::cout<<"game devices: original bindings, neutralization, ABI rejection, PCM and sticky errors PASS\n";return 0;
}catch(const std::exception& e){yakumo::game_platform::shutdown_devices();std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
