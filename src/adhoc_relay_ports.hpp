// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace yakumo::adhoc_relay_ports {
// PPSSPP 1.20.2 defaults to offset 10000, including AemuPostoffice.
// Only relay records are translated; HLE and room-service ports stay unchanged.
inline constexpr std::uint16_t kOffset = 10000;
constexpr std::uint16_t to_wire(std::uint16_t port) {
    const auto shifted = static_cast<std::uint16_t>(static_cast<unsigned>(port) + kOffset);
    return port == 0 ? 0 : shifted == 0 ? 65535 : shifted;
}
constexpr std::uint16_t from_wire(std::uint16_t port) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(port) - kOffset);
}
}
