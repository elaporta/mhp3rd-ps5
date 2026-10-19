// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "game_files.hpp"
#include "game_observer.hpp"
#include "game_audio.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

// Renderer/summary callbacks run on the guest worker. Decoder counters alone
// cross threads. These are host API timings, never physical scanout timestamps.
namespace yakumo::game_performance {
inline std::string cache_profile{"performance-r2"};
inline double capped_rate(double requested, double refresh) {
    const auto cap=std::isfinite(refresh)&&refresh>=1.0?std::min(60.0,refresh):60.0;
    return std::min(requested,cap);
}
inline yakumo::game_files::path cache_directory() {
    return yakumo::game_files::root()/"yakumo/cache"/cache_profile;
}
inline bool valid_cache_profile(const std::string& value) {
    return !value.empty() && value.size()<=32 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_")==std::string::npos;
}
inline void require_upload_finished(int result) {
    if (result!=0) throw std::runtime_error("Replacement upload fence failed rc="+std::to_string(result));
}
inline std::uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int log_printf(const char* format, ...) {
    char line[4096];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0) std::cout.write(line, std::min<std::size_t>(length, sizeof(line)-1));
    return length;
}
inline std::atomic<std::uint64_t> decoded{}, failed{}, decoded_bytes{}, decode_us{};
inline std::uint64_t submitted{}, blended{}, blocked{}, skipped{}, uploaded{}, evicted{}, resident{}, resident_count{};
inline std::uint64_t last_present_us{}, window_start_us{};
inline std::vector<std::uint64_t> intervals;
inline bool interval_overflow{};
inline void note_present() {
    const auto now = now_us();
    if (!window_start_us) window_start_us = now;
    if (last_present_us) {
        if (intervals.size() < 1024) intervals.push_back(now-last_present_us);
        else interval_overflow = true;
    }
    last_present_us = now;
    ++submitted;
}
inline bool allow_failure_log() { return failed.fetch_add(1) < 16; }
template<class Summary> inline void summary(const Summary& s) {
    std::ostringstream line;
    const auto end = now_us();
    line << "[ps5-perf] second=" << s.second << " start_us=" << window_start_us << " end_us=" << end
         << " submitted=" << submitted << " blended=" << blended << " skipped=" << skipped << " blocked=" << blocked
         << " speed=" << s.speed << " game_fps=" << s.game_fps << " requested=" << s.requested_rate
         << " actual=" << s.frame_rate << " refresh_hz=" << s.refresh_hz << " guest_ms=" << s.guest_ms
         << " render_ms=" << s.render_ms << " wait_ms=" << s.wait_ms << " gpu_ms=";
    if (s.gpu_valid) line << s.gpu_avg_ms; else line << "UNAVAILABLE";
    line << " resident_bytes=" << resident << " resident_count=" << resident_count
         << " observer_log_us=" << game_observer::log_us.exchange(0)
         << " observer_log_lines=" << game_observer::log_lines.exchange(0)
         << " observer_pad_us=" << game_observer::pad_us.exchange(0)
         << " observer_pad_samples=" << game_observer::pad_samples.exchange(0)
         << " decoded=" << decoded.load() << " decoded_bytes=" << decoded_bytes.load()
         << " decode_us=" << decode_us.load() << " failed=" << failed.load()
         << " uploaded=" << uploaded << " evicted=" << evicted
         << " interval_overflow=" << interval_overflow << " intervals_us=";
    for (std::size_t i=0; i<intervals.size(); ++i) { if(i) line << ','; line << intervals[i]; }
    line << " scanout=UNAVAILABLE rss=UNAVAILABLE\n";
    std::cout << line.str();
    static std::uint64_t last_audio_report_us{};
    const auto audio=game_audio::telemetry.snapshot();
    if(audio.valid && audio.output_blocks && (!last_audio_report_us || end-last_audio_report_us>=10'000'000)) {
        game_audio::write_levels(std::cout,audio,"periodic");last_audio_report_us=end;
    }
    submitted=blended=skipped=blocked=0;
    intervals.clear(); interval_overflow=false; window_start_us=end;
}
} // namespace yakumo::game_performance
