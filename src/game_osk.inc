// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside mhp3rd's anonymous utility namespace in the GPL projection.
struct NativeOsk {
    std::uint32_t params{},field{},output{},capacity{},limit{},status{};
    std::uint64_t request{};
    std::uint32_t type{},lines{};
    bool name_profile{};
    std::optional<yakumo::text_input::Result> immediate;
};
NativeOsk& native_osk(){static NativeOsk state;return state;}
bool osk_range(const psprecomp::GuestMemory& memory,std::uint32_t address,std::size_t bytes){
    return address&&bytes<=0xffffffffu-address&&memory.contains(address,bytes);
}
bool osk_overlap(std::uint32_t a,std::size_t n,std::uint32_t b,std::size_t m){
    const auto x=psprecomp::GuestMemory::canonical(a),y=psprecomp::GuestMemory::canonical(b);
    return static_cast<std::uint64_t>(x)<static_cast<std::uint64_t>(y)+m&&
        static_cast<std::uint64_t>(y)<static_cast<std::uint64_t>(x)+n;
}
bool osk_string(const psprecomp::GuestMemory& memory,std::uint32_t address,std::string& text){
    if(!address){text.clear();return true;}
    std::u16string units;
    for(unsigned i=0;i<=512;++i){
        if(!osk_range(memory,address,2*(i+1)))return false;
        const auto c=memory.load16(address+2*i);
        if(!c)return yakumo::text_input::decode_utf16(units,text);
        units.push_back(static_cast<char16_t>(c));
    }
    return false;
}
void osk_status(psprecomp::GuestMemory& memory,std::uint32_t status){
    auto& s=native_osk();s.status=status;
    if(osk_range(memory,s.params,0x3c))memory.store32(s.params+0x38,status);
}
void osk_error(const yakumo::text_input::Failure& failure){
    std::cerr<<"[osk-native] error operation="<<failure.operation<<" rc="<<failure.code<<" no_text_applied=1\n";
    yakumo::game_text_observer::notice="Yakumo: text was not applied ("+failure.operation+"). Reopen text entry. See game-first-frame.log.";
}
void osk_deliver(psprecomp::GuestMemory& memory){
    using namespace yakumo::text_input;
    auto& s=native_osk();
    auto result=s.immediate?std::move(s.immediate):service().take(s.request);
    s.immediate.reset();if(!result)return;
    // Snapshot destinations at InitStart and revalidate the complete original
    // parameter contract before any commit. No write follows an altered field.
    const bool intact=osk_range(memory,s.params,0x3c)&&osk_range(memory,s.field,0x34)&&
        memory.load32(s.params+0x30)==1&&memory.load32(s.params+0x34)==s.field&&
        memory.load32(s.field+0x24)==s.capacity&&memory.load32(s.field+0x28)==s.output&&
        memory.load32(s.field+0x30)==s.limit&&memory.load32(s.field+0x10)==s.type&&
        memory.load32(s.field+0x14)==s.lines&&osk_range(memory,s.output,2*s.capacity);
    if(!intact){result=Result{s.request,Outcome::Error,{},Failure{"guest-field-changed",-1}};}
    std::u16string units;
    if(result->outcome==Outcome::Confirmed&&(!yakumo::game_text_policy::encode_guest(result->text,s.type,s.name_profile,units)||units.size()>=s.capacity))
        result=Result{s.request,Outcome::Error,{},Failure{"guest-output-bounds",-1}};
    if(result->outcome==Outcome::Error)osk_error(result->failure);
    const bool confirmed=intact&&result->outcome==Outcome::Confirmed;
    if(confirmed){
        for(std::size_t i=0;i<units.size();++i)memory.store16(s.output+2*static_cast<std::uint32_t>(i),units[i]);
        memory.store16(s.output+2*static_cast<std::uint32_t>(units.size()),0);
        bool same=true;for(std::size_t i=0;i<=units.size();++i)same&=memory.load16(s.output+2*static_cast<std::uint32_t>(i))==(i==units.size()?0:units[i]);
        if(!same)throw psprecomp::Error("OSK committed output readback differs");
        if(s.name_profile)yakumo::game_text_observer::confirmed(result->text);
        std::cout<<"[osk-native] request="<<s.request<<" confirmed text=\""<<yakumo::game_text_policy::escaped_for_log(result->text)<<"\" output="
                 <<psprecomp::hex32(s.output)<<" utf16_units="<<units.size()<<" guest_width="<<(s.name_profile?"unchanged":"psp-category")<<" readback=PASS\n";
    }
    if(osk_range(memory,s.field,0x34))memory.store32(s.field+0x2c,confirmed?2:1);
    memory.store32(s.params+dialog_common::kResultOffset,confirmed?0:1);
    osk_status(memory,dialog_status::kQuit);
    if(!confirmed)std::cout<<"[osk-native] request="<<s.request<<" cancelled_or_error=1 output_unchanged=1\n";
}
void register_osk(HleRegistrar& hle){
    hle.add("sceUtility","sceUtilityOskInitStart",[](Runtime& rt,AllegrexContext& ctx){
        auto& s=native_osk();auto& memory=rt.memory();const auto params=arg(ctx,0);
        if(s.status!=dialog_status::kNone){kernel().finish(ctx,kErrorUtilityInvalidStatus);return;}
        if(!osk_range(memory,params,0x3c)||memory.load32(params)<0x3c){kernel().finish(ctx,error::kIllegalArgument);return;}
        const auto count=memory.load32(params+0x30),field=memory.load32(params+0x34);
        if(count!=1||!osk_range(memory,field,0x34)){kernel().finish(ctx,error::kIllegalArgument);return;}
        const auto capacity=memory.load32(field+0x24),output=memory.load32(field+0x28),limit=memory.load32(field+0x30);
        if(capacity<2||capacity>513||!osk_range(memory,output,2*capacity)||osk_overlap(output,2*capacity,params,0x3c)||
           osk_overlap(output,2*capacity,field,0x34)||osk_overlap(params,0x3c,field,0x34)){
            kernel().finish(ctx,error::kIllegalArgument);return;
        }
        yakumo::text_input::Request request;
        if(!osk_string(memory,memory.load32(field+0x20),request.initial)||!osk_string(memory,memory.load32(field+0x1c),request.prompt)){
            kernel().finish(ctx,error::kIllegalArgument);return;
        }
        const bool default_title=request.prompt.empty();
        request.title=default_title?"Enter text":request.prompt;
        if(request.prompt==request.title)request.prompt.clear();
        const auto type=memory.load32(field+0x10),lines=memory.load32(field+0x14);
        const bool name=yakumo::game_text_policy::configure(request,capacity,limit,type,lines);
        if(name&&default_title)request.title="Enter a name";
        std::u16string initial_units;yakumo::text_input::decode_utf8(request.initial,initial_units);
        std::uint32_t rejected{};
        for(const auto c:initial_units)if(request.allowed&&!request.allowed(c)){rejected=c;break;}
        std::cout<<"[osk-native] initial_utf16_units="<<initial_units.size()<<" first_rejected_unit="<<psprecomp::hex32(rejected)<<'\n';
        s=NativeOsk{params,field,output,capacity,limit};
        s.type=type;s.lines=lines;s.name_profile=name;
        osk_status(memory,dialog_status::kInit);
        yakumo::game_text_observer::background_requested=true;
        const auto submitted=yakumo::text_input::service().submit(std::move(request));s.request=submitted.id;
        yakumo::game_text_observer::request=s.request;
        yakumo::game_text_observer::opened_ms=yakumo::game_platform::milliseconds();
        if(submitted.failure)s.immediate=yakumo::text_input::Result{0,yakumo::text_input::Outcome::Error,{},*submitted.failure};
        std::cout<<"[osk-native] InitStart request="<<s.request<<" capacity_utf16="<<capacity<<" limit_characters="<<limit
                 <<" input_type="<<psprecomp::hex32(type)<<" lines="<<lines<<" language="<<memory.load32(field+8)
                 <<" profile="<<(name?"legacy-name":"psp-category-text")
                 <<" description="<<psprecomp::hex32(memory.load32(field+0x1c))<<" caller="<<psprecomp::hex32(ctx.gpr[31])
                 <<" fixed_setting_ignored=1\n";
        kernel().finish(ctx,0);
    });
    hle.add("sceUtility","sceUtilityOskUpdate",[](Runtime& rt,AllegrexContext& ctx){
        yakumo::text_input::service().tick();
        if(native_osk().status==dialog_status::kVisible)osk_deliver(rt.memory());
        kernel().finish(ctx,0);
    });
    hle.add("sceUtility","sceUtilityOskGetStatus",[](Runtime& rt,AllegrexContext& ctx){
        yakumo::text_input::service().tick();auto& s=native_osk();const auto reported=s.status;
        if(reported==dialog_status::kInit)osk_status(rt.memory(),dialog_status::kVisible);
        else if(reported==dialog_status::kVisible)osk_deliver(rt.memory());
        else if(reported==dialog_status::kFinished){osk_status(rt.memory(),dialog_status::kNone);s=NativeOsk{};yakumo::game_text_observer::background_requested=false;}
        kernel().finish(ctx,reported);
    });
    hle.add("sceUtility","sceUtilityOskShutdownStart",[](Runtime& rt,AllegrexContext& ctx){
        if(native_osk().status!=dialog_status::kQuit){kernel().finish(ctx,kErrorUtilityInvalidStatus);return;}
        osk_status(rt.memory(),dialog_status::kFinished);kernel().finish(ctx,0);
    });
}
