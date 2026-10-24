// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_vk_dispatch.hpp"
#include "game_platform.hpp"
#include "game_audio.hpp"
#include "game_pad.hpp"
#include "game_observer.hpp"
#include "game_display.hpp"
#include "game_settings.hpp"
#include "game_performance.hpp"
#if YAKUMO_GAME_WEB
#include "game_web.hpp"
#endif
#include "text_input_service.hpp"
#include "integration_pad.hpp"
#include "input/bindings.hpp"
#include "settings/settings.hpp"
#include "gpu/frame_pacing.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#if !YAKUMO_NATIVE
#include "game_files.hpp"
#endif

#if YAKUMO_NATIVE
extern "C" {
int sceUserServiceInitialize(const void*);
int sceUserServiceGetInitialUser(int*);
int scePadInit();
int scePadOpen(int,int,int,const void*);
int scePadReadState(int,void*);
int scePadClose(int);
int scePadGetControllerInformation(int,void*);
int sceAudioOutInit();
int sceAudioOutOpen(int,int,int,unsigned,unsigned,unsigned);
int sceAudioOutOutput(int,const void*);
int sceAudioOutClose(int);
}
#endif
namespace yakumo::game_platform {
namespace {
std::atomic<std::uint64_t> presents{};
int width=480,height=272;
float hz=60;
std::atomic<bool> audio_live{}, stop_audio{}, paused_audio{};
std::atomic<int> audio_fault{};
std::atomic<float> gain{1.f};
std::thread audio_thread;
#if YAKUMO_NATIVE
int pad_handle=-1, user=-1, audio_handle=-1;
std::uint64_t next_pad_open{};
mhp3rd::input::Resolver resolver;
bool pad_connected{};
bool hide_hud_ready{};
#endif
}
std::uint64_t milliseconds(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void display_size(int& w,int& h){w=width;h=height;}
float display_hz(){return hz;}
void count_present(){++presents;}
std::uint64_t present_count(){return presents.load();}
std::vector<const char*> instance_extensions(){
    game_dispatch::load_global();
#if YAKUMO_NATIVE
    return {VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_DISPLAY_EXTENSION_NAME};
#else
    return {};
#endif
}
bool create_surface(VkInstance instance,VkSurfaceKHR& surface,std::string& error){
#if YAKUMO_NATIVE
    auto fail=[&](const char* op,VkResult rc){error=std::string(op)+" rc="+std::to_string(rc);return false;};
    std::uint32_t n=0;
    VkResult rc=vkEnumeratePhysicalDevices(instance,&n,nullptr);if(rc||!n)return fail("enumerate display GPU",rc);
    std::vector<VkPhysicalDevice> devices(n);rc=vkEnumeratePhysicalDevices(instance,&n,devices.data());if(rc)return fail("enumerate GPUs",rc);
    VkPhysicalDevice gpu=devices.front();
    for(auto candidate:devices){VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(candidate,&p);if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){gpu=candidate;break;}}
    rc=vkGetPhysicalDeviceDisplayPropertiesKHR(gpu,&n,nullptr);if(rc||!n)return fail("enumerate display",rc);
    std::vector<VkDisplayPropertiesKHR> displays(n);rc=vkGetPhysicalDeviceDisplayPropertiesKHR(gpu,&n,displays.data());if(rc)return fail("display properties",rc);
    const auto display=displays.front().display;
    rc=vkGetDisplayModePropertiesKHR(gpu,display,&n,nullptr);if(rc||!n)return fail("enumerate modes",rc);
    std::vector<VkDisplayModePropertiesKHR> modes(n);rc=vkGetDisplayModePropertiesKHR(gpu,display,&n,modes.data());if(rc)return fail("display modes",rc);
    modes.resize(n);
    const auto& output=game_settings::output_resolution();
    const unsigned requested_height=output=="2160p"?2160:output=="1440p"?1440:1080;
    const auto candidates=game_display::candidates(modes,requested_height);
    rc=vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu,&n,nullptr);if(rc||!n)return fail("enumerate planes",rc);
    std::vector<VkDisplayPlanePropertiesKHR> planes(n);rc=vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu,&n,planes.data());if(rc)return fail("planes",rc);
    std::uint32_t plane=UINT32_MAX;
    for(std::uint32_t i=0;i<planes.size();++i){std::uint32_t count=0;rc=vkGetDisplayPlaneSupportedDisplaysKHR(gpu,i,&count,nullptr);if(rc)return fail("plane displays",rc);std::vector<VkDisplayKHR> supported(count);rc=vkGetDisplayPlaneSupportedDisplaysKHR(gpu,i,&count,supported.data());if(rc)return fail("plane displays",rc);if(std::find(supported.begin(),supported.end(),display)!=supported.end()){plane=i;break;}}
    if(plane==UINT32_MAX){error="No compatible display plane";return false;}
    for(const auto mode:candidates) {
        VkDisplayPlaneCapabilitiesKHR caps{};rc=vkGetDisplayPlaneCapabilitiesKHR(gpu,modes[mode].displayMode,plane,&caps);
        if(rc!=VK_SUCCESS || !(caps.supportedAlpha&VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR)) continue;
        VkDisplaySurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
        info.displayMode=modes[mode].displayMode;info.planeIndex=plane;info.planeStackIndex=planes[plane].currentStackIndex;
        info.transform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;info.alphaMode=VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;info.imageExtent=modes[mode].parameters.visibleRegion;
        VkSurfaceKHR selected=VK_NULL_HANDLE;
        rc=vkCreateDisplayPlaneSurfaceKHR(instance,&info,nullptr,&selected);
        if(rc!=VK_SUCCESS) {
            if(rc!=VK_ERROR_INITIALIZATION_FAILED && rc!=VK_ERROR_FORMAT_NOT_SUPPORTED) return fail("create display surface",rc);
            std::cout<<"[game-display] rejected mode="<<mode<<" rc="<<rc<<"; trying fallback\n";
            continue;
        }
        surface=selected;
        width=info.imageExtent.width;height=info.imageExtent.height;hz=modes[mode].parameters.refreshRate/1000.f;
        const auto requested=mhp3rd::gpu::pacing::requested_rate(mhp3rd::settings::current().frame_rate,hz);
        std::cout<<"[game-display] "<<width<<"x"<<height<<" hz="<<hz<<" mode="<<mode<<" plane="<<plane
                 <<" requested_output="<<output<<" requested_fps="<<requested
                 <<" presentation_limit="<<game_performance::capped_rate(requested,hz)
                 <<" resolution_fallback="<<(height!=static_cast<int>(requested_height))<<"\n";
        return true;
    }
    error="No usable display mode at or below "+output+"; last rc="+std::to_string(rc);
    return false;
#else
    (void)instance;(void)error;surface=VK_NULL_HANDLE;
    std::cout<<"[game-display] Host offscreen GE renderer; no console scanout evidence\n";
#endif
    return true;
}
bool initialize_devices(){
#if YAKUMO_NATIVE
    int user_init=sceUserServiceInitialize(nullptr), pad_init=scePadInit(), audio_init=sceAudioOutInit();
    const int rc=sceUserServiceGetInitialUser(&user);
    std::cout<<"[game-devices] user_init="<<user_init<<" pad_init="<<pad_init<<" audio_init="<<audio_init<<" initial_user_rc="<<rc<<"\n";
    if(rc==0&&user>=0)text_input::initialize_ps5(user);
    return rc==0&&user>=0;
#else
    return true;
#endif
}
mhp3rd::gpu::PadState sample_pad(){
    game_observer::Timer timer{game_observer::pad_us};++game_observer::pad_samples;
    mhp3rd::gpu::PadState result{};
#if YAKUMO_NATIVE
    const auto now=milliseconds();
    if(pad_handle<0&&now>=next_pad_open){next_pad_open=now+500;pad_handle=scePadOpen(user,0,0,nullptr);std::cout<<"[game-pad] open="<<pad_handle<<"\n";}
    if(pad_handle<0){hide_hud_ready=false;return result;}
    alignas(16) unsigned char info[544];std::memset(info,0xa5,sizeof(info));
    const int rc=scePadGetControllerInformation(pad_handle,info);
    const bool guard=std::all_of(info+512,info+544,[](auto c){return c==0xa5;});
    const auto status=ps5::integration_pad_information(info,rc,guard);
    if(!status.valid)throw std::runtime_error("Pad ABI/API unverified: rc="+std::to_string(rc));
    if(!status.connected){if(pad_connected)std::cout<<"[game-pad] disconnected; input neutralized\n";pad_connected=false;hide_hud_ready=false;resolver.reset();const int closed=scePadClose(pad_handle);pad_handle=-1;if(closed)throw std::runtime_error("Pad close rc="+std::to_string(closed));return result;}
    if(!pad_connected){resolver.reset();pad_connected=true;std::cout<<"[game-pad] connected\n";}
    struct alignas(16) Sample{std::uint32_t buttons;unsigned char lx,ly,rx,ry,l2,r2,rest[502];} sample{};
    static_assert(sizeof(Sample)==512);
    const int read=scePadReadState(pad_handle,&sample);if(read)throw std::runtime_error("Pad read rc="+std::to_string(read));
    using namespace mhp3rd::input;
    const auto& settings=mhp3rd::settings::current();
    // Positional SDK bits, independently exercised by the existing device test.
    constexpr std::uint32_t bits[]={0x4000,0x2000,0x8000,0x1000,0x100000,0x10000,0x8,0x2,0x4,0x400,0x800,0x10,0x40,0x80,0x20};
    std::uint32_t actionable{};for(auto bit:bits)actionable|=bit;
    const auto centered_at=[&](unsigned char x,unsigned char y,float dead_zone){return std::hypot(float(int(x)-128),float(int(y)-128))<=127.f*dead_zone;};
    const auto centered=[&](unsigned char x,unsigned char y){return centered_at(x,y,settings.dead_zone);};
    // Unknown SDK status bits cannot represent a held mapped button. Use the
    // same actionable bits, trigger threshold and dead zone as normal gameplay.
    const auto trigger=[&](unsigned char value){return value!=0 && float(value)/255.f>=settings.trigger;};
    const bool neutral=(sample.buttons&actionable)==0&&!trigger(sample.l2)&&!trigger(sample.r2)&&
        centered(sample.lx,sample.ly)&&centered(sample.rx,sample.ry);
    // Returning from the system browser only needs digital controls and
    // triggers released. Stick drift must not keep the game input captured.
    const bool web_neutral=(sample.buttons&actionable)==0&&!trigger(sample.l2)&&!trigger(sample.r2);
    // A deliberate stick move can return control too. Keep a 20% floor so
    // small hardware drift cannot steal the browser session.
    const float handoff_dead_zone=std::max(settings.dead_zone,0.20f);
    const bool web_analog_active=!centered_at(sample.lx,sample.ly,handoff_dead_zone)||
        !centered_at(sample.rx,sample.ry,handoff_dead_zone);
#if YAKUMO_GAME_WEB
    const bool web_suppressed=game_web::suppress_sample((sample.buttons&0x6)==0x6,(sample.buttons&0x2000)!=0,web_neutral,text_input::service().blocks_input(),now,web_analog_active);
#else
    const bool web_suppressed=false;
#endif
    if(text_input::service().suppress_sample(neutral)||web_suppressed){hide_hud_ready=false;resolver.reset();return result;}
    auto held=[&](Binding binding){int i=pad_input_of(binding);if(settings.confirm_south){if(i==0)i=1;else if(i==1)i=0;}if(i==int(PadInput::LeftTrigger))return trigger(sample.l2);if(i==int(PadInput::RightTrigger))return trigger(sample.r2);return i>=0&&i<int(std::size(bits))&&(sample.buttons&bits[i]);};
    static auto saved_controls=settings.controls;
    static auto controls=game_pad::native_layout(saved_controls);
    if(saved_controls!=settings.controls){saved_controls=settings.controls;controls=game_pad::native_layout(saved_controls);resolver.reset();}
    const auto mapped=resolver.update(Table{controls.pad,controls.combos,true},held,now,settings.chord_window);
    result.buttons=mapped.buttons;result.fast_forward=mapped.fast_forward;
    result.native_lead_requested=lead_requested(Table{controls.pad,controls.combos,true},resolver.targets());
#if YAKUMO_GAME_WEB
    static const auto configured=game_settings::ps5_assignments();
    std::array<Action,5> assigned{};
    for(std::size_t i=0;i<configured.size();++i){
        switch(configured[i]){
        case game_settings::Ps5Assignment::L:assigned[i]=Action::L;break;
        case game_settings::Ps5Assignment::R:assigned[i]=Action::R;break;
        case game_settings::Ps5Assignment::HideHud:assigned[i]=Action::HideHud;break;
        case game_settings::Ps5Assignment::FastForward:assigned[i]=Action::FastForward;break;
        case game_settings::Ps5Assignment::Select:assigned[i]=Action::Select;break;
        case game_settings::Ps5Assignment::None:assigned[i]=Action::Count;break;
        }
    }
    const std::array<bool,5> pressed={trigger(sample.l2),trigger(sample.r2),(sample.buttons&0x100000)!=0,
        (sample.buttons&0x400)!=0,(sample.buttons&0x800)!=0};
    const auto extra=game_pad::assigned_actions(assigned,pressed);
    result.buttons|=extra.state.buttons;
    result.fast_forward|=extra.state.fast_forward;
    result.native_lead_requested|=extra.lead_requested;
#endif
    result.native_input_ready=true;
    // One pulse per fresh press; require release after reconnect or native IME.
    const bool hide_hud=mapped.hide_hud
#if YAKUMO_GAME_WEB
        ||extra.state.hide_hud
#endif
        ;
    result.hide_hud=hide_hud&&hide_hud_ready;
    hide_hud_ready=!hide_hud;
    static std::uint32_t previous_buttons{};
    if(result.buttons!=previous_buttons){
        std::cout<<"[game-pad] time_ms="<<now<<" guest_buttons="<<std::hex<<result.buttons<<std::dec<<'\n';
        previous_buttons=result.buttons;
    }
    auto shape=[&](unsigned char x,unsigned char y,unsigned char& ox,unsigned char& oy){float a=(int(x)-128)/127.f,b=(int(y)-128)/127.f;float len=std::sqrt(a*a+b*b);if(len<=settings.dead_zone)return;float scale=std::min((len-settings.dead_zone)/(1-settings.dead_zone),1.f)/len;ox=std::clamp(128+int(a*scale*127),0,255);oy=std::clamp(128+int(b*scale*127),0,255);};
    // The native source used to bypass right-stick settings and mapped axes.
    // Feed the same camera bytes to Yakumo's original camera/aim driver.
    shape(controls.swap_sticks?sample.rx:sample.lx,controls.swap_sticks?sample.ry:sample.ly,result.analog_x,result.analog_y);
    result.analog_x=std::clamp(int(result.analog_x)+mapped.stick_x,0,255);
    result.analog_y=std::clamp(int(result.analog_y)+mapped.stick_y,0,255);
    unsigned char camera_x=128,camera_y=128;
    shape(controls.swap_sticks?sample.lx:sample.rx,controls.swap_sticks?sample.ly:sample.ry,camera_x,camera_y);
    if(settings.right_stick==mhp3rd::settings::RightStick::Camera){
        int x=std::clamp(int(camera_x)-128+mapped.camera_x,-127,127);
        int y=std::clamp(int(camera_y)-128+mapped.camera_y,-127,127);
        result.right_x=128+(settings.invert_camera_x?-x:x);
        result.right_y=128+(settings.invert_camera_y?-y:y);
    }else if(settings.right_stick==mhp3rd::settings::RightStick::DPad){
        // Match upstream: the D-pad threshold measures raw axis travel.
        const float x=std::clamp((int(controls.swap_sticks?sample.lx:sample.rx)-128)/127.f,-1.f,1.f);
        const float y=std::clamp((int(controls.swap_sticks?sample.ly:sample.ry)-128)/127.f,-1.f,1.f);
        if(x <= -settings.right_stick_zone || mapped.camera_x<0)result.buttons|=0x80;
        if(x >= settings.right_stick_zone || mapped.camera_x>0)result.buttons|=0x20;
        if(y <= -settings.right_stick_zone || mapped.camera_y<0)result.buttons|=0x10;
        if(y >= settings.right_stick_zone || mapped.camera_y>0)result.buttons|=0x40;
    }
#else
    // Opt-in headless test input reaches the ordinary guest controller HLE.
    // It never patches guest memory or ships as a native controller source.
    if(std::getenv("YAKUMO_HOST_PAD")&&!game_files::root().empty()){
        const auto name=game_files::root()/"yakumo/config/host-pad.txt";
        game_files::ifstream input(name);
        if(!input){if(errno!=ENOENT)throw std::runtime_error("Host pad input open failed errno="+std::to_string(errno));return result;}
        unsigned value{};char extra{};
        if(!(input>>std::hex>>value)||value>0xffff||(input>>extra))throw std::runtime_error("Invalid host pad mask");
        result.buttons=value;
        static unsigned previous{};
        if(value!=previous){std::cout<<"[game-host-pad] guest_buttons="<<std::hex<<value<<std::dec<<" host_only=1\n";previous=value;}
    }
#endif
    return result;
}
bool audio_start(std::function<void(std::int16_t*,unsigned)> consume){
#if YAKUMO_NATIVE
    if(audio_live)return true;
    game_audio::telemetry.publish({},0,gain.load(),paused_audio.load(),0);
    audio_handle=sceAudioOutOpen(255,0,0,256,48000,4);std::cout<<"[game-audio] open="<<audio_handle<<" PCM 48000 float stereo\n";if(audio_handle<0){audio_fault=audio_handle;return false;}
    std::cout<<"[game-audio] normalization=s16-to-float output_scale="<<game_audio::output_scale
        <<" volume_percent="<<gain.load()*100<<" settings_range=0..100 periodic_levels=enabled\n";
    stop_audio=false;audio_live=true;
    audio_thread=std::thread([consume=std::move(consume)]{
        game_audio::Levels levels;
        std::uint64_t output_blocks{};
        std::array<std::int16_t,1024> input{};std::vector<std::int16_t> pending;std::uint64_t phase=0;
        alignas(64) float output[512]{};
        while(!stop_audio){
            unsigned need=unsigned((phase+255*44100)/48000+2);
            while(pending.size()/2<need){unsigned n=std::min<unsigned>(512,need-pending.size()/2);consume(input.data(),n);pending.insert(pending.end(),input.begin(),input.begin()+n*2);}
            const auto volume=gain.load();const auto paused=paused_audio.load();
            for(unsigned i=0;i<256;++i){auto pos=phase/48000;double t=double(phase%48000)/48000;for(unsigned c=0;c<2;++c){const double pcm=pending[pos*2+c]+(pending[(pos+1)*2+c]-pending[pos*2+c])*t;output[i*2+c]=game_audio::sample(pcm,volume,paused);levels.note(pcm,output[i*2+c]);}phase+=44100;}
            const auto retired=phase/48000;pending.erase(pending.begin(),pending.begin()+retired*2);phase-=retired*48000;
            int rc=sceAudioOutOutput(audio_handle,output);if(rc<0){stop_audio=true;audio_live=false;audio_fault=rc;std::cerr<<"[game-audio] output failure rc="<<rc<<"\n";}
            else game_audio::telemetry.publish(levels,++output_blocks,volume,paused,rc);
        }
        game_audio::write_levels(std::cout,game_audio::telemetry.snapshot(),"shutdown");
    });return true;
#else
    (void)consume;std::cout<<"[game-audio] Host capture has no playback device\n";return false;
#endif
}
void audio_stop(){stop_audio=true;if(audio_thread.joinable())audio_thread.join();
#if YAKUMO_NATIVE
    if(audio_handle>=0){int rc=sceAudioOutClose(audio_handle);if(rc<0)audio_fault=rc;std::cout<<"[game-audio] close="<<rc<<"\n";audio_handle=-1;}
#endif
    audio_live=false;
}
bool audio_running(){return audio_live;}
int audio_error(){return audio_fault;}
void audio_pause(bool p){paused_audio=p;}
void audio_gain(float value){gain=std::isfinite(value)?std::clamp(value,0.f,1.f):0.f;}
void shutdown_devices(){audio_stop();
#if YAKUMO_NATIVE
    hide_hud_ready=false;pad_connected=false;resolver.reset();
    if(pad_handle>=0){std::cout<<"[game-pad] close="<<scePadClose(pad_handle)<<"\n";pad_handle=-1;}
#endif
}
}
