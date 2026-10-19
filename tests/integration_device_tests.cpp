// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual native adapter, replacing only the SDK ABI boundary.
#include "integration.hpp"
#include "integration_pad.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace yakumo::ps5;
namespace {
int user_rc{},open_rc{7},info_rc{},read_rc{},close_rc{},audio_open{9},output_rc{},audio_close{},connected{1};
unsigned opens{},writes{};bool corrupt_guard{};const void* previous_buffer{};
void check(bool b){if(!b)throw std::runtime_error("native device contract rejected");}
void reset(){user_rc=info_rc=read_rc=close_rc=output_rc=audio_close=0;open_rc=7;audio_open=9;connected=1;opens=writes=0;corrupt_guard=false;previous_buffer=nullptr;}
}
extern "C" {
int sceUserServiceGetInitialUser(int* p){*p=42;return user_rc;}
int scePadOpen(int user,int type,int index,const void* extra){check(user==42&&type==0&&index==0&&!extra);++opens;return open_rc;}
int scePadReadState(int handle,void* p){check(handle==7);const std::array<unsigned char,10> sample{0,64,0,0,130,131,132,133,20,30};std::memcpy(p,sample.data(),sample.size());return read_rc;}
int scePadClose(int handle){check(handle==7);return close_rc;}
int scePadGetControllerInformation(int handle,void* p){check(handle==7);IntegrationPadInformation info{};info.connected=connected;info.density=44;info.resolution_x=1920;info.resolution_y=942;std::memcpy(p,&info,sizeof(info));if(corrupt_guard)static_cast<unsigned char*>(p)[512]=0;return info_rc;}
int sceAudioOutOpen(int user,int type,int index,unsigned grain,unsigned rate,unsigned format){check(user==255&&type==0&&index==0&&grain==256&&rate==48000&&format==4);return audio_open;}
int sceAudioOutOutput(int handle,const void* p){check(handle==9&&reinterpret_cast<std::uintptr_t>(p)%64==0);if(previous_buffer)check(p==previous_buffer);previous_buffer=p;check(static_cast<const float*>(p)[0]==0.01f&&static_cast<const float*>(p)[511]==-0.02f);++writes;return output_rc;}
int sceAudioOutClose(int handle){check(handle==9);return audio_close;}
}
int main(){try{
    reset();auto d=make_integration_devices();auto input=d->poll(1,1);check(input.valid&&input.connected&&input.buttons==0x4000&&input.lx==130&&input.l2==20&&opens==1);
    std::array<float,513> samples{};samples[1]=0.01f;samples[512]=-0.02f;
    check(d->open_audio()&&d->audio_status()==9);
    output_rc=256;check(d->output_audio(samples.data()+1,256)&&d->audio_status()==256);
    output_rc=0;check(d->output_audio(samples.data()+1,256)&&writes==2);
    output_rc=-17;check(!d->output_audio(samples.data()+1,256)&&d->audio_status()==-17);
    check(!d->output_audio(samples.data(),255)&&writes==3);check(d->close_audio()&&d->close_pad());
    reset();user_rc=-3;d=make_integration_devices();input=d->poll(1,1);check(!input.valid&&input.api_result==-3&&opens==0);
    reset();open_rc=-4;d=make_integration_devices();input=d->poll(1,1);check(!input.valid&&!input.connected&&input.api_result==-4);
    reset();d=make_integration_devices();check(d->poll(2,1).connected);connected=0;input=d->poll(2,2);check(input.valid&&!input.connected&&input.buttons==0&&input.lx==128);
    open_rc=-4;input=d->poll(2,16);check(input.valid&&!input.connected&&input.open_attempt&&input.api_result==-4);
    input=d->poll(2,17);check(input.valid&&!input.open_attempt&&opens==2);
    open_rc=7;connected=1;input=d->poll(2,31);check(input.valid&&input.connected&&input.buttons==0x4000&&opens==3);check(d->close_pad());
    reset();info_rc=-8;d=make_integration_devices();input=d->poll(1,1);check(!input.valid&&!input.connected&&input.api_result==-8);check(d->close_pad());
    reset();corrupt_guard=true;d=make_integration_devices();check(!d->poll(1,1).valid);check(d->close_pad());
    reset();read_rc=-9;d=make_integration_devices();input=d->poll(1,1);check(!input.valid&&input.api_result==-9&&input.buttons==0);check(d->close_pad());
    reset();d=make_integration_devices();check(d->poll(1,1).connected);close_rc=-10;check(!d->close_pad());
    reset();audio_open=-11;d=make_integration_devices();check(!d->open_audio()&&d->audio_status()==-11&&!d->output_audio(samples.data(),256));
    reset();d=make_integration_devices();check(d->open_audio());audio_close=-12;check(!d->close_audio()&&d->audio_status()==-12);
    std::cout<<"[host-native-devices] PASS real adapter: user identity, positive/zero/negative audio, persistent aligned PCM, reopen, neutralization, ABI/API rejection and cleanup failures\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
