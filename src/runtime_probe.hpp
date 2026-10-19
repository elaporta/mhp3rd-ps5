// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "probe.hpp"

namespace yakumo::ps5 {
struct RuntimeProbeOptions {
    // Host-only injected failures prove that aggregation never hides a failed case.
    const char* corrupt_case{};
    bool fail_sdk{};
    bool fail_cleanup{};
};
struct RuntimeProbeResult {
    bool framework{};
    bool sdk_services{};
    bool cleanup{};
    // Optional suites default to neutral only when the build excludes them.
    bool overlays{true};
    bool passed() const { return framework && sdk_services && cleanup && overlays; }
};
RuntimeProbeResult runtime_probe(const TitlePaths&, Log&, RuntimeProbeOptions = {});
RuntimeProbeResult protected_runtime_probe(const TitlePaths&, Log&);
bool runtime_services(const TitlePaths&, Log&, const RuntimeProbeOptions&, bool& cleanup);
bool runtime_overall_pass(const RuntimeProbeResult&, bool storage, bool devices, bool graphics);
}
