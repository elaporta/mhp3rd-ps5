// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
namespace yakumo::game_web {
struct NativeApi {
    int (*bind)(int,const sockaddr*,socklen_t){};
    int (*listen)(int,int){};
    int (*accept)(int,sockaddr*,socklen_t*){};
    int (*browser)(const char*,const void*){};
    int (*sysctl)(const int*,unsigned,void*,std::size_t*,const void*,std::size_t){};
    const char* missing() const {
        if(!bind)return "sceNetBind";
        if(!listen)return "sceNetListen";
        if(!accept)return "sceNetAccept";
        if(!browser)return "sceSystemServiceLaunchWebBrowser";
        if(!sysctl)return "sysctl";
        return nullptr;
    }
};
// CTL_KERN/KERN_ARND and sysctl signature come from the pinned SDK sysctl.h.
// Read kernel entropy without libSceRandom, filesystem state or a weak PRNG.
inline bool kernel_entropy(const NativeApi& api,unsigned char* data,std::size_t size) {
    if(!api.sysctl||!data||!size)return false;
    const auto total=size;
    auto* const begin=data;
    const int mib[]={1,37};
    while(size) {
        const auto requested=std::min<std::size_t>(size,256);
        auto got=requested;
        if(api.sysctl(mib,2,data,&got,nullptr,0)!=0||!got||got>requested) {
            std::memset(begin,0,total);return false;
        }
        data+=got;size-=got;
    }
    return true;
}
}
