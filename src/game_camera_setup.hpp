// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "camera/game_aspect.hpp"
#include "camera/game_camera.hpp"
#include "psprecomp/runtime.hpp"
#include "native_camera_original.hpp"

namespace yakumo {
// The verified NPJB-40001 helper address is documented in game_camera.cpp.
// The original registration avoids depending on generated unit partitions.
// Both upstream signature checks run before either feature writes guest state.
inline bool prepare_native_camera(psprecomp::Runtime& runtime) {
    const auto original=native_camera_original();
    const bool aspect=mhp3rd::camera::prepare_game_aspect(runtime);
    const bool camera=mhp3rd::camera::prepare_game_camera(runtime,original);
    return aspect&&camera;
}
}
