// SPDX-License-Identifier: GPL-3.0-or-later
// Public ABI facts cross-checked with PS5SX2's PS5 frontend and SDK import stubs.
#include "probe.hpp"
#include <cstdint>
#include <cstring>
#include <unistd.h>

extern "C" {
int sceUserServiceInitialize(const void*);
int sceUserServiceGetInitialUser(std::int32_t*);
int scePadInit();
int scePadOpen(std::int32_t, std::int32_t, std::int32_t, const void*);
int scePadReadState(std::int32_t, void*);
int scePadClose(std::int32_t);
int sceAudioOutInit();
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned);
int sceAudioOutOutput(int, const void*);
int sceAudioOutClose(int);
}
namespace yakumo::ps5 {
bool input_audio_probe(Log& log) {
    const int user_init = sceUserServiceInitialize(nullptr);
    std::int32_t user = -1;
    const int user_result = sceUserServiceGetInitialUser(&user);
    const int pad_init = scePadInit();
    int pad = user_result == 0 ? scePadOpen(user, 0, 0, nullptr) : -1;
    log.write("[pad] user_init=%#x user_result=%#x user=%d init=%#x handle=%d",
              user_init, user_result, user, pad_init, pad);
    const int audio_init = sceAudioOutInit();
    const int audio = sceAudioOutOpen(255, 0, 0, 256, 48000, 4);
    log.write("[audio] init=%#x handle=%d float-stereo 48000Hz grain=256", audio_init, audio);
    bool audio_ok = audio >= 0;
    if (audio_ok) {
        // Low-volume triangle wave, 480 Hz, one second, then one silent block.
        alignas(64) float pcm[512]{};
        for (unsigned block = 0; block < 189; ++block) {
            for (unsigned i = 0; i < 256; ++i) {
                const unsigned phase = (block * 256 + i) % 100;
                const float triangle = static_cast<float>(phase < 50 ? phase : 100 - phase) / 25.0f - 1.0f;
                pcm[i * 2] = pcm[i * 2 + 1] = block == 188 ? 0.0f : triangle * 0.04f;
            }
            const int result = sceAudioOutOutput(audio, pcm);
            if (result < 0) { log.write("[audio] FAIL output=%#x block=%u", result, block); audio_ok = false; break; }
        }
        const int closed = sceAudioOutClose(audio);
        audio_ok = audio_ok && closed == 0;
        log.write("[audio] output_api=%s close=%#x audible result requires user confirmation", audio_ok ? "PASS" : "FAIL", closed);
    }
    // Buffer is deliberately larger than the public prefix being inspected.
    struct alignas(16) PadState {
        std::uint32_t buttons;
        std::uint8_t lx, ly, rx, ry, l2, r2;
        std::uint8_t opaque[502];
    } state{}, previous{};
    bool saw_state = false;
    int last_result = -9999;
    log.write("[pad] sampling 30 seconds; move sticks, press buttons, disconnect and reconnect");
    for (unsigned sample = 0; sample < 1500; ++sample) {
        if (pad < 0 && sample % 50 == 0) {
            if (sceUserServiceGetInitialUser(&user) == 0) pad = scePadOpen(user, 0, 0, nullptr);
        }
        const int result = pad >= 0 ? scePadReadState(pad, &state) : -1;
        if (result != last_result || (result == 0 && std::memcmp(&state, &previous, 10) && sample % 5 == 0)) {
            log.write("[pad] t_ms=%u read=%#x buttons=%#x sticks=%u,%u,%u,%u triggers=%u,%u",
                      sample * 20, result, state.buttons, state.lx, state.ly, state.rx, state.ry, state.l2, state.r2);
            previous = state;
        }
        last_result = result;
        if (result == 0) saw_state = true;
        else if (pad >= 0) { scePadClose(pad); pad = -1; }
        usleep(20000);
    }
    if (pad >= 0) log.write("[pad] close=%#x", scePadClose(pad));
    log.write("[pad] state_api=%s; disconnect/reconnect needs observed state evidence", saw_state ? "PASS" : "FAIL");
    return saw_state && audio_ok;
}
}
