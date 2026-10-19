// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "probe.hpp"
#include "ge_scene.hpp"
#include <array>
#include <cstdint>
namespace yakumo::ps5 {
enum class ProfileFault { None, Hle, Texture, Geometry, Crypto, Files, Cleanup, Omit };
bool profile_probe(const TitlePaths&,Log&,ProfileFault = ProfileFault::None);
struct ProfileResult {bool portable{},sdk{};};
ProfileResult protected_profile_probe(const TitlePaths&,Log&);
bool profile_graphics_scene(ge::Scene&,std::array<std::uint32_t,4>&,Log&);
bool profile_native_errors(Log&);
}
