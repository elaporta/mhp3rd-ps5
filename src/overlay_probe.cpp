// SPDX-License-Identifier: GPL-3.0-or-later
#include "overlay_probe.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <stdexcept>

extern "C" {
const mhp3rd::OverlayModuleInfo* synthetic_a_overlay_info();
const mhp3rd::OverlayModuleInfo* synthetic_b_overlay_info();
void synthetic_a_register_overlay(psprecomp::Runtime&);
void synthetic_b_register_overlay(psprecomp::Runtime&);
}
namespace yakumo::ps5 {
namespace {
#include "overlay_images.inc"
constexpr std::uint32_t slot = 0x08900000u, entry = slot + 64, unique = slot + 92;
constexpr std::uint32_t import_pc = 0x08806000u, exit_pc = 0x08806004u, scratch = 0x08807000u;
void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
std::uint64_t identity(const unsigned char* bytes, std::size_t size) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    return hash;
}
void stop(psprecomp::Runtime& rt, psprecomp::AllegrexContext&) { rt.stop("synthetic overlay completed"); }
void host_import(psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
    const auto token = psprecomp::capture_runtime_execution_context();
    const auto returned = ctx.gpr[31];
    rt.invoke_import_cached(23, "SyntheticHost", 0x01234567u, ctx);
    if (!rt.stopped() && psprecomp::runtime_execution_context_matches(token)) ctx.pc = returned;
}
}
bool overlay_probe(Log& log, bool corrupt_result, const SyntheticOverlayModule* supplied) {
    log.write("[overlays] BEGIN two public synthetic corpora; native mode=static-registration-experiment; no dynamic loader claim");
    const SyntheticOverlayModule built[]{{synthetic_a_overlay_info, synthetic_a_register_overlay},
                                        {synthetic_b_overlay_info, synthetic_b_register_overlay}};
    const auto* modules = supplied ? supplied : built;
    bool passed = false;
    try {
        psprecomp::Runtime runtime;
        unsigned import_calls = 0;
        runtime.register_hle("SyntheticHost", 0x01234567u, [&](auto&, auto& cpu) {
            ++import_calls; cpu.gpr[2] = cpu.gpr[4] + 1000u;
        });
        runtime.register_function(import_pc, host_import, "synthetic_host_import");
        runtime.register_function(exit_pc, stop, "synthetic_overlay_completion");
        const std::size_t host_count = runtime.function_count();
        log.write("[overlays] memory guest_ram=%zu edram=%zu slot_image=128 bytes; static host code remains mapped; process RSS unmeasured",
                  runtime.memory().size(), runtime.memory().vram_size());
        for (unsigned i = 0; i < 2; ++i) {
            const auto* info = modules[i].info();
            const auto* image = i ? image_b : image_a;
            check(info && info->abi_version == mhp3rd::kOverlayAbiVersion && info->base == slot && info->size == 128 &&
                  info->code_size == (i ? 28u : 40u) && info->name, "Overlay ABI/layout mismatch");
            check(info->hash == identity(image, 64 + info->code_size), "Overlay identity hash mismatch");
            log.write("[overlays] metadata=%s abi=%u slot=%#x size=%u code=%u hash=%016llx PASS", info->name,
                      info->abi_version, info->base, info->size, info->code_size, static_cast<unsigned long long>(info->hash));
        }
        const auto execute = [&](std::uint32_t pc, std::uint32_t expected, unsigned tag) {
            runtime.cpu() = {};
            runtime.cpu().gpr[4] = 7;
            runtime.cpu().gpr[5] = scratch;
            runtime.cpu().gpr[31] = exit_pc;
            runtime.memory().store32(scratch, 0);
            runtime.run(pc, 32);
            if (corrupt_result) runtime.cpu().gpr[2] ^= 1;
            check(runtime.stop_reason() == "synthetic overlay completed" && runtime.cpu().gpr[2] == expected &&
                  runtime.cpu().gpr[3] == tag && runtime.memory().load32(scratch) == expected,
                  "Compiled overlay/host-import independent oracle mismatch");
        };
        psprecomp::set_interpreter_fallback_enabled(true);
        psprecomp::reset_interpreter_stats();
        const auto started = std::chrono::steady_clock::now();
        std::size_t maximum_registrations = host_count;
        // A -> B -> A repeated: the same slot must never keep A's extra entry in B.
        for (unsigned round = 0; round < 100; ++round) {
            const unsigned which = round % 2;
            runtime.unregister_functions(slot, slot + 128);
            check(!runtime.has_function(entry) && !runtime.has_function(unique), "Stale slot registration survived removal");
            psprecomp::AllegrexContext isolated{};
            check(!runtime.invoke_isolated_aot(entry, isolated), "Removed direct lookup still executes stale code");
            runtime.memory().copy_in(slot, std::span<const unsigned char>(which ? image_b : image_a, 128));
            modules[which].install(runtime);
            check(runtime.has_function(entry) && (runtime.has_function(unique) == (which == 0)), "Replacement registry mismatch");
            maximum_registrations = std::max(maximum_registrations, runtime.function_count());
            execute(entry, which ? 1029u : 1018u, which ? 202u : 101u);
            check(runtime.has_function(import_pc) && runtime.has_function(exit_pc), "Slot removal erased host functions");
            if (round < 3 || round == 99) log.write("[overlays] round=%u module=%s v0=%u v1=%u expected=%u/%u imports=%u PASS",
                round + 1, which ? "B" : "A", runtime.cpu().gpr[2], runtime.cpu().gpr[3],
                which ? 1029 : 1018, which ? 202 : 101, import_calls);
        }
        check(import_calls == 100 && psprecomp::interpreter_stats().instructions == 0,
              "Compiled corpus silently used interpreter or lost imports");
        log.write("[overlays] compiled-only instructions=0 interpreter_entries=0 host_imports=100 PASS");
        check(maximum_registrations <= host_count + 32, "Registrations grew during repeated replacement");
        // Unknown patched image: remove the old registry before executing new bytes.
        runtime.unregister_functions(slot, slot + 128);
        runtime.memory().copy_in(slot, std::span<const unsigned char>(image_b, 128));
        runtime.memory().store32(entry + 4, 0x24840021u); // addiu a0,a0,33
        runtime.memory().store32(entry + 24, 0x2403012Fu); // addiu v1,zero,303
        check(identity(runtime.memory().raw_pointer(slot, 92), 92) != modules[1].info()->hash,
              "Modified image unexpectedly matched the old identity");
        psprecomp::reset_interpreter_stats();
        execute(entry, 1040, 303);
        const auto stats = psprecomp::interpreter_stats();
        check(stats.instructions == 7 && stats.entries == 2 && import_calls == 101,
              "Unknown overlay fallback did not execute its real instructions/import");
        log.write("[overlays] unmatched-image interpreter instructions=%llu entries=%llu v0=1040 v1=303 imports=101 PASS",
                  static_cast<unsigned long long>(stats.instructions), static_cast<unsigned long long>(stats.entries));
        runtime.unregister_functions(slot, slot + 128);
        modules[0].install(runtime);
        runtime.memory().copy_in(slot, std::span<const unsigned char>(image_a, 128));
        // Isolated A-only compiled entry matches its independent MIPS result 99.
        runtime.cpu() = {}; runtime.cpu().gpr[31] = exit_pc;
        runtime.run(unique, 4);
        check(runtime.cpu().gpr[2] == 99 && runtime.stop_reason() == "synthetic overlay completed", "A-only entry mismatch");
        runtime.unregister_functions(slot, slot + 128);
        check(runtime.function_count() == host_count && !runtime.has_function(unique), "Overlay cleanup leaked registrations");
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        log.write("[overlays] PASS replacements=100 host_imports=101 registered_peak=%zu registered_final=%zu guest_image_bytes=128 elapsed_us=%lld cleanup=PASS",
                  maximum_registrations, runtime.function_count(), static_cast<long long>(micros));
        passed = true;
    } catch (const std::exception& error) { log.write("[overlays] FAIL %s", error.what()); }
    catch (...) { log.write("[overlays] FAIL unknown exception"); }
    return passed;
}
}
