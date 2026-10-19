// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
namespace yakumo::game_web {
inline std::string quote(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
inline std::map<std::string, std::string> form(std::string_view body) {
    auto decode = [](std::string_view s) {
        std::string out;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw std::runtime_error("Invalid form escape");
        };
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%') {
                if (i + 2 >= s.size()) throw std::runtime_error("Truncated form escape");
                const int high = hex(s[i+1]), low = hex(s[i+2]);
                out += char(high * 16 + low); i += 2;
            } else out += s[i] == '+' ? ' ' : s[i];
        }
        if (out.find('\0') != out.npos) throw std::runtime_error("NUL in form");
        return out;
    };
    std::map<std::string, std::string> result;
    while (!body.empty()) {
        const auto end = body.find('&');
        const auto item = body.substr(0, end);
        const auto split = item.find('=');
        if (split == item.npos || !result.emplace(decode(item.substr(0,split)),decode(item.substr(split+1))).second)
            throw std::runtime_error("Invalid or duplicate form field");
        if (end == body.npos) break;
        body.remove_prefix(end + 1);
    }
    return result;
}
// Platform-independent browser/input handoff state.
struct Capture {
    bool active{}, release{}, ready{};
    std::uint64_t ping{};
    bool sample(bool chord, bool neutral, bool busy, std::uint64_t now, bool& launch, bool analog_active = false) {
        launch = false;
        const bool edge = chord && ready;
        if (!chord) ready = true;
        if (edge) ready = false;
        if (busy) { ready = false; return active || release; }
        if (active) {
            // PS5 may close its browser without delivering pagehide or Circle
            // to the game. Once heartbeats stop, the first game input starts
            // the handoff and is swallowed until released to avoid a phantom action.
            if (now > ping && now - ping > 3000) {
                if (!neutral) { active = false; release = true; }
                else if (analog_active) { active = false; release = false; return false; }
            }
            return true;
        }
        if (release) { if (neutral) release = false; return true; }
        if (edge) { active = true; ping = now; launch = true; return true; }
        return false;
    }
    void close() { active = false; release = true; ready = false; }
};
}
