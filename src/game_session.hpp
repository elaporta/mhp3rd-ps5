// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <charconv>
#include <stdexcept>
#include <string_view>
namespace yakumo::game_session {
inline unsigned qa_seconds(std::string_view text) {
    unsigned value{};
    const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
    if(error!=std::errc{} || end!=text.data()+text.size() || (value!=0 && (value<10 || value>4200)))
        throw std::runtime_error("Invalid ps5.qa_seconds: use 0 or 10..4200 seconds");
    return value;
}
inline bool successful_stop(std::string_view reason, bool checkpoint, bool bounded) {
    if(checkpoint) return reason=="First-frame regression checkpoint (150 guest frames)" ||
        reason=="First-frame checkpoint (150 guest frames; codecs pending)";
    return (bounded && reason=="Bounded menu session completed (navigation unverified)") ||
        reason=="guest called sceKernelExitGame" || reason=="window closed" || reason=="quit from the menu";
}
// Cleanup has real descriptor progress of its own; the guest deadline must
// not misclassify a progressing metadata pass as stalled guest execution.
inline const char* timeout(std::uint64_t now,std::uint64_t heartbeat,std::uint64_t guest_deadline,
                           std::uint64_t cleanup_started){
    if(now>=heartbeat&&now-heartbeat>30000)return "no progress for thirty seconds";
    if(cleanup_started){
        if(now>=cleanup_started&&now-cleanup_started>120000)return "cleanup exceeded 120 seconds";
    }else if(guest_deadline && now>guest_deadline&&now-guest_deadline>15000)return "guest deadline exceeded";
    return nullptr;
}
}
