// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "integration.hpp"
#include "ge_scene.hpp"
namespace yakumo::ps5 {
inline ge::Markers integration_markers(const IntegrationSnapshot& s) {
    ge::Markers m;
    constexpr std::uint32_t palette[]{0xff0000ff,0xff00ff00,0xffff0000};
    m.x[0]=8+s.guest%180; m.x[1]=240+unsigned(s.x+127)*200/254;
    m.color[0]=palette[s.guest%3];m.color[1]=s.connected?0xff00ffff:0xffff00ff;
    // Pixel expectations derive from the instruction contract, independently of
    // the guest output used to produce the draw and vertex buffer.
    const unsigned input=s.sequence+(s.buttons&0xffff)+unsigned(s.x+127);
    const unsigned oracle=((input+s.module+3)^(s.module*17+5))+1013;
    const unsigned channel=oracle%3;
    m.expected[0]={12+oracle%180,264,channel==0?255:0,channel==1?255:0,channel==2?255:0,"guest-state"};
    m.expected[1]={m.x[1]+4,264,255,s.connected?255:0,s.connected?0:255,"controls"};
    return m;
}
}
