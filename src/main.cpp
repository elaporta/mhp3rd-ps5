// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#if YAKUMO_WITH_RUNTIME
#include "runtime_probe.hpp"
#endif
#if YAKUMO_WITH_INTEGRATION
#include "integration.hpp"
#endif

#if YAKUMO_WITH_PROFILE
#include "profile_probe.hpp"
#endif
#ifndef YAKUMO_TITLE_ID
#define YAKUMO_TITLE_ID "PPSA99373"
#endif
#ifndef YAKUMO_BUILD_ID
#define YAKUMO_BUILD_ID "development"
#endif
int main(int argc, char** argv) {
    using namespace yakumo::ps5;
    TitlePaths paths;
    Log log;
#if YAKUMO_WITH_INTEGRATION
    if(!integration_boot_watch_start(paths,log)) return 5;
#endif
    log.write("[boot] Yakumo-PS5 probe build=%s title=%s", YAKUMO_BUILD_ID, YAKUMO_TITLE_ID);
    log.write("[boot] SDK=b83202be73e930050e00de0f1b4d9ec46c0391bf Vulkan=252bf912a745e3566a93b69b098bdc8ec4048837");
    char executable_dir[path_capacity]{};
    if (argc > 0 && argv && argv[0] && argv[0][0] == '/' && std::strlen(argv[0]) < sizeof(executable_dir)) {
        std::snprintf(executable_dir, sizeof(executable_dir), "%s", argv[0]);
        char* slash = std::strrchr(executable_dir, '/');
        if (slash) *slash = '\0';
    }
    // Prefer the explicitly documented installation directory over its /app0
    // mount alias. Every candidate still needs both identity checks; a write
    // failure on the selected root never triggers a fallback to another root.
    const char* candidates[] = {"/data/homebrew/" YAKUMO_TITLE_ID, executable_dir, "/app0"};
    char app0_api[256]{}, data_api[256]{};
    filesystem_diagnostics("/app0", "APP0", app0_api, sizeof(app0_api), log);
    filesystem_diagnostics(candidates[0], "DATA", data_api, sizeof(data_api), log);
    if (!paths.resolve(candidates, 3, YAKUMO_TITLE_ID, &log)) {
        log.write("[boot] FAIL no verified title root; details above in klog");
        char notice[2600];
        std::snprintf(notice, sizeof(notice), "Yakumo %s: ROOT FAILED\n%s\n%s\n%.1800s",
                      YAKUMO_BUILD_ID, app0_api, data_api, paths.diagnostics);
        notify_user(notice);
        sleep(5);
        return 2;
    }
    // Attach before preparing game/save/cache folders, so later failures are on disk.
    bool attached = false;
    if (paths.prepare_logs(&log)) {
        attached = log.attach(paths);
        if (!attached) paths.fail("open-or-flush-log", paths.logs, errno, &log);
    }
    if (!attached || !paths.prepare(&log)) {
        char notice[2600];
        std::snprintf(notice, sizeof(notice), "Yakumo %s: STORAGE FAILED\n%s", YAKUMO_BUILD_ID, paths.diagnostics);
        log.write("[boot] %s", notice);
        notify_user(notice);
        sleep(5);
        return 3;
    }
    log.write("[paths] verified root=%s", paths.root);
#if YAKUMO_WITH_INTEGRATION
    integration_boot_watch_ready(paths);
#endif
    const bool storage = storage_probe(paths, log);
    const bool devices = input_audio_probe(log);
#if YAKUMO_WITH_RUNTIME
    const auto runtime_result = protected_runtime_probe(paths, log);
#endif
#if YAKUMO_WITH_VULKAN
    const bool graphics = graphics_probe(paths, log);
#else
    const bool graphics = false;
    log.write("[vulkan] NOT_BUILT explicit platform-only diagnostic; not a graphics pass");
#endif
#if YAKUMO_WITH_RUNTIME
    const bool baseline_runtime = runtime_overall_pass(runtime_result, storage, devices, graphics);
#if YAKUMO_WITH_PROFILE
    const auto profile=protected_profile_probe(paths,log);
    const bool inherited_runtime=baseline_runtime&&profile.portable&&profile.sdk;
    log.write("[result-profile] inherited=%s profile=%s sdk_negative=%s",baseline_runtime?"PASS":"FAIL",profile.portable?"PASS":"FAIL",profile.sdk?"PASS":"FAIL");
#else
    const bool inherited_runtime=baseline_runtime;
#endif
#if YAKUMO_WITH_INTEGRATION
    const bool integration=integration_probe(paths,log) && inherited_runtime;
    const bool runtime=inherited_runtime && integration;
    integration_boot_watch_stop(log);
    log.write("[result-integration] inherited_runtime=%s integration=%s",inherited_runtime?"PASS":"FAIL",integration?"PASS":"FAIL");
#else
    const bool runtime=inherited_runtime;
#endif
#if YAKUMO_WITH_OVERLAYS
    log.write("[result-overlays] overlays=%s; runtime includes this required suite",
              runtime_result.overlays ? "PASS" : "FAIL");
#endif
    log.write("[result] storage=%s device_api=%s graphics=%s framework=%s sdk_services=%s runtime=%s; manual observation required",
              storage ? "PASS" : "FAIL", devices ? "PASS" : "FAIL", graphics ? "PASS" : "NOT_PASSED",
              runtime_result.framework ? "PASS" : "FAIL", runtime_result.sdk_services ? "PASS" : "FAIL",
              runtime ? "PASS" : "FAIL");
#else
    log.write("[result] storage=%s device_api=%s graphics=%s; manual observation required",
              storage ? "PASS" : "FAIL", devices ? "PASS" : "FAIL", graphics ? "PASS" : "NOT_PASSED");
#endif
    log.write("[exit] probe finished; no game code was loaded");
    notify_user("Yakumo " YAKUMO_BUILD_ID ": tests finished. Retrieve yakumo/logs/probe.log for results.");
#if YAKUMO_WITH_RUNTIME
#if YAKUMO_WITH_INTEGRATION
    return integration ? 0 : 4;
#else
    return runtime ? 0 : 4;
#endif
#else
    return storage && devices && (!YAKUMO_WITH_VULKAN || graphics) ? 0 : 4;
#endif
}
