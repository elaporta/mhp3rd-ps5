// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace yakumo::game_network {
void initialize();
void tick(std::uint64_t now_ms);
void shutdown() noexcept;
}
