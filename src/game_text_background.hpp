// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <optional>

namespace yakumo::game_text_background {
struct Pixels {
    std::uint64_t count{},nonblack{},sum_rgb{},hash{14695981039346656037ull};
    double nonblack_percent()const{return count?100.0*nonblack/count:0;}
    double mean_rgb()const{return count?static_cast<double>(sum_rgb)/(3*count):0;}
};
inline std::optional<Pixels> measure(std::span<const std::uint8_t> rgba,std::uint32_t width,std::uint32_t height){
    if(!width||!height||static_cast<std::uint64_t>(width)*height>rgba.size()/4||
       static_cast<std::uint64_t>(width)*height*4!=rgba.size())return {};
    Pixels out;out.count=static_cast<std::uint64_t>(width)*height;
    for(std::size_t i=0;i<rgba.size();i+=4){
        out.nonblack+=rgba[i]>8||rgba[i+1]>8||rgba[i+2]>8;
        for(unsigned j=0;j<3;++j){out.sum_rgb+=rgba[i+j];out.hash=(out.hash^rgba[i+j])*1099511628211ull;}
    }
    return out;
}
// Capture the displayed picture before a confirm/cancel press reaches the
// guest, which can clear its framebuffer before calling OskInitStart. Track
// mapped PSP buttons so custom native bindings keep working. No work per frame.
class CaptureEdge {
    std::uint32_t previous_{};
public:
    bool sample(std::uint32_t buttons,bool blocked){
        const bool rising=((buttons&~previous_)&0x6000u)!=0;previous_=buttons;
        return rising&&!blocked;
    }
};
enum class Phase { Open, Closed };
// Only closing dialogs probe live pixels. Reopening cancels recovery without
// touching the retained image. A genuinely black scene must not stay frozen.
class Recovery {
    bool active_{};
    std::uint64_t started_{},first_frame_{},next_{};
public:
    bool active()const{return active_;}
    void begin(std::uint64_t now,std::uint64_t frame){
        if(active_)return;
        active_=true;started_=now;first_frame_=frame;next_=now+100;
    }
    void cancel(){active_=false;}
    bool expired(std::uint64_t now)const{return active_&&now-started_>=1500;}
    bool due(std::uint64_t now,std::uint64_t frame){
        if(!active_||now<next_||frame-first_frame_<2)return false;
        next_=now+100;return true;
    }
};
struct SamplePoint{std::uint32_t x,y;};
inline SamplePoint sample_point(std::uint32_t index,std::uint32_t width,std::uint32_t height){
    return {static_cast<std::uint32_t>((2ull*(index%8)+1)*width/16),
            static_cast<std::uint32_t>((2ull*(index/8)+1)*height/16)};
}
class Probe {
    std::uint64_t request_{},opened_{};bool seen_open_{},seen_closed_{};
public:
    void begin(std::uint64_t id,std::uint64_t now){request_=id;opened_=now;seen_open_=seen_closed_=false;}
    std::optional<Phase> due(bool active,std::uint64_t now){
        if(!request_||request_>16||seen_closed_)return {};
        if(active&&!seen_open_&&now>=opened_&&now-opened_>=1000){seen_open_=true;return Phase::Open;}
        if(!active){seen_closed_=true;return Phase::Closed;}
        return {};
    }
};
}
