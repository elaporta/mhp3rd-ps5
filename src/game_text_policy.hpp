// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "text_input_service.hpp"
#include <algorithm>

namespace yakumo::game_text_policy {
inline std::string escaped_for_log(std::string_view text){
    std::string out;
    for(char c:text){
        if(c=='\n')out+="\\n";
        else{if(c=='"'||c=='\\')out+='\\';out+=c;}
    }
    return out;
}
// Public PSP OSK category bits (PSPSDK). Japanese Latin categories use
// fullwidth code points. ASCII from a native Latin keyboard is an explicit
// width alias only where the corresponding PSP Latin category is disabled.
inline bool name_shape(std::uint32_t capacity,std::uint32_t limit,std::uint32_t type,std::uint32_t lines){
    return capacity==13&&limit<=12&&type==0xdf0f&&lines<=1;
}
inline unsigned latin_category(char32_t c){
    if(c>=U'0'&&c<=U'9')return 1;
    if(c>=U'a'&&c<=U'z')return 4;
    if(c>=U'A'&&c<=U'Z')return 8;
    return c>=0x20&&c<=0x7e?2:0;
}
inline char32_t guest_character(std::uint32_t type,char32_t c){
    const auto category=latin_category(c);
    if(type&&category&&!(type&category)&&(type&(category<<8)))
        return c==U' '?0x3000:c+0xfee0;
    return c;
}
inline bool psp_character(std::uint32_t type,bool multiline,char32_t c){
    if(c==U'\n')return multiline;
    const auto mask=type?type:0xfffffu;
    c=guest_character(type,c);
    if(const auto category=latin_category(c))return (mask&category)||(mask&0x80000u);
    if(c>=0xff01&&c<=0xff5e)return mask&(latin_category(c-0xfee0)<<8);
    if(c==0x3005)return mask&(0x8000|0x200);
    if(c==0x3000||(c>=0x3001&&c<=0x303f)||c==0x30fb||c==0x30fc||
       c==0x00d7||c==0x00f7||(c>=0x2010&&c<=0x2026)||c==0x203b||
       (c>=0x2190&&c<=0x2199)||(c>=0x25a0&&c<=0x25ef)||c==0x2605||c==0x2606||c==0x266a)return mask&0x200;
    if(c>=0x3041&&c<=0x3096)return mask&0x1000;
    if(c>=0x30a1&&c<=0x30fa)return mask&0x4000;
    if(c>=0xff61&&c<=0xff9f)return mask&0x2000;
    if((c>=0x3400&&c<=0x4dbf)||(c>=0x4e00&&c<=0x9fff))return mask&0x8000;
    if((c>=0x430&&c<=0x44f)||c==0x451)return mask&0x10000;
    if((c>=0x410&&c<=0x42f)||c==0x401)return mask&0x20000;
    if(c>=0xac00&&c<=0xd7a3)return mask&0x40000;
    return false; // No controls, unpaired surrogates, emoji or guessed glyph fallback.
}
inline bool encode_guest(std::string_view text,std::uint32_t type,bool name,std::u16string& units){
    std::u16string encoded;
    if(!text_input::decode_utf8(text,encoded))return false;
    if(!name)for(auto& c:encoded)c=static_cast<char16_t>(guest_character(type,c));
    units=std::move(encoded);return true;
}
inline bool configure(text_input::Request& request,std::uint32_t capacity,std::uint32_t limit,
                      std::uint32_t type,std::uint32_t lines){
    const bool name=name_shape(capacity,limit,type,lines);
    request.max_characters=limit?std::min(limit,capacity-1):capacity-1;
    request.max_utf16_units=capacity-1;request.max_utf8_bytes=4*(capacity-1);
    request.allow_empty=!name;request.multiline=!name&&lines>1;
    request.keyboard=!name&&(type==0||(type&0x7ff00))?text_input::KeyboardKind::Text:text_input::KeyboardKind::BasicLatin;
    request.allowed=name?std::function<bool(char32_t)>(text_input::hunter_name_character):
        std::function<bool(char32_t)>([type,multiline=request.multiline](char32_t c){return psp_character(type,multiline,c);});
    return name;
}
}
