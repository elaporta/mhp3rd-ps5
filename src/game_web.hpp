// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
namespace yakumo::game_web {
// Lifecycle/tick and controller sampling belong to the guest owner thread.
void start();
// Optional menu failure must never prevent the game or online session booting.
void initialize() noexcept;
void tick(std::uint64_t now);
void stop() noexcept;
bool suppress_sample(bool chord, bool circle, bool neutral, bool ime_busy, std::uint64_t now,
                     bool analog_active = false);
bool blocks_input();
std::string browser_url();
}
