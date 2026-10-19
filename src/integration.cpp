// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.hpp"
#include "overlay_module.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/interpreter.hpp"
#include "audio/guest_pcm.hpp"
#include "audio/sas_core.hpp"
#include "input/presets.hpp"
#include "input/chords.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#if YAKUMO_NATIVE
extern "C" void ps5_thread_stacks(unsigned*,unsigned*);
#endif

namespace yakumo::ps5 {
namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
constexpr unsigned scratch = 0x08807000, pcm = 0x08808000, host = 0x08806000, stop_pc = host + 4;
struct IntegrationModule {
    const mhp3rd::OverlayModuleInfo* (*info)();
    void (*install)(psprecomp::Runtime&);
    const unsigned char* image;
    unsigned immediate, mask, tag;
};
struct AudioPacket { unsigned sequence{},hash{};std::vector<std::int16_t> samples; };
#include "integration_catalog.inc"
constexpr unsigned module_count = YAKUMO_INTEGRATION_MODULE_COUNT;
std::uint64_t fnv64(const unsigned char* bytes, unsigned count) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    return hash;
}
std::uint32_t checksum(const void* data, std::size_t size) {
    auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
unsigned expected_guest(unsigned index, unsigned input) {
    // Derived from public MIPS arithmetic: + immediate, xor mask, 3*(2+1),
    // call delay slot +4, host import +1000. No generated result reused here.
    return ((input + index + 3) ^ (index * 17 + 5)) + 1013;
}
void host_import(psprecomp::Runtime& runtime, psprecomp::AllegrexContext& ctx) {
    const auto token = psprecomp::capture_runtime_execution_context();
    const auto returned = ctx.gpr[31];
    runtime.invoke_import_cached(23, "IntegrationHost", 0x01234567, ctx);
    if (!runtime.stopped() && psprecomp::runtime_execution_context_matches(token)) ctx.pc = returned;
}
void complete(psprecomp::Runtime& runtime, psprecomp::AllegrexContext&) { runtime.stop("integration tick completed"); }
unsigned input_value(const IntegrationSnapshot& s) { return s.sequence + (s.buttons & 0xffff) + unsigned(s.x + 127); }
std::uint32_t data_hash(const IntegrationSnapshot& s) {
    const std::uint32_t words[]{s.sequence,s.module,s.buttons,s.guest,s.tag,s.audio_hash,
        static_cast<unsigned>(s.x),static_cast<unsigned>(s.y),s.connected ? 1u : 0u};
    return checksum(words, sizeof(words));
}
template<class T> struct Queue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<T> items;
    bool closed{};
    unsigned peak{};
    bool push(T value) {
        std::lock_guard lock(mutex);
        if (closed || items.size() >= 8) return false;
        items.push_back(std::move(value)); peak = std::max(peak, unsigned(items.size())); wake.notify_one(); return true;
    }
    bool pop(T& value) {
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, std::chrono::seconds(5), [&]{return closed || !items.empty();}))
            throw std::runtime_error("queue wait exceeded five seconds");
        if (items.empty()) return false;
        value = std::move(items.front()); items.pop_front(); return true;
    }
    void close() { std::lock_guard lock(mutex); closed = true; wake.notify_all(); }
};
// Bounded upstream stream capture is drained into events, never recovered from stdout.
struct Capture : std::streambuf {
    std::string bytes;
    bool overflowed{};
    std::streambuf *out, *err;
    Capture() : out(std::cout.rdbuf(this)), err(std::cerr.rdbuf(this)) {}
    ~Capture() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            if (bytes.size() >= 8192) overflowed = true;
            else bytes.push_back(traits_type::to_char_type(c));
        }
        return traits_type::not_eof(c);
    }
};
}

std::vector<float> IntegrationResampler::append(const std::vector<std::int16_t>& stereo) {
    require(stereo.size() % 2 == 0, "odd stereo resampler input");
    frames_ += stereo.size()/2;
    pending_.insert(pending_.end(), stereo.begin(), stereo.end());
    std::vector<float> output;
    while (phase_/48000 + 1 < pending_.size()/2) {
        const auto source = phase_/48000;
        const auto fraction = phase_%48000;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const double a = pending_[source*2+channel], b = pending_[(source+1)*2+channel];
            output.push_back(float((a + (b-a)*double(fraction)/48000) * (0.04/32768.0)));
        }
        phase_ += 44100; ++outputs_;
    }
    const auto remove = std::min<std::uint64_t>(phase_/48000, pending_.size()/2);
    pending_.erase(pending_.begin(), pending_.begin()+remove*2);
    phase_ -= remove*48000;
    return output;
}

bool integration_snapshot_valid(const IntegrationSnapshot& s) {
    return s.module < module_count && s.x >= -127 && s.x <= 127 && s.y >= -127 && s.y <= 127 &&
        s.guest == expected_guest(s.module,input_value(s)) && s.tag == s.module+401 &&
        s.data_hash == data_hash(s) && (s.connected || (s.buttons == 0 && s.x == 0 && s.y == 0));
}

bool integration_audio_contracts() {
    psprecomp::GuestMemory memory;
    for (unsigned i = 0; i < 64; ++i) memory.store16(pcm+i*2, 1000);
    std::vector<std::int16_t> stage;
    require(mhp3rd::audio::stage_guest_pcm(memory,pcm,32,true,stage) && stage.size()==64 &&
        std::all_of(stage.begin(),stage.end(),[](auto x){return x==1000;}), "mono PCM oracle");
    memory.store16(pcm,1234); memory.store16(pcm+2,static_cast<unsigned short>(-4321));
    require(mhp3rd::audio::stage_guest_pcm(memory,pcm,1,false,stage) && stage[0]==1234 && stage[1]==-4321, "stereo PCM oracle");
    const auto before = stage;
    require(!mhp3rd::audio::stage_guest_pcm(memory,0x09ffffff,2,false,stage) && stage==before &&
        !mhp3rd::audio::stage_guest_pcm(memory,0,1,true,stage) && stage==before, "PCM rejected range preservation");
    memory.store16(pcm,1000); memory.store16(pcm+2,1000);
    mhp3rd::audio::SasCore sas;
    sas.init(256,2,0); sas.set_voice_pcm(0,pcm,128,0); sas.key_on(0);
    std::array<std::int16_t,64> mix{}; sas.render(memory,mix.data(),32);
    require(std::all_of(mix.begin(),mix.end(),[](auto x){return x==999;}), "SAS constant PCM independent 999 oracle");
    sas.set_pause(1,true); sas.render(memory,mix.data(),32);
    require(std::all_of(mix.begin(),mix.end(),[](auto x){return x==0;}) && !(sas.end_flag()&1), "SAS pause");
    sas.set_pause(1,false); sas.set_voice_pcm(1,pcm,128,0); sas.key_on(1); sas.render(memory,mix.data(),32);
    require(mix[0]==1998 && mix[1]==1998, "SAS two voice mixture");
    for (unsigned i=0;i<64;++i) memory.store16(pcm+i*2,32767);
    sas.key_on(0); sas.key_on(1); sas.render(memory,mix.data(),32);
    require(mix[0]==32767 && mix[1]==32767, "SAS saturation");
    sas.key_off(0); sas.key_off(1); sas.render(memory,mix.data(),32);
    require((sas.end_flag()&3)==3 && mix[0]==0, "SAS voice end");
    const unsigned vag=pcm+1024;
    memory.store8(vag,0); memory.store8(vag+1,3);
    for(unsigned i=2;i<16;++i) memory.store8(vag+i,0x11);
    sas.set_voice(0,vag,16,true); sas.key_on(0); sas.render(memory,mix.data(),32);
    require(mix[0]==4095 && mix[63]==4095, "SAS synthetic VAG predictor-zero oracle");
    return true;
}

bool integration_controls_contracts() {
    using namespace mhp3rd::input;
    Resolver resolver;
    const auto& preset=layout(Preset::Default);
    const auto t=table(preset,true);
    auto south=[](Binding b){return b==pad(PadInput::South);};
    require(resolver.update(t,south,100,50).buttons & 0x4000, "Default South/Cross mapping");
    resolver.reset();
    auto none=[](Binding){return false;};
    require(resolver.update(t,none,120,50).buttons==0 && resolver.update(t,none,200,50).buttons==0, "disconnect stale chord neutralization");
    Bindings chords{};
    chords[unsigned(Action::Square)][0]=single(pad(PadInput::West));
    chords[unsigned(Action::TriangleCircle)][0]=chord(pad(PadInput::West),pad(PadInput::North));
    Table custom{chords,{},true}; resolver.reset();
    auto west=[](Binding b){return b==pad(PadInput::West);};
    auto both=[](Binding b){return b==pad(PadInput::West)||b==pad(PadInput::North);};
    require(resolver.update(custom,west,0,50).buttons==0, "short chord must wait");
    require(resolver.update(custom,both,20,50).buttons==0x3000, "long chord independent triangle+circle oracle");
    resolver.reset(); require(resolver.update(custom,none,30,50).buttons==0, "reset cancels pending chord");
    return true;
}

bool integration_checkpoint(const TitlePaths& paths, const IntegrationSnapshot& s, bool corrupt) {
    char dir[path_capacity], original[path_capacity], renamed[path_capacity], name[96];
    if (!integration_snapshot_valid(s) || !join_path(dir,sizeof(dir),paths.cache,"integration-probe")) return false;
    if (mkdir(dir,0700)!=0 && errno!=EEXIST) return false;
    const int directory=open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(directory<0) return false;
    struct stat info{};
    bool ok=fstat(directory,&info)==0 && S_ISDIR(info.st_mode);
    ok=(close(directory)==0)&&ok; if(!ok) return false;
    std::snprintf(name,sizeof(name),"checkpoint-%llu-%u",static_cast<unsigned long long>(now_us()),s.sequence);
    if(!join_path(original,sizeof(original),dir,name)) return false;
    std::strcat(name,".renamed"); if(!join_path(renamed,sizeof(renamed),dir,name)) return false;
    // Explicit fixed-width payload, no padding/pointers or user data.
    const std::uint32_t payload[]{0x594b3138,s.sequence,s.module,s.buttons,s.guest,s.tag,s.audio_hash,s.data_hash,
        unsigned(s.x),unsigned(s.y),s.connected?1u:0u};
    int fd=open(original,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0) return false;
    ok=write(fd,payload,sizeof(payload))==ssize_t(sizeof(payload));
    ok=(fsync(fd)==0)&&ok; ok=(close(fd)==0)&&ok;
    const int reservation=open(renamed,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    bool owned=reservation>=0, moved=false;
    if(owned) { ok=(close(reservation)==0)&&ok; moved=rename(original,renamed)==0; ok=moved&&ok; }
    else ok=false;
    if(moved) {
        fd=open(renamed,O_RDONLY|O_NOFOLLOW);
        if(fd>=0) {
            std::array<unsigned char,sizeof(payload)+1> actual{};
            ok=(fstat(fd,&info)==0 && S_ISREG(info.st_mode) && info.st_size==off_t(sizeof(payload)))&&ok;
            ok=(read(fd,actual.data(),actual.size())==ssize_t(sizeof(payload)))&&ok;
            if(corrupt) actual[0]^=1;
            ok=(std::memcmp(actual.data(),payload,sizeof(payload))==0)&&ok;
            ok=(close(fd)==0)&&ok;
        } else ok=false;
    }
    bool cleaned=moved || unlink(original)==0;
    cleaned=(!owned || unlink(renamed)==0)&&cleaned;
    return ok&&cleaned;
}

struct IntegrationSession::Impl {
    const TitlePaths& paths; Log& log; IntegrationDevices& devices; unsigned cycle; IntegrationOptions options;
    std::atomic<bool> failed{false}, done{false}, abort{false};
    std::atomic<bool> execution{false},controls{false},audio{false},files{false},clean{true};
    std::atomic<unsigned> ticks{0},audio_blocks{0},checkpoints{0};
    std::atomic<unsigned> registrations{0},registrations_peak{0};
    std::array<std::atomic<std::uint64_t>,4> progress{};
    std::array<std::atomic<bool>,4> running{};
    std::mutex publication,events_mutex;
    IntegrationSnapshot snapshot{};
    std::deque<std::string> events;
    Queue<AudioPacket> audio_queue;
    Queue<IntegrationSnapshot> file_queue;
    std::thread simulation,audio_worker,file_worker;
    unsigned rendered_frames{},pixels{};
    std::uint64_t last_log{};
    Impl(const TitlePaths& p,Log& l,IntegrationDevices& d,unsigned c,IntegrationOptions o):paths(p),log(l),devices(d),cycle(c),options(o){}
    void beat(unsigned i){progress[i].store(now_us());}
    void event(const std::string& text){std::lock_guard lock(events_mutex);if(events.size()>=128) failed=true;else events.push_back(text);}
    void fail(const char* text){failed=true;event(std::string("FAIL ")+text);}
    void simulate() {
        running[0]=true; beat(0);
        try {
            Capture diagnostics;
            require(integration_audio_contracts(),"portable audio contracts");
            require(integration_controls_contracts(),"portable controls contracts");
            event("profile PCM mono/stereo/ranges; SAS mix/saturation/pause/end/VAG; Default/chords/reset PASS");
            psprecomp::Runtime runtime;
            unsigned imports=0;
            runtime.register_hle("IntegrationHost",0x01234567,[&](auto&,auto& ctx){++imports;ctx.gpr[2]=ctx.gpr[4]+1000;});
            runtime.register_function(host,host_import,"integration_host"); runtime.register_function(stop_pc,complete,"integration_complete");
            psprecomp::set_interpreter_fallback_enabled(true); psprecomp::reset_interpreter_stats();
            mhp3rd::input::Resolver resolver;
            bool was_connected=false, disconnected=false,recovered=false,fresh=false,seen_connected=false;
            std::uint64_t disconnected_ms=0; bool was_cross=false;
            const auto start=Clock::now();
            std::size_t peak=2;
            for(unsigned seq=1;seq<=options.ticks && !abort.load();++seq) {
                if(seq>1) std::this_thread::sleep_until(start+std::chrono::microseconds(std::uint64_t(seq-1)*options.tick_us));
                IntegrationSnapshot s{}; s.sequence=seq; s.module=(seq-1)%module_count;
                const auto control_ms=std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count())*1000/(options.tick_us*30ull);
                auto raw=devices.poll(cycle,seq);
                if(raw.valid && raw.open_attempt && raw.api_result<0)
                    event("[expected-retry] controller explicitly disconnected; reopen rc="+std::to_string(raw.api_result)+"; recovery still required");
                if(seq==1 || raw.connected!=was_connected || !raw.valid) {
                    event(std::string("device operation=")+raw.operation+" rc="+std::to_string(raw.api_result)+" connected_raw="+std::to_string(raw.reported_connected)+
                        " count="+std::to_string(raw.count)+" resolution="+std::to_string(raw.resolution_x)+"x"+std::to_string(raw.resolution_y)+
                        " density="+std::to_string(raw.density)+" device_class="+std::to_string(raw.device_class)+" valid="+(raw.valid?"1":"0"));
                }
                if(!raw.valid && std::string(raw.operation)=="pad-info")
                    event("ABI_UNVERIFIED controller information failed validation; recovery=FAIL");
                require(raw.valid && options.fault!=IntegrationFault::Device,"pad ABI/API response invalid");
                if(raw.connected!=was_connected) {
                    event(raw.connected?"pad connected":"pad disconnected; controls neutralized; resolver reset");
                    resolver.reset(); was_cross=false;
                    if(!raw.connected && was_connected) {disconnected=true;disconnected_ms=control_ms;}
                    if(raw.connected && disconnected && control_ms-disconnected_ms>=5000) recovered=true;
                }
                s.connected=raw.connected; seen_connected|=raw.connected;
                if(raw.connected) {
                    using namespace mhp3rd::input;
                    const auto held=[&](Binding b) {
                        const int input=pad_input_of(b);
                        constexpr unsigned masks[]{0x4000,0x2000,0x8000,0x1000,0x100000,0x10000,8,2,4,0x400,0x800,0x10,0x40,0x80,0x20};
                        if(input>=0 && unsigned(input)<std::size(masks)) return (raw.buttons&masks[input])!=0;
                        return input==32?raw.l2>127:input==33?raw.r2>127:false;
                    };
                    const auto state=resolver.update(table(layout(Preset::Default),true),held,control_ms,50);
                    s.buttons=state.buttons;
                    s.x=std::clamp(int(raw.lx)-128,-127,127); s.y=std::clamp(int(raw.ly)-128,-127,127);
                    const bool cross=(s.buttons&0x4000)!=0;
                    if(recovered && cross && !was_cross && control_ms<=30000) {fresh=true;event("pad recovered; fresh Cross observed PASS");}
                    was_cross=cross;
                }
                was_connected=raw.connected;
                if(cycle==2 && seq==900) require(disconnected&&recovered&&fresh,"mandatory disconnect/5-second wait/reconnect/fresh Cross absent within first 30 seconds");
                const auto& module=integration_modules[s.module]; const auto* info=module.info();
                require(info && info->abi_version==mhp3rd::kOverlayAbiVersion && info->size==512 &&
                    info->base==0x08910000+(s.module%2)*0x1000 && (info->code_size==128||info->code_size==256) &&
                    info->hash==fnv64(module.image,64+info->code_size),"module identity/ABI mismatch");
                const unsigned base=info->base,entry=base+64,retired=base+256;
                runtime.unregister_functions(base,base+512);
                require(!runtime.has_function(entry)&&!runtime.has_function(retired),"stale registry after removal");
                runtime.memory().copy_in(base,std::span<const unsigned char>(module.image,512)); module.install(runtime);
                require(runtime.has_function(entry) && runtime.has_function(retired)==(info->code_size==256) &&
                    runtime.has_function(host)&&runtime.has_function(stop_pc),"replacement/outside-slot registry mismatch");
                peak=std::max(peak,runtime.function_count()); require(peak<=130,"registry grows beyond two slots");
                registrations=runtime.function_count();registrations_peak=peak;
                runtime.cpu()={}; runtime.cpu().gpr[4]=input_value(s);runtime.cpu().gpr[5]=scratch;runtime.cpu().gpr[31]=stop_pc;
                const auto interpreted=psprecomp::interpreter_stats().instructions;
                runtime.run(entry,128); s.guest=runtime.memory().load32(scratch);s.tag=runtime.memory().load32(scratch+4);
                if(options.fault==IntegrationFault::Guest) s.guest^=1;
                require(runtime.stop_reason()=="integration tick completed" && runtime.cpu().gpr[2]==s.guest &&
                    s.guest==expected_guest(s.module,input_value(s)) && s.tag==s.module+401 &&
                    runtime.memory().load32(scratch+8)+1000==s.guest &&
                    psprecomp::interpreter_stats().instructions==interpreted,"generated independent oracle/unexpected interpretation");
                // Guest output determines tone. Cross changes guest input and selects the other tone.
                const unsigned frequency=(s.buttons&0x4000)?660:440;
                std::vector<std::int16_t> source;
                for(unsigned sample=0;sample<1470;++sample) {
                    const unsigned phase=(std::uint64_t(seq-1)*1470+sample)*frequency%44100;
                    const int triangle=phase<22050 ? int(phase)*48000/22050-24000 : 72000-int(phase)*48000/22050;
                    runtime.memory().store16(pcm+sample*2,static_cast<unsigned short>(triangle+int(s.guest%17)));
                }
                require(mhp3rd::audio::stage_guest_pcm(runtime.memory(),pcm,1470,true,source),"shared guest PCM staging");
                mhp3rd::audio::SasCore sas; sas.init(1470,1,0);sas.set_voice_pcm(0,pcm,2940,0);sas.key_on(0);
                std::vector<std::int16_t> mix(2940);sas.render(runtime.memory(),mix.data(),1470);
                // The unity, no-ADSR mixer produces floor(sample*32767/32768).
                for(unsigned sample=0;sample<mix.size();++sample)
                    require(mix[sample]==((std::int32_t(source[sample])*32767)>>15),"shared PCM/SAS independent sample oracle");
                if(options.fault==IntegrationFault::Audio) mix[0]^=1;
                require(mix[0]==((std::int32_t(source[0])*32767)>>15),"injected audio oracle rejected");
                s.audio_hash=checksum(mix.data(),mix.size()*2);s.data_hash=data_hash(s);
                require(integration_snapshot_valid(s),"shared snapshot checksum/oracle");
                if(options.fault==IntegrationFault::Queue) {
                    Queue<int> bounded;for(unsigned i=0;i<8;++i) require(bounded.push(i),"queue capacity");
                    require(!bounded.push(9),"overflow must reject");throw std::runtime_error("controlled queue overflow propagated");
                }
                require(audio_queue.push(AudioPacket{seq,s.audio_hash,std::move(mix)}),"audio queue overflow");
                if(seq%300==0 && options.fault!=IntegrationFault::Omit) require(file_queue.push(s),"checkpoint queue overflow");
                {std::lock_guard lock(publication);snapshot=s;}
                ticks=seq;beat(0);
                if(cycle==2 && seq==1200) {
                    std::vector<std::int16_t> preserved{11,22};const auto original=preserved;
                    require(!mhp3rd::audio::stage_guest_pcm(runtime.memory(),0xffffffff,4,false,preserved)&&preserved==original,
                        "controlled invalid PCM did not reject exactly/preserve staging");
                    event("[expected-rejection] invalid PCM range; previous staging preserved PASS");
                }
                if(cycle==2 && seq==1500) {
                    Queue<int> bounded;for(unsigned i=0;i<8;++i)require(bounded.push(i),"negative queue setup");
                    require(!bounded.push(9) && bounded.items.size()==8,"controlled ninth enqueue not rejected/preserved");
                    bounded.close();int got=0;for(unsigned i=0;i<8;++i)require(bounded.pop(got)&&got==int(i),"queue overflow corrupted FIFO");
                    require(!bounded.pop(got),"closed/drained queue did not stop");
                    event("[expected-rejection] queue capacity=8 ninth enqueue rejected; FIFO preserved/drained PASS");
                }
            }
            // Unknown-image fallback: same real loop/import, changed immediate identity.
            const auto& module=integration_modules[0];const auto base=module.info()->base;
            runtime.unregister_functions(base,base+512);runtime.memory().copy_in(base,std::span<const unsigned char>(module.image,512));
            runtime.memory().store32(base+68,0x2484001f); // +31 replaces +3
            require(fnv64(runtime.memory().raw_pointer(base,320),320)!=module.info()->hash,"unknown image identity");
            runtime.cpu()={};runtime.cpu().gpr[4]=7;runtime.cpu().gpr[5]=scratch;runtime.cpu().gpr[31]=stop_pc;
            psprecomp::reset_interpreter_stats();runtime.run(base+64,128);
            const auto fallback=psprecomp::interpreter_stats();
            require(runtime.cpu().gpr[2]==((7u+31u)^5u)+1013u && runtime.cpu().gpr[3]==401 &&
                runtime.stop_reason()=="integration tick completed" && runtime.memory().load32(scratch)==1048 &&
                runtime.memory().load32(scratch+4)==401 && fallback.instructions==23 && fallback.entries==2,
                "unknown image fallback independent 1048/401/23-instruction/2-entry oracle");
            event("unknown-image fallback v0="+std::to_string(runtime.cpu().gpr[2])+" expected=1048 tag=401 instructions="+
                std::to_string(fallback.instructions)+" expected=23 entries="+std::to_string(fallback.entries)+" expected=2 PASS; matched modules interpreted=0");
            // Real controlled negative: invalid PCM must leave existing staging untouched.
            std::vector<std::int16_t> unchanged{7,9};const auto original=unchanged;
            require(!mhp3rd::audio::stage_guest_pcm(runtime.memory(),0xffffffff,2,false,unchanged)&&unchanged==original,"negative PCM acceptance");
            runtime.unregister_functions(0x08910000,0x08912000);
            require(runtime.function_count()==2,"registry teardown");runtime.unregister_functions(host,host+8);
            require(runtime.function_count()==0,"host registry teardown");
            registrations=0;
            require(ticks==options.ticks && imports==options.ticks+1,"omitted tick/import");
            const auto measured_us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();
            require(measured_us>=std::int64_t(options.ticks-1)*options.tick_us &&
                measured_us<std::int64_t(options.tick_us)*options.ticks+5000000,"simulation pacing outside five-second allowance");
            controls=seen_connected && (cycle==1 || (disconnected&&recovered&&fresh)); execution=true;
            if(!controls) fail("mandatory controls missing");
            require(!diagnostics.overflowed,"upstream diagnostic capture overflow");
            if(!diagnostics.bytes.empty()) event(std::string("upstream: ")+diagnostics.bytes);
            event("execution complete; ticks="+std::to_string(ticks.load())+" imports="+std::to_string(imports)+" elapsed_us="+std::to_string(measured_us)+" registry_final=0; guest RAM=33554432 EDRAM=2097152; process RSS unmeasured");
        } catch(const std::exception& e){fail(e.what());} catch(...){fail("unknown simulation exception");}
        if(!devices.close_pad()){clean=false;fail("pad close failed");}
        audio_queue.close();file_queue.close();done=true;beat(0);running[0]=false;
    }
    void sound() {
        running[1]=true;beat(1);bool opened=false;
        try {
            opened=devices.open_audio();event("audio-open rc="+std::to_string(devices.audio_status()));require(opened,"audio open failed");
            IntegrationResampler resampler;AudioPacket input;std::vector<float> pending;
            while(audio_queue.pop(input)) {
                require(input.sequence==audio_blocks+1 && input.hash==checksum(input.samples.data(),input.samples.size()*2),"audio shared-state sequence/checksum mismatch");
                auto converted=resampler.append(input.samples);pending.insert(pending.end(),converted.begin(),converted.end());
                require(std::all_of(converted.begin(),converted.end(),[](float x){return x>=-0.04f&&x<=0.04f;}),"audio peak exceeded 0.04");
                std::size_t at=0;
                while(pending.size()-at>=512){const bool output=devices.output_audio(pending.data()+at,256);if(audio_blocks==0||!output)event("audio-output rc="+std::to_string(devices.audio_status()));require(output,"native audio output failed");at+=512;beat(1);}
                pending.erase(pending.begin(),pending.begin()+at);++audio_blocks;beat(1);
            }
            if(!pending.empty()){pending.resize(512,0);require(devices.output_audio(pending.data(),256),"audio tail failed");beat(1);}
            require(audio_blocks==options.ticks && resampler.input_frames()==std::uint64_t(options.ticks)*1470 &&
                resampler.output_frames()+2>=std::uint64_t(options.ticks)*1600,"audio coverage/rational sample count");
            audio=true;event("audio complete; persistent rational 44100->48000 phase; peak<=0.04; all blocks drained");
        }catch(const std::exception& e){fail(e.what());}catch(...){fail("unknown audio exception");}
        if(opened){const bool closed=devices.close_audio();event("audio-close rc="+std::to_string(devices.audio_status()));if(!closed){clean=false;fail("audio close failed");}}
        beat(1);running[1]=false;
    }
    void checkpoint() {
        running[2]=true;beat(2);
        try {
            IntegrationSnapshot s;
            // File worker legitimately waits ten seconds between checkpoints; wake
            // every five seconds and report the completed bounded queue wait.
            for(;;) {
                std::unique_lock lock(file_queue.mutex);
                file_queue.wake.wait_for(lock,std::chrono::seconds(5),[&]{return file_queue.closed||!file_queue.items.empty();});
                if(file_queue.items.empty()) {beat(2);if(file_queue.closed)break;continue;}
                s=file_queue.items.front();file_queue.items.pop_front();lock.unlock();
                require(integration_checkpoint(paths,s,options.fault==IntegrationFault::File),"checkpoint write/fsync/rename/read/oracle/cleanup failed");
                ++checkpoints;beat(2);event("checkpoint sequence="+std::to_string(s.sequence)+" payload=44 bytes PASS cleanup=PASS");
            }
            require(checkpoints==6,"mandatory six checkpoints omitted");files=true;
        }catch(const std::exception& e){fail(e.what());}catch(...){fail("unknown file exception");}
        beat(2);running[2]=false;
    }
};
IntegrationSession::IntegrationSession(const TitlePaths& p,Log& l,IntegrationDevices& d,unsigned c,IntegrationOptions o):impl_(std::make_unique<Impl>(p,l,d,c,o)){}
IntegrationSession::~IntegrationSession(){if(impl_->simulation.joinable()||impl_->audio_worker.joinable()||impl_->file_worker.joinable()) finish(false);}
void IntegrationSession::start(){
    auto& s=*impl_;s.running[3]=true;s.beat(3);
    try {s.audio_worker=std::thread([&]{s.sound();});s.file_worker=std::thread([&]{s.checkpoint();});s.simulation=std::thread([&]{s.simulate();});}
    catch(...){s.abort=true;s.audio_queue.close();s.file_queue.close();s.fail("thread creation failed");s.done=true;}
}
bool IntegrationSession::frame(IntegrationSnapshot& snapshot){
    auto& s=*impl_;s.beat(3);
    ++s.rendered_frames;
    {std::lock_guard lock(s.publication);snapshot=s.snapshot;}
    {std::lock_guard lock(s.events_mutex);for(const auto& e:s.events)s.log.write("[integration] cycle=%u %s",s.cycle,e.c_str());s.events.clear();}
    const auto now=now_us();
    if(now-s.last_log>=5000000){s.last_log=now;
        unsigned aq=0,fq=0;
        {std::lock_guard lock(s.audio_queue.mutex);aq=s.audio_queue.items.size();}
        {std::lock_guard lock(s.file_queue.mutex);fq=s.file_queue.items.size();}
        s.log.write("[integration] cycle=%u ticks=%u frames=%u readbacks=%u audio_blocks=%u checkpoints=%u registry=%u peak=%u queues=%u/%u progress_us=%llu failure=%u",
            s.cycle,s.ticks.load(),s.rendered_frames,s.pixels,s.audio_blocks.load(),s.checkpoints.load(),s.registrations.load(),s.registrations_peak.load(),aq,fq,static_cast<unsigned long long>(now),s.failed.load());
        s.log.write("[integration-state] sequence=%u module=%u guest=%u expected=%u tag=%u buttons=%#x xy=%d,%d connected=%u PCM_checksum=%#x payload_checksum=%#x",
            snapshot.sequence,snapshot.module,snapshot.guest,expected_guest(snapshot.module,input_value(snapshot)),snapshot.tag,snapshot.buttons,
            snapshot.x,snapshot.y,snapshot.connected,snapshot.audio_hash,snapshot.data_hash);
#if YAKUMO_NATIVE
        unsigned live=0,cached=0;ps5_thread_stacks(&live,&cached);s.log.write("[integration] stacks_live=%u stacks_cached=%u (SDK pool counts, not RSS)",live,cached);
#endif
    }
    return !s.done.load()&&!s.failed.load();
}
void IntegrationSession::rendered(bool pass){auto& s=*impl_;if(pass)++s.pixels;else s.fail("graphics pixel/API error");s.beat(3);}
IntegrationResult IntegrationSession::finish(bool graphics){
    auto& s=*impl_;s.abort=true;
    if(s.simulation.joinable())s.simulation.join();
    s.audio_queue.close();s.file_queue.close();
    if(s.audio_worker.joinable())s.audio_worker.join();if(s.file_worker.joinable())s.file_worker.join();
    IntegrationSnapshot final;frame(final);
    if(s.options.fault==IntegrationFault::Cleanup){s.clean=false;s.fail("injected cleanup failed");}
    const bool ok=!s.failed.load();s.running[3]=false;s.beat(3);
    IntegrationResult result{s.execution.load()&&ok,s.controls.load()&&ok,s.audio.load()&&ok,s.files.load()&&ok,
        graphics&&s.pixels>0&&ok,s.clean.load()&&ok};
    s.log.write("[integration-cycle] cycle=%u execution=%s controls=%s audio=%s files=%s graphics=%s cleanup=%s integration=%s audio_queue_peak=%u file_queue_peak=%u ticks=%u",
        s.cycle,result.execution?"PASS":"FAIL",result.controls?"PASS":"FAIL",result.audio?"PASS":"FAIL",result.files?"PASS":"FAIL",
        result.graphics?"PASS":"FAIL",result.cleanup?"PASS":"FAIL",result.passed()?"PASS":"FAIL",s.audio_queue.peak,s.file_queue.peak,s.ticks.load());
    return result;
}
std::uint64_t IntegrationSession::heartbeat(unsigned worker)const{return impl_->progress[worker].load();}
bool IntegrationSession::active(unsigned worker)const{return impl_->running[worker].load();}
}
