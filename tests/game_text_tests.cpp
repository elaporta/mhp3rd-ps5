// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_text_policy.hpp"
#include "game_text_background.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace yakumo;
void check(bool ok){if(!ok)throw std::runtime_error("text field/background assertion failed");}
int main(){
    text_input::Request r;
    check(game_text_policy::escaped_for_log("a\"\\\nb")=="a\\\"\\\\\\nb");
    check(game_text_policy::configure(r,13,0,0xdf0f,1));
    check(!r.allow_empty&&!r.multiline&&r.max_characters==12&&text_input::validate(r,"Yakumo27")&&!text_input::validate(r,"%"));
    check(!game_text_policy::configure(r,65,48,0xf,1));
    check(r.allow_empty&&r.max_utf16_units==64&&r.max_characters==48&&text_input::validate(r,"100% ready [OK]")&&text_input::validate(r,""));
    check(!text_input::validate(r,std::string(49,'a'))&&!text_input::validate(r,"a\nb")&&!text_input::validate(r,"a\rb"));
    game_text_policy::configure(r,65,0,0,3);
    check(r.multiline&&text_input::validate(r,"a\nb")&&!text_input::validate(r,"a\rb"));
    for(const auto& pair:std::array<std::pair<unsigned,std::string>,4>{{{1,"0129"},{2," %[]"},{4,"az"},{8,"AZ"}}}){
        game_text_policy::configure(r,65,0,pair.first,1);check(text_input::validate(r,pair.second));
        for(char c:std::string("0%aA"))check(r.allowed(c)==(pair.first==(c=='0'?1u:c=='%'?2u:c=='a'?4u:8u)));
    }
    game_text_policy::configure(r,65,0,0x80000,1);check(text_input::validate(r,"https://example.invalid/a?q=1%20"));
    check(!r.allowed(0)&&!r.allowed(0x7f)&&!r.allowed(0xe9)&&!r.allowed(0x10000));
    // Actual r5 hardware Guild Card request: Japanese categories, 45 units.
    game_text_policy::configure(r,45,0,0xdf00,1);
    std::string japanese;check(text_input::decode_utf16(u"初めまして！よろしくお願いします。",japanese));
    check(r.keyboard==text_input::KeyboardKind::Text&&r.max_characters==44&&text_input::validate(r,japanese));
    check(text_input::validate(r,"Guild card 27!")&&text_input::validate(r,std::string(44,'a'))&&!text_input::validate(r,std::string(45,'a')));
    check(!text_input::validate(r,"\xf0\x9f\x98\x80")&&!r.allowed(0xff71)&&!r.allowed(0xd800));
    std::u16string units;
    check(game_text_policy::encode_guest("Guild 27!",0xdf00,false,units)&&units==u"Ｇｕｉｌｄ　２７！");
    check(game_text_policy::encode_guest(japanese,0xdf00,false,units)&&units==u"初めまして！よろしくお願いします。");
    check(game_text_policy::encode_guest("a 27!",0xdf0f,false,units)&&units==u"a 27!");
    check(game_text_policy::encode_guest("Yakumo27",0xdf0f,true,units)&&units==u"Yakumo27");
    check(!game_text_policy::encode_guest("\xc0\x80",0xdf00,false,units)&&units==u"Yakumo27");
    for(const auto& pair:std::array<std::pair<unsigned,char32_t>,12>{{{0x100,U'１'},{0x200,U'！'},{0x400,U'ａ'},{0x800,U'Ａ'},
        {0x1000,U'あ'},{0x2000,U'ｱ'},{0x4000,U'ア'},{0x8000,U'狩'},{0x10000,U'я'},{0x20000,U'Я'},{0x40000,U'한'},{0x200,U'ー'}}}){
        check(game_text_policy::psp_character(pair.first,false,pair.second));
        check(!game_text_policy::psp_character(1,false,pair.second));
    }
    using namespace game_text_background;
    CaptureEdge edge;check(!edge.sample(0,false)&&edge.sample(0x4000,false)&&!edge.sample(0x4000,false));
    check(!edge.sample(0,false)&&edge.sample(0x2000,false)&&!edge.sample(0,false));
    check(!edge.sample(0x4000,true)&&!edge.sample(0x4000,false)&&!edge.sample(0,false)&&!edge.sample(0x10,false));
    std::array<std::uint8_t,8> image{0,0,0,255,255,255,255,0};
    const auto stats=measure(image,2,1);check(stats&&stats->count==2&&stats->nonblack==1&&stats->mean_rgb()==127.5&&stats->nonblack_percent()==50);
    const auto hash=stats->hash;image[3]=0;check(measure(image,2,1)->hash==hash); // Alpha cannot masquerade as scene colour.
    check(!measure(image,0,1)&&!measure(image,1,1)&&!measure(image,0xffffffff,0xffffffff));
    Pixels empty;check(empty.mean_rgb()==0&&empty.nonblack_percent()==0);
    Probe probe;check(!probe.due(true,5000));probe.begin(1,100);
    check(!probe.due(true,99)&&!probe.due(true,1099)&&probe.due(true,1100)==Phase::Open&&!probe.due(true,1200));
    check(probe.due(false,1300)==Phase::Closed&&!probe.due(false,1400));
    probe.begin(2,2000);check(probe.due(false,2001)==Phase::Closed&&!probe.due(true,4000));
    probe.begin(17,0);check(!probe.due(true,99999)&&!probe.due(false,99999));
    game_text_background::Recovery recovery;
    check(!recovery.active()&&!recovery.due(0,0)&&!recovery.expired(0));
    recovery.begin(100,10);recovery.begin(200,12);
    check(recovery.active()&&!recovery.due(199,12)&&!recovery.due(200,11));
    check(recovery.due(200,12)&&!recovery.due(250,13)&&recovery.due(300,14));
    check(!recovery.expired(1599)&&recovery.expired(1600));recovery.cancel();
    check(!recovery.active()&&!recovery.expired(2000));
    for(unsigned i=0;i<64;++i){
        const auto point=game_text_background::sample_point(i,1920,1088);
        check(point.x<1920&&point.y<1088);
        const auto tiny=game_text_background::sample_point(i,1,1);check(tiny.x==0&&tiny.y==0);
    }
    std::cout<<"field categories/limits/empty/multiline and bounded background probe PASS\n";
}
