// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_web_native.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
namespace {
int mode{},calls{};
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int entropy(const int* mib,unsigned count,void* output,std::size_t* length,const void* input,std::size_t input_size) {
    check(mib[0]==1&&mib[1]==37&&count==2&&!input&&!input_size,"read-only kernel entropy ABI");
    check(*length>0&&*length<=256,"kernel request bounded to 256 bytes");
    ++calls;
    if(mode==1)return -1;
    if(mode==2){*length=0;return 0;}
    if(mode==3){++*length;return 0;}
    if(mode==4)*length=1;
    auto* bytes=static_cast<unsigned char*>(output);
    for(std::size_t i=0;i<*length;++i)bytes[i]=0xab;
    if(mode==5&&calls==2)return -1;
    return 0;
}
int fake_bind(int,const sockaddr*,socklen_t){return 0;}
int fake_listen(int,int){return 0;}
int fake_accept(int,sockaddr*,socklen_t*){return 0;}
int browser(const char*,const void*){return 0;}
}
int main(){try{
    using namespace yakumo::game_web;
    NativeApi api{};check(std::string(api.missing())=="sceNetBind","missing bind reported");
    api.bind=fake_bind;check(std::string(api.missing())=="sceNetListen","missing listen reported");
    api.listen=fake_listen;check(std::string(api.missing())=="sceNetAccept","missing accept reported");
    api.accept=fake_accept;check(std::string(api.missing())=="sceSystemServiceLaunchWebBrowser","missing browser reported");
    api.browser=browser;check(std::string(api.missing())=="sysctl","missing entropy reported");
    std::array<unsigned char,513> bytes{};
    check(!kernel_entropy(api,bytes.data(),bytes.size()),"null entropy import rejected");
    api.sysctl=entropy;check(api.missing()==nullptr,"complete API accepted");
    check(!kernel_entropy(api,nullptr,1)&&!kernel_entropy(api,bytes.data(),0),"invalid output rejected");
    for(mode=0;mode<=5;++mode){
        bytes.fill(0xff);calls=0;
        const bool result=kernel_entropy(api,bytes.data(),bytes.size());
        check(result==(mode==0||mode==4),"entropy success/failure contract");
        for(auto byte:bytes)check(byte==(result?0xab:0),"failed partial entropy is erased; no weak token fallback");
        if(mode==0)check(calls==3,"large entropy request split into bounded reads");
        if(mode==4)check(calls==513,"short entropy reads completed");
    }
    std::cout<<"kernel entropy bounds, partial/error wiping and absent API contracts PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
