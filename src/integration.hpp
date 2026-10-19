// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "probe.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace yakumo::ps5 {
struct IntegrationSnapshot {
    std::uint32_t sequence{}, module{}, buttons{}, guest{}, tag{}, audio_hash{}, data_hash{};
    int x{}, y{};
    bool connected{};
};
struct IntegrationResult {
    bool execution{}, controls{}, audio{}, files{}, graphics{}, cleanup{};
    bool passed() const { return execution && controls && audio && files && graphics && cleanup; }
};
struct IntegrationInput {
    std::uint32_t buttons{};
    unsigned char lx{128}, ly{128}, rx{128}, ry{128}, l2{}, r2{};
    bool connected{}, valid{};
    int api_result{}, reported_connected{-1},device_class{};
    unsigned count{},resolution_x{},resolution_y{};
    float density{};
    bool open_attempt{};
    const char* operation{"synthetic"};
};
// Device boundaries are injected on host; framework/profile algorithms remain real.
struct IntegrationDevices {
    virtual ~IntegrationDevices() = default;
    virtual IntegrationInput poll(unsigned cycle, unsigned tick) = 0;
    virtual bool close_pad() = 0;
    virtual bool open_audio() = 0;
    virtual bool output_audio(const float* stereo, unsigned frames) = 0;
    virtual bool close_audio() = 0;
    virtual int audio_status() const { return 0; }
};
std::unique_ptr<IntegrationDevices> make_integration_devices();
enum class IntegrationFault { None, Guest, Audio, Queue, File, Device, Cleanup, Omit };
struct IntegrationOptions {
    unsigned ticks{1800}, tick_us{33333};
    IntegrationFault fault{IntegrationFault::None};
};
// Exact rational phase in units of 1/48000 source frames, preserved across blocks.
class IntegrationResampler {
public:
    std::vector<float> append(const std::vector<std::int16_t>& stereo);
    std::uint64_t input_frames() const { return frames_; }
    std::uint64_t output_frames() const { return outputs_; }
private:
    std::vector<std::int16_t> pending_;
    std::uint64_t phase_{}, frames_{}, outputs_{};
};
class IntegrationSession {
public:
    IntegrationSession(const TitlePaths&, Log&, IntegrationDevices&, unsigned cycle, IntegrationOptions = {});
    ~IntegrationSession();
    void start();
    bool frame(IntegrationSnapshot&); // main-thread logging and publication only
    void rendered(bool passed);
    IntegrationResult finish(bool graphics);
    std::uint64_t heartbeat(unsigned worker) const;
    bool active(unsigned worker) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool integration_audio_contracts();
bool integration_controls_contracts();
bool integration_checkpoint(const TitlePaths&, const IntegrationSnapshot&, bool corrupt = false);
bool integration_snapshot_valid(const IntegrationSnapshot&);
bool integration_probe(const TitlePaths&, Log&) noexcept;
bool integration_boot_watch_start(const TitlePaths&, Log&) noexcept;
void integration_boot_watch_ready(const TitlePaths&) noexcept;
void integration_boot_watch_stop(Log&) noexcept;
struct IntegrationGraphicsHooks {
    void* state;
    bool (*frame)(void*, IntegrationSnapshot&);
    void (*rendered)(void*, bool);
};
bool integration_graphics(const TitlePaths&, Log&, IntegrationGraphicsHooks);
}
