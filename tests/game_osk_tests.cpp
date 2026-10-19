// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_input_service.hpp"
#include "game_text_observer.hpp"
#include "hle/hle_common.hpp"
#include "hle/utility_dialog.hpp"
#include "settings/settings.hpp"
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
using namespace yakumo::text_input;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Fake final:Backend {
    Request request;bool running{},fail_open{};std::optional<Result> next;
    std::optional<Failure> open(const Request& r)override{request=r;if(fail_open)return Failure{"native-unavailable",-63};running=true;return {};}
    std::optional<Result> poll()override{if(!next)return {};running=false;auto value=std::move(next);next.reset();return value;}
    void cancel()override{next=Result{0,Outcome::Cancelled,{},{}};}
    bool active()const override{return running;}
};
}
int main(){try{
    Fake backend;check(service().set_backend(backend),"inject real HLE backend");
    psprecomp::Runtime rt(64*1024*1024);auto& memory=rt.memory();auto& cpu=rt.cpu();auto& kernel=mhp3rd::kernel();
    kernel=mhp3rd::Kernel{};kernel.install(rt,0x08801000,0x08820000);kernel.start_loader_thread(cpu,0x08820000,0);
    rt.nids().load_csv(PSPRECOMP_TEST_NIDS_CSV);std::map<std::string,unsigned> nids;
    for(const auto& entry:rt.nids().all())if(entry.library=="sceUtility")nids[entry.name]=entry.nid;
    mhp3rd::HleRegistrar hle(rt);mhp3rd::register_utility(hle,"unused-public-fixture");
    constexpr unsigned params=0x08840000,field=params+0x100,output=field+0x100,initial=output+0x100;
    auto call=[&](const char* name,unsigned a0=0){cpu.set_gpr(4,a0);cpu.set_gpr(31,0x08822000);rt.invoke_import("sceUtility",nids.at(name),cpu);check(!rt.stopped(),"actual HLE continues");return cpu.gpr[2];};
    auto prepare=[&]{memory.store32(params,0x40);memory.store32(params+0x30,1);memory.store32(params+0x34,field);
        memory.store32(field+0x10,0xdf0f);memory.store32(field+0x14,1);memory.store32(field+0x1c,0);
        memory.store32(field+0x20,initial);memory.store16(initial,0);memory.store32(field+0x24,13);memory.store32(field+0x28,output);memory.store32(field+0x30,0);
        for(unsigned i=0;i<14;++i)memory.store16(output+i*2,0x5555);
    };
    auto finish=[&]{check(yakumo::game_text_observer::background_requested,"background retained through QUIT");check(call("sceUtilityOskGetStatus")==3,"guest sees QUIT");check(call("sceUtilityOskShutdownStart")==0,"original shutdown");check(call("sceUtilityOskGetStatus")==4&&call("sceUtilityOskGetStatus")==0,"FINISHED to NONE");check(!yakumo::game_text_observer::background_requested,"background released at NONE");service().suppress_sample(true);};
    mhp3rd::settings::current().name="Hunter";mhp3rd::settings::current().name_entry=mhp3rd::settings::NameEntry::Fixed;
    prepare();check(call("sceUtilityOskShutdownStart")==mhp3rd::kErrorUtilityInvalidStatus,"inactive shutdown");
    check(call("sceUtilityOskInitStart",params)==0,"real InitStart");check(call("sceUtilityOskInitStart",params)==mhp3rd::kErrorUtilityInvalidStatus,"duplicate HLE init");
    check(backend.running&&backend.request.initial.empty()&&backend.request.max_utf16_units==12,"fixed Hunter not substituted; original capacity");
    check(call("sceUtilityOskGetStatus")==1&&call("sceUtilityOskGetStatus")==2&&call("sceUtilityOskGetStatus")==2,"VISIBLE waits for actual answer");
    check(memory.load16(output)==0x5555,"pending does not write output");backend.next=Result{0,Outcome::Confirmed,"Yakumo27",{}};
    check(call("sceUtilityOskUpdate")==0,"real Update delivers");
    const std::string expected="Yakumo27";for(unsigned i=0;i<expected.size();++i)check(memory.load16(output+2*i)==expected[i],"confirmed text reaches original guest buffer");
    check(memory.load16(output+16)==0&&memory.load16(output+18)==0x5555&&memory.load16(output+26)==0x5555,"terminator and canaries");
    check(memory.load32(field+0x2c)==2&&memory.load32(params+0x1c)==0&&memory.load32(params+0x38)==3,"field/header/state consumed by game");
    check(yakumo::game_text_observer::expected=="Yakumo27","character observer uses confirmed value");finish();
    for(auto outcome:{Outcome::Cancelled,Outcome::Error}){
        prepare();check(call("sceUtilityOskInitStart",params)==0,"reopen");call("sceUtilityOskGetStatus");
        backend.next=Result{0,outcome,"Partial",Failure{"injected",-7}};call("sceUtilityOskGetStatus");
        check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1&&memory.load32(params+0x1c)==1,"cancel/error preserve buffer and guest semantics");finish();
    }
    prepare();backend.fail_open=true;call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");call("sceUtilityOskGetStatus");
    check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"unavailable native API never confirms Hunter");finish();backend.fail_open=false;
    for(const auto& text:{"", "Percent%", "tooLongName123"}){
        prepare();call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Confirmed,text,{}};call("sceUtilityOskGetStatus");
        check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"invalid name not partially applied");finish();
    }
    prepare();memory.store16(initial,'A');memory.store16(initial+2,0);memory.store32(field+0x30,1);
    call("sceUtilityOskInitStart",params);check(backend.request.initial=="A"&&backend.request.max_characters==1,"original initial text and explicit limit");
    call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Confirmed,"Z",{}};call("sceUtilityOskGetStatus");check(memory.load16(output)=='Z'&&memory.load16(output+2)==0,"explicit limit applied exactly");finish();
    prepare();call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");memory.store32(field+0x28,output+0x20);
    backend.next=Result{0,Outcome::Confirmed,"Altered",{}};call("sceUtilityOskGetStatus");check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"changed destination rejected");finish();
    prepare();memory.store32(field+0x24,1);check(call("sceUtilityOskInitStart",params)==mhp3rd::error::kIllegalArgument,"no room for text rejected");
    prepare();memory.store32(params+0x30,2);check(call("sceUtilityOskInitStart",params)==mhp3rd::error::kIllegalArgument,"unsupported multi-field request rejected");
    prepare();memory.store32(field+0x28,params);check(call("sceUtilityOskInitStart",params)==mhp3rd::error::kIllegalArgument,"overlapping output cannot corrupt dialogue state");
    prepare();memory.store16(initial,0xd800);memory.store16(initial+2,0);check(call("sceUtilityOskInitStart",params)==mhp3rd::error::kIllegalArgument,"malformed guest UTF-16 rejected");
    prepare();memory.store32(field+0x20,0xfffffffe);check(call("sceUtilityOskInitStart",params)==mhp3rd::error::kIllegalArgument,"unmapped guest initial rejected");
    // Exercise the actual HLE with generic fields, not only a policy mock.
    auto comment=[&](unsigned lines=1){prepare();memory.store32(field+0x24,65);memory.store32(field+0x30,48);
        memory.store32(field+0x10,0xf);memory.store32(field+0x14,lines);
        for(unsigned i=0;i<66;++i)memory.store16(output+2*i,0x5555);};
    for(const std::string text:{"Guild card: 100% ready!",""}){
        comment();check(call("sceUtilityOskInitStart",params)==0,"generic text opens shared OSK");
        check(backend.request.allow_empty&&backend.request.max_characters==48&&backend.request.max_utf16_units==64&&
              backend.request.title=="Enter text"&&!backend.request.multiline,"consumer-specific text contract");
        call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Confirmed,text,{}};call("sceUtilityOskUpdate");
        for(unsigned i=0;i<text.size();++i)check(memory.load16(output+2*i)==text[i],"generic text reaches original buffer");
        check(memory.load16(output+2*text.size())==0&&memory.load16(output+130)==0x5555&&memory.load32(field+0x2c)==2,"empty/comment confirmation and capacity canary");
        check(yakumo::game_text_observer::expected=="Z","generic comment not mistaken for hunter rename");finish();
    }
    comment(3);call("sceUtilityOskInitStart",params);check(backend.request.multiline,"multiline consumer hint");
    call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Confirmed,"First\nSecond",{}};call("sceUtilityOskUpdate");
    check(memory.load16(output+10)=='\n'&&memory.load32(field+0x2c)==2,"multiline text delivered intact");finish();
    comment();call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Cancelled,"Partial",{}};call("sceUtilityOskUpdate");
    check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"comment cancellation preserves original buffer");finish();
    comment();call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");memory.store32(field+0x10,1);
    backend.next=Result{0,Outcome::Confirmed,"Changed",{}};call("sceUtilityOskUpdate");check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"changed consumer categories reject atomic commit");finish();
    // Reproduce the real Guild Card contract from the hardware log, including
    // its Japanese/fullwidth initial value, not the synthetic Latin-only field.
    auto guild=[&]{prepare();memory.store32(field+0x24,45);memory.store32(field+0x10,0xdf00);
        const std::u16string original=u"初めまして！よろしくお願いします。";
        for(unsigned i=0;i<=original.size();++i)memory.store16(initial+2*i,i==original.size()?0:original[i]);
        for(unsigned i=0;i<46;++i)memory.store16(output+2*i,0x5555);};
    std::string original;check(decode_utf16(u"初めまして！よろしくお願いします。",original),"fixture UTF-8");
    for(const auto& text:std::vector<std::string>{"Yakumo27",original,"",std::string(44,'a')}){
        guild();call("sceUtilityOskInitStart",params);
        check(backend.running&&backend.request.initial==original&&backend.request.max_utf16_units==44,"real Guild Card opens and preserves initial text");
        call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Confirmed,text,{}};call("sceUtilityOskUpdate");
        std::u16string expected;check(yakumo::text_input::decode_utf8(text,expected),"fixture conversion");
        for(auto& c:expected)if(c>=0x20&&c<=0x7e)c=c==' '?0x3000:c+0xfee0;
        for(unsigned i=0;i<expected.size();++i)check(memory.load16(output+2*i)==expected[i],"fullwidth/Japanese Guild Card reaches original guest buffer");
        check(memory.load16(output+2*expected.size())==0&&memory.load16(output+90)==0x5555&&memory.load32(field+0x2c)==2,"complete comment plus NUL inside 45 units");finish();
    }
    for(const auto& text:std::vector<std::string>{std::string(45,'a'),"\xf0\x9f\x98\x80"}){
        guild();call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Confirmed,text,{}};call("sceUtilityOskUpdate");
        check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"Guild Card rejects overflow/unsupported glyph without partial writes");finish();
    }
    guild();call("sceUtilityOskInitStart",params);call("sceUtilityOskGetStatus");backend.next=Result{0,Outcome::Cancelled,"Partial",{}};call("sceUtilityOskUpdate");
    check(memory.load16(output)==0x5555&&memory.load32(field+0x2c)==1,"real Guild Card cancellation preserves text");finish();
    check(call("sceUtilityOskInitStart",0xfffffffe)==mhp3rd::error::kIllegalArgument,"invalid range rejected");
    kernel=mhp3rd::Kernel{};psprecomp::set_runtime_starvation_hook(nullptr,0);
    std::cout<<"real projected OSK: buffer, flags, full lifecycle, no Hunter fallback PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
