// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_input_ps5.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

#if YAKUMO_NATIVE
#if YAKUMO_IME_HOST_WEAK_FIXTURE
// Linux-only absence fixture. Native PS5 imports must use STB_GLOBAL: r3's
// STB_WEAK entries remained empty on hardware despite successful module load.
#define YAKUMO_IME_BINDING __attribute__((weak))
#else
#define YAKUMO_IME_BINDING
#endif
extern "C" {
int sceSysmoduleLoadModule(std::uint16_t);
int sceImeDialogInit(yakumo::text_input::ImeParam*,void*) YAKUMO_IME_BINDING;
int sceImeDialogGetStatus() YAKUMO_IME_BINDING;
int sceImeDialogGetResult(yakumo::text_input::ImeResult*) YAKUMO_IME_BINDING;
int sceImeDialogTerm() YAKUMO_IME_BINDING;
int sceImeDialogAbort() YAKUMO_IME_BINDING;
}
#undef YAKUMO_IME_BINDING
#endif
namespace yakumo::text_input {
std::optional<Failure> checked_ime_imports(ImeApi& api,const ImeApi& imported){
    const bool present[]={imported.init!=nullptr,imported.status!=nullptr,imported.result!=nullptr,imported.term!=nullptr,imported.abort!=nullptr};
    const char* names[]={"sceImeDialogInit","sceImeDialogGetStatus","sceImeDialogGetResult","sceImeDialogTerm","sceImeDialogAbort"};
    std::optional<Failure> failure;
    for(unsigned i=0;i<5;++i){
        std::cout<<"[ps5-ime] checked_import="<<names[i]<<" present="<<present[i]<<"\n";
        if(!present[i]&&!failure)failure=Failure{std::string("unbound-")+names[i],-1};
    }
    if(failure)return failure; // Never publish a partial API table.
    api=imported;return {};
}
namespace {
#if YAKUMO_NATIVE
// Read the actual GOT values, including zero. Volatile prevents the compiler
// from treating a strong function declaration as evidence that it is nonnull.
// The converted library retains its existing loose-import attributes (9).
// Native disassembly/ELF audits verify all five data loads and relocations.
extern "C" __attribute__((noinline,visibility("hidden"))) void yakumo_read_native_ime_imports(ImeApi* out){
    decltype(ImeApi::init) volatile init=sceImeDialogInit;
    decltype(ImeApi::status) volatile status=sceImeDialogGetStatus;
    decltype(ImeApi::result) volatile result=sceImeDialogGetResult;
    decltype(ImeApi::term) volatile term=sceImeDialogTerm;
    decltype(ImeApi::abort) volatile abort=sceImeDialogAbort;
    *out={init,status,result,term,abort};
}
#endif
std::optional<Failure> resolve_native(ImeApi& api){
#if YAKUMO_NATIVE
    const int load=sceSysmoduleLoadModule(0x0096);
    std::cout<<"[ps5-ime] sysmodule=0x0096 load_rc="<<load<<" resolution=checked-global-got-r4\n"<<std::flush;
    // Read bindings after the request, including on retry. No static cache of
    // absent bindings and no call through a PLT entry whose target is zero.
    ImeApi imported{};yakumo_read_native_ime_imports(&imported);
    if(auto failure=checked_ime_imports(api,imported)){
        if(load<0)return Failure{"sceSysmoduleLoadModule",load};
        return failure;
    }
    return {};
#else
    (void)api;return Failure{"ps5-ime-unavailable-on-host",-1};
#endif
}
Ps5Backend& native_backend(){static Ps5Backend backend(resolve_native);return backend;}
}
Service& service(){static Service instance(native_backend());return instance;}
void initialize_ps5(int user){native_backend().user(user);}
std::optional<Failure> Ps5Backend::open(const Request& request){
    if(running_)return Failure{"ime-busy",-1};
    if(user_<0)return Failure{"ime-user",user_};
    std::u16string initial,title,prompt;
    if(request.max_utf16_units>512||!decode_utf8(request.initial,initial)||!decode_utf8(request.title,title)||
       !decode_utf8(request.prompt,prompt)||title.size()>128||prompt.size()>128||initial.size()>request.max_utf16_units)
        return Failure{"ime-request-bounds",-1};
    if(!api_.init){if(auto error=resolve_(api_))return error;}
    if(!api_.init||!api_.status||!api_.result||!api_.term||!api_.abort)return Failure{"ime-incomplete-api",-1};
    buffer_.fill(0);title_.fill(0);prompt_.fill(0);
    std::copy(initial.begin(),initial.end(),buffer_.begin());std::copy(title.begin(),title.end(),title_.begin());std::copy(prompt.begin(),prompt.end(),prompt_.begin());
    param_=ImeParam{};param_.user=user_;param_.type=static_cast<std::uint32_t>(request.keyboard);
    if(request.multiline)param_.options|=1; // Public IME MULTILINE option.
    param_.max_length=static_cast<std::uint32_t>(request.max_utf16_units);
    param_.buffer=buffer_.data();param_.title=title_.data();param_.placeholder=prompt_.data();
    const int rc=api_.init(&param_,nullptr);
    std::cout<<"[ps5-ime] init_rc="<<rc<<" user="<<user_<<" max_utf16="<<param_.max_length
             <<" keyboard_type="<<param_.type<<" options="<<param_.options<<" abi=public-96 baseline=r4-basic-hardware-verified\n";
    if(rc!=0)return Failure{"sceImeDialogInit",rc};
    running_=true;closing_=cancelled_=false;close_attempts_=cancel_polls_=0;return {};
}
void Ps5Backend::cancel(){
    if(!running_||closing_)return;
    cancelled_=true;const int rc=api_.abort();
    std::cout<<"[ps5-ime] abort_rc="<<rc<<"\n";
    if(rc){pending_={0,Outcome::Error,{},Failure{"sceImeDialogAbort",rc}};closing_=true;}
}
std::optional<Result> Ps5Backend::poll(){
    if(!running_)return {};
    if(!closing_){
        const int status=api_.status();
        if(status==1){
            if(!cancelled_||++cancel_polls_<120)return {};
            pending_={0,Outcome::Error,{},Failure{"ime-abort-timeout",-1}};closing_=true;
        }
        if(!closing_){
        std::cout<<"[ps5-ime] terminal_status="<<status<<"\n";
        pending_={0,Outcome::Error,{},Failure{"sceImeDialogGetStatus",status}};
        if(status==2){
            // Supplied reference revisions disagree about trailing result size.
            // Protect adjacent storage and reject writes beyond the declared ABI.
            struct ResultStorage { ImeResult value{}; std::array<unsigned char,64> guard; } storage;
            storage.guard.fill(0xa5);
            auto& result=storage.value;const int rc=api_.result(&result);
            std::cout<<"[ps5-ime] result_rc="<<rc<<" end="<<result.end<<"\n";
            if(!std::all_of(storage.guard.begin(),storage.guard.end(),[](auto b){return b==0xa5;}))pending_.failure={"ime-result-abi-overwrite",-1};
            else if(rc)pending_.failure={"sceImeDialogGetResult",rc};
            else if(result.end==1||cancelled_)pending_={0,Outcome::Cancelled,{},{}};
            else if(result.end==0){
                const auto end=std::find(buffer_.begin(),buffer_.begin()+param_.max_length+1,char16_t{});
                if(end==buffer_.begin()+param_.max_length+1)pending_.failure={"ime-missing-terminator",-1};
                else if(!decode_utf16(std::u16string_view(buffer_.data(),end-buffer_.begin()),pending_.text))pending_.failure={"ime-invalid-utf16",-1};
                else{pending_.outcome=Outcome::Confirmed;pending_.failure={};}
            }else pending_.failure={"ime-end-status",static_cast<int>(result.end)};
        }else if(status==0){running_=false;return std::move(pending_);}
        else{const int rc=api_.abort();std::cout<<"[ps5-ime] error_abort_rc="<<rc<<"\n";}
        closing_=true;
        }
    }
    const int rc=api_.term();
    if(rc){
        if(close_attempts_++==0)std::cout<<"[ps5-ime] term_rc="<<rc<<" retaining_buffers=1\n";
        pending_={0,Outcome::Error,{},Failure{"sceImeDialogTerm",rc}};
        if(close_attempts_>=120)throw std::runtime_error("PS5 IME cleanup failed after 120 bounded polls; buffers retained until title exit");
        return {};
    }
    std::cout<<"[ps5-ime] term_rc=0\n";
    running_=closing_=false;buffer_.fill(0);return std::move(pending_);
}
} // namespace yakumo::text_input
