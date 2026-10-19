// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_probe.hpp"
#include <array>
#include <cmath>
#include <cstring>
extern "C" {
int sceAudioOutOpen(int,int,int,unsigned,unsigned,unsigned);
int sceAudioOutOutput(int,const void*);
int sceAudioOutClose(int);
int scePadGetControllerInformation(int,void*);
}
namespace yakumo::ps5 {
bool profile_native_errors(Log& log){
    alignas(64) std::array<float,512> pcm{};
    for(unsigned frame=0;frame<256;++frame)pcm[2*frame]=pcm[2*frame+1]=0.02f*std::sin(float(frame)*6.28318530718f*220.0f/48000.0f);
    bool pass=true;
    const int bad_open=sceAudioOutOpen(255,0,0,0,48000,4);
    log.write("[profile-sdk] audio invalid-grain open rc=%d expected=negative",bad_open);
    if(bad_open>=0){pass=false;const int cleanup=sceAudioOutClose(bad_open);log.write("[profile-sdk] unexpected handle cleanup rc=%d",cleanup);}
    const int bad_output=sceAudioOutOutput(-1,pcm.data());pass=bad_output<0&&pass;
    log.write("[profile-sdk] audio invalid-handle output rc=%d expected=negative rejection=%s",bad_output,bad_output<0?"PASS":"FAIL");
    alignas(16) std::array<unsigned char,544> info;info.fill(0xa5);
    const int bad_pad=scePadGetControllerInformation(-1,info.data());
    bool guard=true;for(unsigned i=512;i<544;++i)guard=guard&&info[i]==0xa5;
    pass=pass&&bad_pad<0&&guard;
    log.write("[profile-sdk] pad invalid-handle info rc=%d expected=negative guard=%s; API error is not a disconnected observation",bad_pad,guard?"PASS":"FAIL");
    for(unsigned cycle=1;cycle<=2;++cycle){
        const int handle=sceAudioOutOpen(255,0,0,256,48000,4);log.write("[profile-sdk] audio reopen cycle=%u handle=%d",cycle,handle);
        if(handle<0){pass=false;continue;}
        bool output=true;int rc=0;unsigned blocks=0;for(;blocks<36;++blocks){rc=sceAudioOutOutput(handle,pcm.data());if(rc<0){output=false;break;}}
        const int closed=sceAudioOutClose(handle);const int stale=sceAudioOutOutput(handle,pcm.data());
        const bool ok=output&&closed==0&&stale<0;pass=pass&&ok;
        log.write("[profile-sdk] audio recovery cycle=%u blocks=%u output_last_rc=%d close=%d closed-handle output=%d expected=negative recovery=%s",cycle,blocks,rc,closed,stale,ok?"PASS":"FAIL");
    }
    log.write("[profile-sdk-summary] expected real API rejections, normal reopen/output/close=%s",pass?"PASS":"FAIL");return pass;
}
}
