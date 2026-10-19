// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ostream>
namespace yakumo::game_audio {
// Unity gain for signed-16-bit PCM -> normalized float output. Remove the
// former 0.04/0.06 attenuation from the game's master-volume path.
inline constexpr float output_scale=1.0f;
inline float sample(double pcm, float volume, bool paused) {
    return paused||!std::isfinite(volume)?0.f:static_cast<float>(pcm*std::clamp(volume,0.f,1.f)*output_scale/32768.0);
}
struct Levels {
    std::uint64_t samples{},input_rails{},outside_range{},nonfinite{};
    double peak{},energy{};
    void note(double input,float output) {
        ++samples;
        if(input<=-32768 || input>=32767)++input_rails;
        if(!std::isfinite(output)){++nonfinite;return;}
        const double magnitude=std::abs(output);
        peak=std::max(peak,magnitude);energy+=double(output)*output;
        if(magnitude>1.0)++outside_range;
    }
    double rms()const{return samples?std::sqrt(energy/samples):0;}
};
struct Snapshot {
    Levels levels;
    std::uint64_t output_blocks{};
    float volume{};
    bool paused{},valid{};
    int last_result{};
};
// Single audio-thread writer, bounded worker-thread snapshots. All fields are
// atomic so a concurrent publish never introduces a C++ data race. The version
// prevents combining counters/volume from different blocks. No file I/O or
// allocation occurs on the audio thread during publication.
class Telemetry {
    std::atomic<std::uint64_t> version{},samples{},rails{},outside{},nonfinite{},blocks{};
    std::atomic<double> peak{},energy{};
    std::atomic<float> volume{};
    std::atomic<bool> paused{};
    std::atomic<int> result{};
public:
    void publish(const Levels& levels,std::uint64_t count,float gain,bool pause,int rc) {
        ++version;
        samples=levels.samples;rails=levels.input_rails;outside=levels.outside_range;nonfinite=levels.nonfinite;
        peak=levels.peak;energy=levels.energy;blocks=count;volume=gain;paused=pause;result=rc;
        ++version;
    }
    Snapshot snapshot()const {
        for(unsigned attempt=0;attempt<3;++attempt) {
            const auto before=version.load();if(before&1)continue;
            Snapshot value;
            value.levels={samples.load(),rails.load(),outside.load(),nonfinite.load(),peak.load(),energy.load()};
            value.output_blocks=blocks.load();value.volume=volume.load();value.paused=paused.load();value.last_result=result.load();
            if(before==version.load()){value.valid=true;return value;}
        }
        return {};
    }
};
inline Telemetry telemetry;
inline void write_levels(std::ostream& stream,const Snapshot& value,const char* phase) {
    if(!value.valid || !value.output_blocks)return;
    stream<<"[game-audio-levels] phase="<<phase<<" normalization=s16-to-float output_scale="<<output_scale
        <<" volume_percent="<<value.volume*100<<" paused="<<value.paused
        <<" samples="<<value.levels.samples<<" peak="<<value.levels.peak<<" rms="<<value.levels.rms()
        <<" input_rail_samples="<<value.levels.input_rails<<" outside_range="<<value.levels.outside_range
        <<" nonfinite="<<value.levels.nonfinite<<" output_blocks="<<value.output_blocks<<" last_rc="<<value.last_result<<'\n';
}
}
