// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace yakumo::game_observer {
inline std::atomic<std::uint64_t> log_us{},pad_us{},log_lines{},pad_samples{};
struct Timer {
    using Clock=std::chrono::steady_clock;
    std::atomic<std::uint64_t>& duration;
    Clock::time_point start=Clock::now();
    ~Timer(){duration+=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();}
};
}
