// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.hpp"
#include "integration_pad.hpp"
#include <algorithm>
#include <array>
#include <cstring>

extern "C" {
int sceUserServiceGetInitialUser(int*);
int scePadOpen(int,int,int,const void*);
int scePadReadState(int,void*);
int scePadClose(int);
int scePadGetControllerInformation(int,void*);
int sceAudioOutOpen(int,int,int,unsigned,unsigned,unsigned);
int sceAudioOutOutput(int,const void*);
int sceAudioOutClose(int);
}
namespace yakumo::ps5 {
namespace {
struct alignas(16) GuardedInfo {unsigned char bytes[512]{};unsigned char guard[32];};
struct alignas(16) PadSample {std::uint32_t buttons;unsigned char lx,ly,rx,ry,l2,r2;unsigned char rest[502];};
static_assert(sizeof(PadSample)==512);
class Devices final: public IntegrationDevices {
    int pad_{-1},user_{-1},audio_{-1},audio_status_{};
    bool invalid_{},reconnecting_{};
    unsigned last_open_{};
    // Extra space permits alignment without requiring over-aligned allocation.
    // Storage survives the SDK call and remains owned by the audio worker.
    std::array<float,528> pcm_{};
public:
    IntegrationInput poll(unsigned,unsigned tick) override {
        IntegrationInput result{};
        if(user_<0){
            result.operation="initial-user";
            result.api_result=sceUserServiceGetInitialUser(&user_);
            if(result.api_result!=0||user_<0){invalid_=true;return result;}
        }
        const bool attempted=pad_<0 && (last_open_==0||tick-last_open_>=15);
        if(attempted){last_open_=tick;pad_=scePadOpen(user_,0,0,nullptr);}
        if(pad_<0){
            result.operation="pad-open";result.api_result=pad_;
            result.valid=!invalid_&&reconnecting_;result.open_attempt=attempted;
            if(!result.valid)invalid_=true;
            return result;
        }
        GuardedInfo info{};std::memset(info.bytes,0xa5,sizeof(info.bytes));std::memset(info.guard,0xa5,sizeof(info.guard));
        const int rc=scePadGetControllerInformation(pad_,info.bytes);
        const bool guard=std::all_of(std::begin(info.guard),std::end(info.guard),[](auto b){return b==0xa5;});
        result=integration_pad_information(info.bytes,rc,guard);
        result.operation="pad-info";
        if(!result.valid){invalid_=true;return result;}
        if(!result.connected){
            reconnecting_=true;
            const int closed=scePadClose(pad_);pad_=-1;
            if(closed!=0){result.operation="pad-close";result.api_result=closed;result.valid=false;invalid_=true;}
            return result;
        }
        reconnecting_=false;
        PadSample sample{};
        const int read=scePadReadState(pad_,&sample);
        if(read!=0){result.operation="pad-read";result.api_result=read;result.valid=false;invalid_=true;return result;}
        result.buttons=sample.buttons;result.lx=sample.lx;result.ly=sample.ly;result.rx=sample.rx;result.ry=sample.ry;result.l2=sample.l2;result.r2=sample.r2;
        return result;
    }
    bool close_pad() override {const bool ok=pad_<0||scePadClose(pad_)==0;pad_=-1;return ok;}
    bool open_audio() override {audio_status_=audio_=sceAudioOutOpen(255,0,0,256,48000,4);return audio_>=0;}
    bool output_audio(const float* stereo,unsigned frames) override {
        if(!stereo||frames!=256||audio_<0){audio_status_=-1;return false;}
        const auto address=(reinterpret_cast<std::uintptr_t>(pcm_.data())+63)&~std::uintptr_t(63);
        auto* aligned=reinterpret_cast<float*>(address);
        std::copy_n(stereo,512,aligned);
        audio_status_=sceAudioOutOutput(audio_,aligned);
        // Match the already hardware-tested platform diagnostic's convention.
        return audio_status_>=0;
    }
    int audio_status() const override {return audio_status_;}
    bool close_audio() override {audio_status_=audio_<0?0:sceAudioOutClose(audio_);audio_=-1;return audio_status_==0;}
};
}
std::unique_ptr<IntegrationDevices> make_integration_devices(){return std::make_unique<Devices>();}
}
