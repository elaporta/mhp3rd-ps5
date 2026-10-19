// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yakumo::adhoc_log {
inline constexpr std::size_t kCapacity=64, kMaxRecord=2048;
struct Stats { std::size_t pending{}; std::uint64_t suppressed{}; };
// Producers never touch an output stream. Each priority has its own fixed cap.
void enqueue(std::string_view line, bool important);
std::vector<std::string> take(std::size_t trace_budget, std::size_t important_budget);
Stats stats();
// Owner-thread only, outside the ad hoc transport mutex. No forced stream flush.
void pump(std::uint64_t now_ms);
void finish();
}
