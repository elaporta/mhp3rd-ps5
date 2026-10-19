// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_probe.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

int main() {
    char fixture[] = "/tmp/yakumo-runtime-XXXXXX";
    const char* root = mkdtemp(fixture);
    if (!root) return 2;
    int status = 0;
    try {
        yakumo::ps5::TitlePaths paths;
        std::snprintf(paths.cache, sizeof(paths.cache), "%s", root);
        yakumo::ps5::Log log;
        using namespace yakumo::ps5;
        const auto normal = runtime_probe(paths, log);
        if (!runtime_overall_pass(normal, true, true, true)) throw std::runtime_error("Normal real-runtime suite failed");
        for (unsigned bits = 0; bits < 8; ++bits) {
            if (runtime_overall_pass(normal, bits & 1, bits & 2, bits & 4) != (bits == 7))
                throw std::runtime_error("Platform/GE failure was hidden by runtime PASS");
        }
        for (const auto options : {RuntimeProbeOptions{"interpreter", false, false},
                                    RuntimeProbeOptions{nullptr, true, false}, RuntimeProbeOptions{nullptr, false, true}}) {
            const auto rejected = runtime_probe(paths, log, options);
            if (runtime_overall_pass(rejected, true, true, true)) throw std::runtime_error("Injected failure reached PASS");
            if (options.corrupt_case && rejected.framework) throw std::runtime_error("Wrong oracle was accepted");
            if (options.fail_sdk && rejected.sdk_services) throw std::runtime_error("SDK failure was accepted");
            if (options.fail_cleanup && rejected.cleanup) throw std::runtime_error("Cleanup failure was accepted");
        }
        // Unexpected user-owned files must survive normal and failed runs.
        const auto personal = std::filesystem::path(root) / "runtime-probe/preserve.txt";
        { auto* file = std::fopen(personal.c_str(), "wb"); if (!file) throw std::runtime_error("Fixture failed");
          std::fputs("preserve", file); std::fclose(file); }
        RuntimeProbeOptions options{nullptr, false, true};
        (void)runtime_probe(paths, log, options);
        if (std::filesystem::file_size(personal) != 8) throw std::runtime_error("Personal file was changed");
        for (const auto& entry : std::filesystem::directory_iterator(personal.parent_path()))
            if (entry.path() != personal) throw std::runtime_error("Synthetic file cleanup was incomplete");
        // A symlink replacing the probe directory must be refused before writes.
        std::filesystem::remove(personal);
        std::filesystem::remove(personal.parent_path());
        std::filesystem::create_directory_symlink(root, personal.parent_path());
        const auto denied = runtime_probe(paths, log);
        if (denied.sdk_services || denied.passed()) throw std::runtime_error("Symlink directory was accepted");
        std::puts("[host-runtime] PASS; incorrect oracle and SDK/cleanup failures rejected; host evidence only");
    } catch (const std::exception& error) { std::fprintf(stderr, "[host-runtime] FAIL %s\n", error.what()); status = 1; }
    std::filesystem::remove_all(root);
    return status;
}
