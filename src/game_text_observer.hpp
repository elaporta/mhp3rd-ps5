// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <cstdint>
#include <utility>
namespace yakumo::game_text_observer {
// Owned exclusively by the guest worker; never a direct write to character RAM.
inline std::string expected, notice;
inline bool matched{}, mismatch_logged{}, background_requested{};
inline std::uint64_t request{},opened_ms{};
inline void confirmed(std::string text){expected=std::move(text);matched=mismatch_logged=false;}
}
