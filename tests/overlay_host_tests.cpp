// SPDX-License-Identifier: GPL-3.0-or-later
#include "overlay_probe.hpp"
#include "runtime_probe.hpp"
#include <cstdio>
#include <dlfcn.h>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Two explicit synthetic DSO paths required");
        using namespace yakumo::ps5;
        Log log;
        if (!overlay_probe(log)) throw std::runtime_error("Static synthetic corpus failed");
        if (overlay_probe(log, true)) throw std::runtime_error("Incorrect independent oracle was accepted");
        RuntimeProbeResult rejected{true, true, true, false};
        if (runtime_overall_pass(rejected, true, true, true)) throw std::runtime_error("Overlay failure reached runtime PASS");
        SyntheticOverlayModule modules[2]{};
        // Match the desktop RTLD_NOW/LOCAL and process-lifetime mapping policy.
        for (unsigned i = 0; i < 2; ++i) {
            void* handle = dlopen(argv[i + 1], RTLD_NOW | RTLD_LOCAL);
            if (!handle) throw std::runtime_error(dlerror());
            modules[i].info = reinterpret_cast<decltype(modules[i].info)>(dlsym(handle, "mhp3rd_overlay_info"));
            modules[i].install = reinterpret_cast<decltype(modules[i].install)>(dlsym(handle, "mhp3rd_register_overlay"));
            if (!modules[i].info || !modules[i].install) throw std::runtime_error("Original overlay ABI exports absent");
        }
        if (!overlay_probe(log, false, modules)) throw std::runtime_error("Desktop synthetic DSO corpus failed");
        const auto correct_install = modules[1].install;
        modules[1].install = modules[0].install;
        if (overlay_probe(log, false, modules)) throw std::runtime_error("Wrong replacement registry accepted");
        modules[1].install = correct_install;
        auto incorrect = *modules[1].info(); incorrect.hash ^= 1;
        static mhp3rd::OverlayModuleInfo bad;
        bad = incorrect;
        modules[1].info = +[]() -> const mhp3rd::OverlayModuleInfo* { return &bad; };
        if (overlay_probe(log, false, modules)) throw std::runtime_error("Incorrect image hash accepted");
        std::puts("[host-overlays] PASS static and desktop DSO corpora; bad oracle/hash and aggregate failure rejected");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "[host-overlays] FAIL %s\n", error.what()); return 1; }
}
