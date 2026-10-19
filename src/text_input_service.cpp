// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_input_service.hpp"
#include <cstring>
#include <iostream>
#include <utility>

namespace yakumo::text_input {
namespace {
bool scalars(std::string_view text, const std::function<bool(char32_t)>& accept) {
    for (std::size_t i=0;i<text.size();) {
        const auto first=static_cast<unsigned char>(text[i++]);
        char32_t c{}; unsigned extra{}; char32_t minimum{};
        if(first<0x80){c=first;extra=0;minimum=0;}
        else if(first>=0xc2&&first<=0xdf){c=first&31;extra=1;minimum=0x80;}
        else if(first>=0xe0&&first<=0xef){c=first&15;extra=2;minimum=0x800;}
        else if(first>=0xf0&&first<=0xf4){c=first&7;extra=3;minimum=0x10000;}
        else return false;
        if(extra>text.size()-i)return false;
        while(extra--){const auto b=static_cast<unsigned char>(text[i++]);if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&63);}
        if(!c||c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff)||!accept(c))return false;
    }
    return true;
}
}
bool decode_utf8(std::string_view text,std::u16string& out) {
    std::u16string value;
    if(!scalars(text,[&](char32_t c){
        if(c<0x10000)value.push_back(static_cast<char16_t>(c));
        else {c-=0x10000;value.push_back(static_cast<char16_t>(0xd800+(c>>10)));value.push_back(static_cast<char16_t>(0xdc00+(c&1023)));}
        return true;
    }))return false;
    out=std::move(value);return true;
}
bool decode_utf16(std::u16string_view text,std::string& out) {
    std::string value;
    for(std::size_t i=0;i<text.size();++i){
        char32_t c=text[i];
        if(!c)return false;
        if(c>=0xd800&&c<=0xdbff){
            if(i+1==text.size()||text[i+1]<0xdc00||text[i+1]>0xdfff)return false;
            c=0x10000+((c-0xd800)<<10)+(text[++i]-0xdc00);
        }else if(c>=0xdc00&&c<=0xdfff)return false;
        if(c<0x80)value.push_back(static_cast<char>(c));
        else if(c<0x800){value.push_back(static_cast<char>(0xc0|(c>>6)));value.push_back(static_cast<char>(0x80|(c&63)));}
        else if(c<0x10000){value.push_back(static_cast<char>(0xe0|(c>>12)));value.push_back(static_cast<char>(0x80|((c>>6)&63)));value.push_back(static_cast<char>(0x80|(c&63)));}
        else{value.push_back(static_cast<char>(0xf0|(c>>18)));value.push_back(static_cast<char>(0x80|((c>>12)&63)));value.push_back(static_cast<char>(0x80|((c>>6)&63)));value.push_back(static_cast<char>(0x80|(c&63)));}
    }
    out=std::move(value);return true;
}
bool validate(const Request& request,std::string_view text){
    if((text.empty()&&!request.allow_empty)||text.size()>request.max_utf8_bytes)return false;
    std::size_t count{},units{};
    return scalars(text,[&](char32_t c){++count;units+=c>=0x10000?2:1;return c!=U'\r'&&(c!=U'\n'||request.multiline)&&count<=request.max_characters&&units<=request.max_utf16_units&&(!request.allowed||request.allowed(c));});
}
bool hunter_name_character(char32_t c){
    return (c>=U'a'&&c<=U'z')||(c>=U'A'&&c<=U'Z')||(c>=U'0'&&c<=U'9')||
        (c&&c<0x80&&std::strchr(" !#$&'()+,-./:;=?@_~",static_cast<int>(c)));
}
Submission Service::submit(Request request){
    if(active_||result_||backend_->active()||drain_)return {0,Failure{"busy",-1}};
    std::u16string title,prompt;
    auto initial_rules=request;initial_rules.allow_empty=true;
    if(!request.max_characters||!request.max_utf16_units||!request.max_utf8_bytes||
       !validate(initial_rules,request.initial)||!decode_utf8(request.title,title)||!decode_utf8(request.prompt,prompt))
        return {0,Failure{"request-validation",-1}};
    request_=std::move(request);active_=++next_;drain_=true;cancelling_=false;
    const auto id=active_;
    if(auto error=backend_->open(request_)){
        result_=Result{id,Outcome::Error,{},std::move(*error)};active_=0;
    }
    std::cout<<"[text-input] request="<<id<<" opened="<<backend_->active()<<"\n";
    return {id,{}};
}
void Service::tick(){
    if(!backend_->active())return;
    auto result=backend_->poll();
    if(!result||!active_)return;
    result->id=active_;
    if(cancelling_&&result->outcome==Outcome::Confirmed)result->outcome=Outcome::Cancelled;
    if(result->outcome==Outcome::Confirmed&&!validate(request_,result->text)){
        result->outcome=Outcome::Error;result->failure={"result-validation",-1};
    }
    if(result->outcome!=Outcome::Confirmed)result->text.clear();
    std::cout<<"[text-input] request="<<active_<<" outcome="<<static_cast<int>(result->outcome)
             <<" operation="<<result->failure.operation<<" rc="<<result->failure.code<<"\n";
    result_=std::move(result);active_=0;
}
std::optional<Result> Service::take(std::uint64_t id){
    if(!result_||result_->id!=id)return {};
    auto value=std::move(result_);result_.reset();return value;
}
void Service::cancel(std::uint64_t id){if(active_&&active_==id){cancelling_=true;backend_->cancel();}}
void Service::shutdown(){if(active_)cancel(active_);tick();}
bool Service::blocks_input() const{return drain_||backend_->active()||active_;}
bool Service::suppress_sample(bool neutral){
    if(!blocks_input())return false;
    if(!active_&&!backend_->active()&&neutral)drain_=false;
    return true;
}
bool Service::set_backend(Backend& backend){if(active_||result_||backend_->active()||drain_)return false;backend_=&backend;return true;}
} // namespace yakumo::text_input
