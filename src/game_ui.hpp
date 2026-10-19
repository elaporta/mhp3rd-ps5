// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ui/ui.hpp"
#include <iostream>
#include <stdexcept>
// Capability boundary for the optional desktop menu, not a PSP HLE service.
// No key binding can request this menu on the native first-frame target.
namespace mhp3rd::ui {
inline bool attach(gpu::VulkanRenderer&) { std::cout << "[game-ui] Desktop menu unavailable; game rendering enabled\n"; return false; }
inline void draw_over_game() {}
inline bool menu_requested() { return false; }
inline bool menu_over_game() { return false; }
inline bool menu_pauses() { return false; }
inline bool take_quit_request() { return false; }
inline bool backup_reminder_due() { return false; }
inline void set_lock_on_marker(std::optional<std::array<float,2>>) {}
inline void show_note(const std::string& text) { std::cout << "[game-note] " << text << '\n'; }
inline std::string take_screenshot() { return {}; }
inline bool run_menu() { throw std::runtime_error("Desktop menu unavailable"); }
inline bool run_backup_reminder() { throw std::runtime_error("Desktop backup dialog unavailable"); }
inline void open_menu_over_game() { throw std::runtime_error("Desktop menu unavailable"); }
}
