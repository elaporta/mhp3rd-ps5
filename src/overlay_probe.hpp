// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "probe.hpp"
#include "overlay_module.hpp"
namespace psprecomp { class Runtime; }
namespace yakumo::ps5 {
struct SyntheticOverlayModule {
    const mhp3rd::OverlayModuleInfo* (*info)();
    void (*install)(psprecomp::Runtime&);
};
bool overlay_probe(Log&, bool corrupt_result = false, const SyntheticOverlayModule* modules = nullptr);
}
