// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.hpp"
#include "integration_markers.hpp"
#include "integration_pad.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
using namespace yakumo::ps5;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct HostDevices:IntegrationDevices {
    bool fail_open{},fail_output{},fail_close{},omit_disconnect{},short_disconnect{},omit_cross{};
    bool audio_open{},pad_open{};unsigned outputs{};
    IntegrationInput poll(unsigned cycle,unsigned tick)override{
        IntegrationInput s;s.valid=true;s.connected=true;pad_open=true;
        if(cycle==2&&!omit_disconnect&&tick>=31&&tick<(short_disconnect?61u:211u))s.connected=false;
        if(cycle==2&&!omit_cross&&tick>=240&&tick<270)s.buttons=0x4000;
        s.lx=static_cast<unsigned char>(110+tick%40);return s;
    }
    bool close_pad()override{pad_open=false;return !fail_close;}
    bool open_audio()override{audio_open=!fail_open;return audio_open;}
    bool output_audio(const float* p,unsigned frames)override{
        require(audio_open&&frames==256,"audio handle/frame ownership");
        require(std::all_of(p,p+frames*2,[](float x){return std::isfinite(x)&&std::abs(x)<=0.04f;}),"native sink amplitude oracle");
        ++outputs;return !fail_output;
    }
    bool close_audio()override{audio_open=false;return !fail_close;}
};
IntegrationResult run(const TitlePaths& paths,Log& log,HostDevices& io,unsigned cycle,IntegrationFault fault=IntegrationFault::None,bool gpu=true){
    IntegrationSession session(paths,log,io,cycle,{1800,2000,fault});session.start();
    IntegrationSnapshot s;
    while(session.frame(s)) {
        if(s.sequence){require(integration_snapshot_valid(s),"published snapshot invalid");
            const auto m=integration_markers(s);
            require(m.x[0]+4==m.expected[0].x,"guest marker position oracle");
            session.rendered(gpu);}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto result=session.finish(gpu);require(!io.audio_open&&!io.pad_open,"handles retained after join");return result;
}
int main(){
    char fixture[]="/tmp/yakumo-integration-XXXXXX";const char* root=mkdtemp(fixture);if(!root)return 2;
    int status=0;
    try{
        TitlePaths paths;std::snprintf(paths.cache,sizeof(paths.cache),"%s",root);Log log;
        require(integration_audio_contracts(),"real PCM/SAS contracts");require(integration_controls_contracts(),"real binding/chord contracts");
        IntegrationPadInformation info{1,1920,1080,10,10,0,1,1,0,{}};
        require(integration_pad_information(&info,0,true).valid,"coherent candidate pad ABI rejected");
        require(!integration_pad_information(&info,1,true).valid&&!integration_pad_information(&info,0,false).valid,"pad API/guard failure hidden");
        info.connected=2;require(!integration_pad_information(&info,0,true).valid,"invented connection accepted");
        info.connected=1;info.density=0;require(!integration_pad_information(&info,0,true).valid,"incoherent connected ABI accepted");
        info.connected=0;require(integration_pad_information(&info,0,true).valid&&!integration_pad_information(&info,0,true).connected,"disconnect state");
        std::vector<std::int16_t> wave;
        for(int i=0;i<4410;++i){wave.push_back(static_cast<std::int16_t>(i*3));wave.push_back(static_cast<std::int16_t>(-i*2));}
        IntegrationResampler full,split;
        const auto expected=full.append(wave);std::vector<float> pieces;
        for(std::size_t at=0;at<wave.size();) {
            const auto count=std::min<std::size_t>(74,wave.size()-at);
            auto part=split.append({wave.begin()+at,wave.begin()+at+count});pieces.insert(pieces.end(),part.begin(),part.end());at+=count;
        }
        require(pieces==expected && full.output_frames()==4799 && split.input_frames()==4410,"rational phase across arbitrary blocks/count oracle");
        for(unsigned out=0;out<100;++out){
            const double source=double(out)*44100/48000;
            require(std::abs(expected[out*2]-float(source*3*0.04/32768))<1e-8,"linear frequency conversion slope oracle");
        }
        HostDevices io;require(run(paths,log,io,1).passed(),"first combined cycle");
        HostDevices io2;require(run(paths,log,io2,2).passed(),"second combined cycle/recovery");
        for(auto fault:{IntegrationFault::Guest,IntegrationFault::Audio,IntegrationFault::Queue,IntegrationFault::File,
                       IntegrationFault::Device,IntegrationFault::Cleanup,IntegrationFault::Omit}){
            HostDevices broken;require(!run(paths,log,broken,1,fault).passed(),"injected failure reached integration PASS");
        }
        for(unsigned scenario=0;scenario<6;++scenario){
            HostDevices broken;
            if(scenario==0)broken.fail_open=true;if(scenario==1)broken.fail_output=true;if(scenario==2)broken.fail_close=true;
            if(scenario==3)broken.omit_disconnect=true;if(scenario==4)broken.short_disconnect=true;if(scenario==5)broken.omit_cross=true;
            require(!run(paths,log,broken,scenario>=3?2:1).passed(),"device or mandatory recovery failure reached PASS");
        }
        HostDevices bad_gpu;require(!run(paths,log,bad_gpu,1,IntegrationFault::None,false).passed(),"graphics failure reached PASS");
        // Preserve unrelated files and reject a no-follow directory before writes.
        const auto directory=std::filesystem::path(root)/"integration-probe";
        for(const auto& item:std::filesystem::directory_iterator(directory))require(false,"owned checkpoint file leaked");
        {std::ofstream keep(directory/"keep.txt");keep<<"personal sentinel";}
        HostDevices normal;require(run(paths,log,normal,1).passed(),"user-shaped file preservation cycle");
        require(std::filesystem::file_size(directory/"keep.txt")==17,"unrelated data changed");
        std::filesystem::remove(directory/"keep.txt");std::filesystem::remove(directory);
        std::filesystem::create_directory_symlink(root,directory);
        HostDevices symlink;require(!run(paths,log,symlink,1).passed(),"symlink checkpoint directory accepted");
        IntegrationResult result{true,true,true,true,true,true};
        for(unsigned i=0;i<6;++i){auto bad=result;bool* fields[]{&bad.execution,&bad.controls,&bad.audio,&bad.files,&bad.graphics,&bad.cleanup};*fields[i]=false;require(!bad.passed(),"aggregate swallowed subsystem failure");}
        std::puts("[host-integration] PASS real generated32/runtime/PCM/SAS/chords, two cycles, rational conversion, recovery and failure propagation; host evidence only");
    }catch(const std::exception& e){std::fprintf(stderr,"[host-integration] FAIL %s\n",e.what());status=1;}
    std::filesystem::remove_all(root);return status;
}
