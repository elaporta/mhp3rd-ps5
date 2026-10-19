// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_audio.hpp"
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <thread>
void check(bool value){if(!value)throw std::runtime_error("native audio gain/levels regression");}
int main(){
    using namespace yakumo::game_audio;
    check(sample(8192,1,false)==.25f);
    check(sample(-32768,1,false)==-1.f);
    check(sample(32767,1,false)==32767.f/32768.f);
    check(sample(8192,.5f,false)==.125f);
    check(sample(8192,NAN,false)==0);
    check(sample(32767,1,true)==0&&sample(-32768,0,false)==0);
    check(sample(8192,2,false)==sample(8192,1,false)&&sample(8192,-1,false)==0);
    Levels levels;
    for(int i=-32768;i<=32767;++i)levels.note(i,sample(i,1,false));
    check(levels.samples==65536&&levels.input_rails==2&&levels.outside_range==0&&levels.nonfinite==0);
    check(levels.peak==1&&levels.rms()>.577&&levels.rms()<.578);
    // Linear resampling is convex, including transitions between both rails.
    for(unsigned phase=0;phase<48000;++phase) {
        const double interpolated=-32768+(32767.+32768)*phase/48000.;
        const auto value=sample(interpolated,1,false);check(value>=-1&&value<1);
    }
    Telemetry state;
    state.publish(levels,128,.5f,false,256);
    auto snapshot=state.snapshot();
    check(snapshot.valid&&snapshot.output_blocks==128&&snapshot.levels.samples==65536);
    check(snapshot.volume==.5f&&snapshot.levels.peak==1&&snapshot.last_result==256);
    std::ostringstream output;write_levels(output,snapshot,"periodic");
    check(output.str().find("phase=periodic normalization=s16-to-float output_scale=1 volume_percent=50")!=std::string::npos);
    check(output.str().find("outside_range=0 nonfinite=0")!=std::string::npos);
    // The worker can safely snapshot while the audio writer publishes blocks.
    std::atomic<bool> done{};
    std::thread writer([&]{for(unsigned i=1;i<=10000;++i){Levels l;l.samples=i;l.energy=i;l.peak=1;state.publish(l,i,1,false,256);}done=true;});
    while(!done){const auto value=state.snapshot();if(value.valid)check(value.output_blocks==128||value.output_blocks==value.levels.samples);}
    writer.join();check(state.snapshot().output_blocks==10000);
    levels.note(0,1.1f);levels.note(0,NAN);
    check(levels.outside_range==1&&levels.nonfinite==1);
    std::cout<<"Unity PCM normalization, volume/mute, resampling bounds and live telemetry PASS\n";
}
