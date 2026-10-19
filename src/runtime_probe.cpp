// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_probe.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <streambuf>
#include <stdexcept>
#if YAKUMO_WITH_OVERLAYS
#include "overlay_probe.hpp"
#endif

namespace yakumo::ps5 {
namespace {
constexpr std::uint32_t base = 0x08804000u;
constexpr std::uint32_t scratch = base + 0x1000u;
constexpr std::uint32_t finish_pc = base + 0x100u;
void require(bool condition, const char* diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}
template<class F> void expect_error(F operation, const char* text) {
    bool caught = false;
    try { operation(); }
    catch (const psprecomp::Error& error) {
        caught = std::strstr(error.what(), text) != nullptr;
        std::cerr << "[expected-error] " << error.what() << "\n";
    }
    require(caught, "Required framework exception or diagnostic was absent");
}
// Redirect real upstream diagnostic streams without growing an unbounded buffer.
class DiagnosticBuffer final : public std::streambuf {
public:
    explicit DiagnosticBuffer(Log& log) : log_(log) {}
    ~DiagnosticBuffer() override { sync(); }
protected:
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char c = traits_type::to_char_type(value);
        if (c == '\n' || used_ == line_.size() - 1) flush();
        if (c != '\n') line_[used_++] = c;
        return value;
    }
    // cerr has unitbuf set; sync() after each insertion must not split a line.
    int sync() override { return 0; }
private:
    void flush() {
        if (!used_) return;
        line_[used_] = '\0';
        log_.write("[framework-diagnostic] %s", line_.data());
        used_ = 0;
    }
    Log& log_;
    std::array<char, 1536> line_{};
    std::size_t used_{};
};
class Capture final {
public:
    explicit Capture(Log& log) : buffer_(log), out_(std::cout.rdbuf(&buffer_)), err_(std::cerr.rdbuf(&buffer_)) {}
    ~Capture() { std::cout.rdbuf(out_); std::cerr.rdbuf(err_); }
private:
    DiagnosticBuffer buffer_;
    std::streambuf *out_, *err_;
};
void next(psprecomp::Runtime&, psprecomp::AllegrexContext& cpu) { ++cpu.gpr[2]; cpu.pc += 4; }
void completed(psprecomp::Runtime& runtime, psprecomp::AllegrexContext&) { runtime.stop("synthetic completed"); }
void memory_fault(psprecomp::Runtime& runtime, psprecomp::AllegrexContext&) {
    (void)runtime.memory().load32(0x0A000000u);
}
void same_pc(psprecomp::Runtime&, psprecomp::AllegrexContext&) {}
void memory_contract() {
    psprecomp::GuestMemory memory;
    require(memory.size() == 32u * 1024u * 1024u && memory.vram_size() == 2u * 1024u * 1024u,
            "Default RAM/EDRAM allocation size mismatch");
    memory.store32(scratch, 0x12345678u);
    require(memory.load32(scratch | 0x80000000u) == 0x12345678u &&
            memory.aot_load32(scratch | 0x40000000u) == 0x12345678u, "RAM alias mismatch");
    memory.store32(0x04000000u, 0xABCDEF12u);
    require(memory.load32(0x44200000u) == 0xABCDEF12u, "EDRAM mirror mismatch");
    constexpr auto end = psprecomp::GuestMemory::kPhysicalBase + 32u * 1024u * 1024u;
    memory.store32(end - 4, 0xC0FFEE12u);
    memory.store8(psprecomp::GuestMemory::kPhysicalBase, 0x5Au);
    require(memory.load32(end - 4) == 0xC0FFEE12u && memory.load8(psprecomp::GuestMemory::kPhysicalBase) == 0x5Au,
            "Valid RAM endpoints mismatch");
    require(!memory.contains(end, 1) && !memory.contains(end - 3, 4) &&
            !memory.raw_pointer(end - 3, 4) && !memory.raw_pointer(0xFFFFFFFFu, 8), "Invalid range accepted");
    expect_error([&] { memory.store32(end - 3, 0); }, "outside PSP RAM/EDRAM");
    expect_error([&] { (void)memory.aot_load32(end - 3); }, "outside PSP RAM/EDRAM");
    expect_error([&] { (void)memory.load8(0); }, "outside PSP RAM/EDRAM");
    require(memory.load32(end - 4) == 0xC0FFEE12u && memory.load32(scratch) == 0x12345678u,
            "Rejected memory access changed valid bytes");
    auto view = memory.aot_fast_view();
    view.aot_store16(scratch, 0x9876u);
    require(view.aot_load16(scratch) == 0x9876u && memory.load32(scratch) == 0x12349876u,
            "AOT fast memory view mismatch");
}
void registration_contract() {
    psprecomp::Runtime runtime;
    runtime.register_function(base, next, "synthetic_next");
    runtime.register_function(base + 4, completed, "synthetic_completed");
    require(runtime.function_count() == 2 && runtime.has_function(base) && !runtime.has_function(base + 2),
            "Function registration mismatch");
    runtime.run(base, 4);
    require(runtime.cpu().gpr[2] == 1 && runtime.stop_reason() == "synthetic completed", "Dispatch mismatch");
    runtime.unregister_functions(base, base + 4);
    require(!runtime.has_function(base) && runtime.has_function(base + 4) && runtime.function_count() == 1,
            "Half-open unregister range mismatch");
    runtime.register_function(base, next, "synthetic_replacement");
    psprecomp::AllegrexContext isolated{};
    isolated.pc = base;
    require(runtime.invoke_isolated_aot(base, isolated) && isolated.gpr[2] == 1, "Replacement dispatch mismatch");
    expect_error([&] { runtime.register_function(base, nullptr, "invalid"); }, "null recompiled function");
}
void imports_contract() {
    psprecomp::Runtime runtime;
    unsigned calls = 0;
    runtime.register_hle("synthetic", 0x10203040u, [&](auto& rt, auto& cpu) {
        ++calls;
        cpu.gpr[2] = rt.memory().load32(scratch) + cpu.gpr[4];
    });
    runtime.memory().store32(scratch, 35);
    runtime.cpu().gpr[4] = 7;
    runtime.invoke_import("synthetic", 0x10203040u, runtime.cpu());
    runtime.invoke_import_cached(17, "synthetic", 0x10203040u, runtime.cpu());
    runtime.invoke_import_cached(17, "synthetic", 0x10203040u, runtime.cpu());
    require(calls == 3 && runtime.cpu().gpr[2] == 42 && !runtime.stopped(), "Real HLE/cache dispatch mismatch");
    runtime.invoke_import("synthetic_missing", 0x1234u, runtime.cpu());
    require(runtime.stopped() && runtime.stop_reason().find("Missing HLE import synthetic_missing::") == 0,
            "Missing HLE import was not rejected");
    std::cerr << "[expected-stop] " << runtime.stop_reason() << "\n";
}
void interpreter_contract(bool corrupt) {
    psprecomp::Runtime runtime;
    // Independent oracle: 5 + 7 = 12; taken branch executes its delay slot,
    // skips addiu v0,99, then jr ra executes addiu v1,3 in its delay slot.
    constexpr std::array<std::uint32_t, 9> program{
        0x24080005u, 0x25090007u, 0xAC890000u, 0x8C820000u,
        0x10490002u, 0x24030002u, 0x24020063u, 0x03E00008u, 0x24630003u};
    for (std::size_t n = 0; n < program.size(); ++n) runtime.memory().store32(base + 4u * n, program[n]);
    runtime.register_function(finish_pc, completed, "synthetic_interpreter_return");
    runtime.cpu().gpr[4] = scratch;
    runtime.cpu().gpr[31] = finish_pc;
    psprecomp::set_interpreter_fallback_enabled(true);
    psprecomp::reset_interpreter_stats();
    runtime.run(base, 4);
    if (corrupt) runtime.cpu().gpr[2] ^= 1u;
    const auto stats = psprecomp::interpreter_stats();
    require(runtime.cpu().gpr[2] == 12 && runtime.cpu().gpr[3] == 5 && runtime.cpu().gpr[0] == 0 &&
            runtime.memory().load32(scratch) == 12 && runtime.stop_reason() == "synthetic completed",
            "Interpreter arithmetic/memory/branch/delay-slot oracle mismatch (expected v0=12 v1=5)");
    require(stats.instructions == 8 && stats.entries == 1 && stats.distinct_entry_addresses == 1,
            "Interpreter execution statistics mismatch (expected 8/1/1)");
}
void budgets_contract() {
    psprecomp::Runtime runtime;
    runtime.memory().store32(base, 0x1000FFFFu); // beq zero,zero,-1
    runtime.memory().store32(base + 4, 0);
    psprecomp::AllegrexContext cpu{};
    cpu.pc = base;
    require(psprecomp::interpret_allegrex(runtime, cpu, 10) == psprecomp::InterpreterExit::Budget && cpu.pc == base,
            "Interpreter instruction budget mismatch");
    psprecomp::set_interpreter_fallback_enabled(false);
    try { runtime.run(base, 3); }
    catch (...) { psprecomp::set_interpreter_fallback_enabled(true); throw; }
    psprecomp::set_interpreter_fallback_enabled(true);
    require(runtime.stop_reason() == "No recompiled function registered at 0x08804000",
            "Disabled fallback did not produce strict dispatch stop");
    psprecomp::Runtime capped;
    capped.register_function(base, same_pc, "synthetic_loop");
    capped.run(base, 3);
    require(capped.stopped() && capped.stop_reason() == "Dispatch limit reached at 0x08804000",
            "Dispatch budget mismatch");
}
void errors_contract() {
    psprecomp::Runtime runtime;
    runtime.register_function(base, memory_fault, "synthetic_memory_fault");
    bool rejected = false;
    try { runtime.run(base, 3); }
    catch (const psprecomp::Error& error) {
        const std::string text(error.what());
        rejected = text.find("outside PSP RAM/EDRAM") != std::string::npos &&
            text.find("synthetic_memory_fault") != std::string::npos &&
            text.find("dispatch=0x08804000") != std::string::npos && text.find("gpr:") != std::string::npos;
        std::cerr << "[expected-error] " << text << "\n";
    }
    require(rejected, "Guest memory exception lost dispatch identity/context");
    psprecomp::Runtime invalid;
    invalid.memory().store32(base, 0x4C000000u); // Reserved primary opcode 0x13.
    invalid.run(base, 2);
    require(invalid.stopped() && invalid.stop_reason().find("Unsupported Allegrex instruction") == 0,
            "Invalid instruction did not stop with diagnostic");
    std::cerr << "[expected-stop] " << invalid.stop_reason() << "\n";
}
}
RuntimeProbeResult runtime_probe(const TitlePaths& paths, Log& log, RuntimeProbeOptions options) {
    RuntimeProbeResult result{true, false, false};
    log.write("[runtime] Yakumo=5b10e6687bca05ec34d9f40f7cb4d9f7addd81f0 synthetic-only; exceptions enabled");
    const std::pair<const char*, void(*)()> cases[] = {
        {"memory", memory_contract}, {"registration", registration_contract}, {"imports", imports_contract},
        {"interpreter", +[] { interpreter_contract(false); }}, {"budgets", budgets_contract}, {"errors", errors_contract}};
    {
        Capture capture(log);
        for (const auto& [name, test] : cases) {
            log.write("[framework] BEGIN case=%s", name);
            try {
                if (options.corrupt_case && std::strcmp(name, "interpreter") == 0 &&
                    std::strcmp(options.corrupt_case, name) == 0) interpreter_contract(true);
                else test();
                log.write("[framework] case=%s PASS expected oracle and error contracts matched", name);
            } catch (const std::exception& error) {
                result.framework = false;
                log.write("[framework] case=%s FAIL %s", name, error.what());
            } catch (...) {
                result.framework = false;
                log.write("[framework] case=%s FAIL unknown exception", name);
            }
        }
    }
#if YAKUMO_WITH_OVERLAYS
    {
        Capture capture(log);
        result.overlays = overlay_probe(log);
    }
#endif
    try { result.sdk_services = runtime_services(paths, log, options, result.cleanup); }
    catch (const std::exception& error) { log.write("[sdk-services] FAIL exception=%s", error.what()); }
    catch (...) { log.write("[sdk-services] FAIL unknown exception"); }
    log.write("[runtime-summary] framework=%s sdk_services=%s cleanup=%s",
              result.framework ? "PASS" : "FAIL", result.sdk_services ? "PASS" : "FAIL",
              result.cleanup ? "PASS" : "FAIL");
    return result;
}
bool runtime_overall_pass(const RuntimeProbeResult& result, bool storage, bool devices, bool graphics) {
    return result.passed() && storage && devices && graphics;
}
}
