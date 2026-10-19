// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_log.hpp"
#include <deque>
#include <iostream>
#include <mutex>
#include <utility>

namespace yakumo::adhoc_log {
namespace {
struct Queue {
    std::mutex mutex;
    std::deque<std::string> trace, important;
    std::uint64_t suppressed{};
};
Queue& queue() { static auto* value=new Queue; return *value; }
void write(const std::vector<std::string>& records) {
    for(const auto& line:records) std::cout.write(line.data(),static_cast<std::streamsize>(line.size()));
}
}
void enqueue(std::string_view line,bool important) {
    auto& state=queue();
    std::lock_guard lock(state.mutex);
    auto& pending=important ? state.important : state.trace;
    if(pending.size()==kCapacity) { pending.pop_front(); ++state.suppressed; }
    std::string bounded(line.substr(0,kMaxRecord));
    bounded+='\n';
    pending.push_back(std::move(bounded));
}
std::vector<std::string> take(std::size_t trace_budget,std::size_t important_budget) {
    auto& state=queue();
    std::vector<std::string> records;
    {
        std::lock_guard lock(state.mutex);
        for(auto [pending,budget]:{std::pair{&state.important,important_budget}, {&state.trace,trace_budget}}) {
            while(budget-- && !pending->empty()) {
                records.push_back(std::move(pending->front()));
                pending->pop_front();
            }
        }
    }
    return records;
}
Stats stats() {
    auto& state=queue();
    std::lock_guard lock(state.mutex);
    return {state.trace.size()+state.important.size(),state.suppressed};
}
void pump(std::uint64_t now_ms) {
    static std::uint64_t next{};
    if(now_ms<next) return;
    next=now_ms+1000;
    write(take(16,16));
}
void finish() { write(take(kCapacity,kCapacity)); }
}
