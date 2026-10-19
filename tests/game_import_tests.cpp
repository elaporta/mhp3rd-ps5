// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the real MPEG ringbuffer feeder through the local import boundary.
#include "game_import.hpp"
#include "hle/hle_common.hpp"
#include <iostream>
#include <stdexcept>
namespace {
constexpr std::uint32_t entry=0x08821000,callback=0x08822000,finished=0x08823000;
constexpr std::uint32_t ring=0x08840000,data=0x08850000;
unsigned feeds{};
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void imported(psprecomp::Runtime& rt,psprecomp::AllegrexContext& ctx){
    const auto token=psprecomp::capture_runtime_execution_context();
    rt.invoke_import("sceMpeg",0xB240A59E,ctx);
    yakumo::game_import::complete(ctx,token,entry);
}
void feeder(psprecomp::Runtime& rt,psprecomp::AllegrexContext& ctx){
    const auto expected=data+(feeds==0?2048:0);
    check(ctx.gpr[4]==expected&&ctx.gpr[5]==1&&ctx.gpr[6]==0x1234,"wrapped real guest callback arguments");
    check(ctx.gpr[31]==mhp3rd::kGuestCallReturnStub,"real kernel callback trampoline");
    for(unsigned i=0;i<2048;++i)rt.memory().store8(expected+i,0x5a+feeds);
    ++feeds;ctx.set_gpr(2,1);ctx.pc=ctx.gpr[31];
}
void finish(psprecomp::Runtime& rt,psprecomp::AllegrexContext& ctx){
    check(feeds==2&&ctx.gpr[2]==2,"both callback results reach the original import return");
    rt.stop("public ringbuffer callback completed");
}
}
int main(){try{
    psprecomp::Runtime runtime(64*1024*1024);
    auto& kernel=mhp3rd::kernel();kernel=mhp3rd::Kernel{};
    kernel.install(runtime,0x08801000,0x08820000);
    runtime.nids().load_csv(PSPRECOMP_TEST_NIDS_CSV);
    mhp3rd::HleRegistrar registrar(runtime);mhp3rd::register_mpeg(registrar);
    auto& ctx=runtime.cpu();
    ctx.pc=entry;ctx.set_gpr(31,finished);
    const unsigned args[]={ring,2,data,4096,callback,0x1234};
    for(unsigned i=0;i<6;++i)ctx.set_gpr(4+i,args[i]);
    runtime.invoke_import("sceMpeg",0x37295ED8,ctx); // RingbufferConstruct.
    check(!runtime.stopped()&&runtime.memory().load32(ring)==2,"real MPEG ring construction");
    runtime.memory().store32(ring+4,1);runtime.memory().store32(ring+8,1);
    kernel.start_loader_thread(ctx,entry,0);
    ctx.set_gpr(4,ring);ctx.set_gpr(5,2);ctx.set_gpr(6,2);ctx.set_gpr(31,finished);
    runtime.register_function(entry,imported,"public_mpeg_import");
    runtime.register_function(callback,feeder,"public_guest_feeder");
    runtime.register_function(finished,finish,"public_guest_return");
    runtime.run(entry,32);
    check(runtime.stop_reason()=="public ringbuffer callback completed","bounded actual callback execution");
    check(runtime.memory().load32(ring+12)==2&&runtime.memory().load32(ring+8)==1,
          "real HLE commits wrapped packets exactly once");
    check(runtime.memory().load8(data)==0x5b&&runtime.memory().load8(data+2048)==0x5a,"real feeder wrote both ring segments");
    ctx.pc=entry;ctx.set_gpr(31,finished);
    auto token=psprecomp::capture_runtime_execution_context();
    yakumo::game_import::complete(ctx,token,entry);check(ctx.pc==finished,"ordinary same-thread return");
    ctx.pc=entry;token=psprecomp::capture_runtime_execution_context();
    const auto uid=psprecomp::runtime_thread_uid();
    psprecomp::set_runtime_thread_identity(uid+100,"other");
    psprecomp::set_runtime_thread_identity(uid,"back");
    yakumo::game_import::complete(ctx,token,entry);
    check(ctx.pc==entry,"switch-away/back invalidates stale caller even with matching PC and UID");
    kernel=mhp3rd::Kernel{};psprecomp::set_runtime_starvation_hook(nullptr,0);
    std::cout<<"game import: actual wrapped MPEG guest callbacks, normal return and scheduler token PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
