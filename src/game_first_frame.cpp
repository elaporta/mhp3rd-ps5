// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_files.hpp"
#include "game_platform.hpp"
#include "game_worker.hpp"
#include "game_bootstrap.hpp"
#include "game_import.hpp"
#include "game_session.hpp"
#include "game_observer.hpp"
#include "game_log.hpp"
#include "text_input_service.hpp"
#include "game_text_background.hpp"
#include "game_text_observer.hpp"
#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#if YAKUMO_GAME_PERFORMANCE
#include "game_performance.hpp"
#endif
#include "audio/atrac_decoder.hpp"
#include "movie/avc_decoder.hpp"
#include "mhp3rd_profile.hpp"
#include "app_paths.hpp"
#include "install/user_data.hpp"
#include "install/game_identity.hpp"
#include "hle/hle_common.hpp"
#include "fonts/game_font.hpp"
#include "settings/settings.hpp"
#if YAKUMO_GAME_PERFORMANCE
#include "game_camera_setup.hpp"
#endif
#include "psprecomp/interpreter.hpp"
#include "psprecomp/sha256.hpp"
#include "probe.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <limits>
#include <pthread.h>
#include <thread>
#include <unordered_map>
#ifndef YAKUMO_GAME_BUILD
#define YAKUMO_GAME_BUILD "game-first-frame-0.2.0-r2"
#endif
#ifndef YAKUMO_TITLE_ID
#define YAKUMO_TITLE_ID "PPSA99373"
#endif
#ifndef YAKUMO_NATIVE
#define YAKUMO_NATIVE 0
#endif
#ifndef YAKUMO_GAME_MENU
#define YAKUMO_GAME_MENU 0
#endif
#ifndef YAKUMO_GAME_PERFORMANCE
#define YAKUMO_GAME_PERFORMANCE 0
#endif
#ifndef YAKUMO_AOT_LABEL
#define YAKUMO_AOT_LABEL "O0"
#endif
#if YAKUMO_NATIVE
#include <pthread_np.h>
extern "C" int sceKernelDebugOutText(int,const char*);
extern "C" int sceSystemServiceLoadExec(const char*,const char*[]);
#endif
namespace {
namespace files=yakumo::game_files;
namespace platform=yakumo::game_platform;
std::atomic<bool> done{};
std::atomic<std::uint64_t> heartbeat{};
std::atomic<std::uint64_t> cleanup_started{};
bool watchdog_failed=false;
std::chrono::steady_clock::time_point deadline;
std::unordered_map<std::uint32_t,psprecomp::PspImport> imports;
std::uint64_t calls{},ge_lists{},flips{},reads{},bytes{},captures{},captured_frame{};
std::uint64_t mpeg_calls{},atrac_calls{};
files::path capture_folder;
int worker_result=1;
bool checkpoint_mode=!YAKUMO_GAME_MENU;
constexpr const char* checkpoint_stop=YAKUMO_GAME_MENU?"First-frame regression checkpoint (150 guest frames)":"First-frame checkpoint (150 guest frames; codecs pending)";
constexpr const char* session_stop="Bounded menu session completed (navigation unverified)";
unsigned run_seconds=120;

class RedirectLog {
    std::streambuf* out_;std::streambuf* err_;
public:
    explicit RedirectLog(yakumo::GameLog& log):out_(std::cout.rdbuf(&log)),err_(std::cerr.rdbuf(&log)){}
    ~RedirectLog(){std::cout.flush();std::cerr.flush();std::cout.rdbuf(out_);std::cerr.rdbuf(err_);}
};
void dispatch_import(psprecomp::Runtime& runtime,psprecomp::AllegrexContext& ctx){
    const auto& import=imports.at(ctx.pc);++calls;
    if(import.library=="sceMpeg"){
        ++mpeg_calls;
        if(mpeg_calls<=64||mpeg_calls%4096==0)std::cout<<"[game-mpeg] calls="<<mpeg_calls<<" nid="<<std::hex<<import.nid<<" pc="<<ctx.pc<<std::dec<<" thread="<<psprecomp::runtime_thread_name()<<'\n';
    }
    if(import.library=="sceAtrac3plus")++atrac_calls;
    if(calls<=28)std::cout<<"[game-call] #="<<calls<<" pc="<<std::hex<<ctx.pc<<" library="<<import.library<<" nid="<<import.nid<<" a0="<<ctx.gpr[4]<<" a1="<<ctx.gpr[5]<<" a2="<<ctx.gpr[6]<<" a3="<<ctx.gpr[7]<<" ra="<<ctx.gpr[31]<<std::dec<<" thread="<<psprecomp::runtime_thread_name()<<'\n';
    const auto token=psprecomp::capture_runtime_execution_context();
    if(import.library=="sceDisplay"&&import.nid==0x289D82FEu)++flips;
    if(import.library=="sceGe_user"&&import.nid==0xAB49E76Au)++ge_lists;
    const bool reading=import.library=="IoFileMgrForUser"&&import.nid==0x6A638D83u;
    runtime.invoke_import(import.library,import.nid,ctx);
    if(reading&&static_cast<std::int32_t>(ctx.gpr[2])>0){++reads;bytes+=ctx.gpr[2];}
    yakumo::game_import::complete(ctx,token,import.stub_address);
    if(import.library=="sceDisplay"&&import.nid==0x289D82FEu){
        // Completed guest flip: never read back a partially recorded frame.
        // Diagnose the game image independently of the system compositor.
        static yakumo::game_text_background::Probe probe;
        static std::uint64_t observed{};
        namespace evidence=yakumo::game_text_observer;
        if(observed!=evidence::request){observed=evidence::request;probe.begin(observed,evidence::opened_ms);}
        if(const auto phase=probe.due(yakumo::text_input::service().active(),platform::milliseconds())){
            if(auto* renderer=mhp3rd::active_renderer()){
                std::vector<std::uint8_t> pixels;std::uint32_t width{},height{};
                std::optional<yakumo::game_text_background::Pixels> measured;
                if(renderer->read_frame(pixels,width,height))measured=yakumo::game_text_background::measure(pixels,width,height);
                std::cout<<"[osk-background] request="<<observed<<" phase="<<(*phase==yakumo::game_text_background::Phase::Open?"open":"closed")
                          <<" source=presentation-source-before-compositor readback="<<bool(measured);
                if(measured)std::cout<<" width="<<width<<" height="<<height<<" nonblack_percent="<<measured->nonblack_percent()
                                    <<" mean_rgb="<<measured->mean_rgb()<<" rgb_hash="<<std::hex<<measured->hash<<std::dec;
                std::cout<<" final_scanout=UNMEASURED\n";
            }
        }
    }
}
void dispatch_bound(psprecomp::Runtime& runtime,psprecomp::AllegrexContext&,std::uint32_t,int){
    heartbeat=platform::milliseconds();
    static std::uint64_t next_text_tick{};
    if(heartbeat.load()>=next_text_tick){
        next_text_tick=heartbeat.load()+16;
        yakumo::text_input::service().tick();
        namespace evidence=yakumo::game_text_observer;
        if(!evidence::notice.empty()){
#if YAKUMO_NATIVE
            yakumo::ps5::notify_user(evidence::notice.c_str());
#endif
            std::cerr<<"[osk-native] notice="<<evidence::notice<<'\n';evidence::notice.clear();
        }
        if(!evidence::expected.empty()&&!evidence::matched){
            mhp3rd::game::GuestRam ram(runtime.memory());
            const auto actual=mhp3rd::game::hunter_name(ram);
            if(!actual.empty()&&(actual==evidence::expected||!evidence::mismatch_logged)){
                evidence::matched=actual==evidence::expected;evidence::mismatch_logged=true;
                std::cout<<"[osk-character] expected=\""<<evidence::expected<<"\" actual=\""<<actual
                         <<"\" match="<<evidence::matched<<" source=original-game-ram\n";
            }
        }
    }
    if(platform::audio_error()<0)throw std::runtime_error("Native PCM device failed rc="+std::to_string(platform::audio_error()));
    if(auto* renderer=mhp3rd::active_renderer()){
        // Movie frames are CPU-written buffers and need not increment the GE
        // draw-frame counter. Guest flips cover both GE and decoded movies.
        const auto frame=checkpoint_mode?renderer->frames_presented():flips;
        if(frame>captured_frame){
            captured_frame=frame;
            const bool sample= YAKUMO_GAME_PERFORMANCE ? (captures==0) : captures==0||frame<=3||frame==10||
                (frame<=300&&frame%30==0)||frame==201||(!checkpoint_mode&&frame%300==0&&captures<96);
#if YAKUMO_NATIVE
            const bool capture=sample;
#else
            const bool capture=sample||(checkpoint_mode&&frame<=300)||
                (YAKUMO_GAME_PERFORMANCE&&std::getenv("YAKUMO_QA_CAPTURES")&&(frame==30||frame==60));
#endif
            if(renderer->draws_submitted()>0&&capture){
                const auto filename=capture_folder/("frame-"+std::to_string(frame)+".bmp");
                if(!renderer->capture_frame(filename))throw std::runtime_error("Game frame capture failed");
                ++captures;std::cout<<"[game-capture] frame="<<frame<<" draws="<<renderer->draws_submitted()<<" path="<<filename.string()<<" correctness=UNVERIFIED\n";
            }
        }
        if(checkpoint_mode&&frame>=150&&captures>0)runtime.stop(checkpoint_stop);
    }
    if(run_seconds && std::chrono::steady_clock::now()>=deadline)runtime.stop(checkpoint_mode?"First-frame run time limit (120 seconds)":session_stop);
}
void* execute(void*){
    try{
#if YAKUMO_NATIVE
        struct StackApi {
            int init(pthread_attr_t* p){return pthread_attr_init(p);}
            int query(pthread_attr_t* p){return pthread_attr_get_np(pthread_self(),p);}
            int size(pthread_attr_t* p,std::size_t* n){return pthread_attr_getstacksize(p,n);}
            int destroy(pthread_attr_t* p){return pthread_attr_destroy(p);}
        } api;
        const auto stack=yakumo::game_worker::inspect_stack(api);
        std::cout<<"[game-worker] actual_stack="<<stack.bytes<<" init_rc="<<stack.init<<" attr_rc="<<stack.query<<" size_rc="<<stack.size<<" destroy_rc="<<stack.destroy<<"\n";
        if(!stack.valid())throw std::runtime_error("Native execution stack validation failed");
#endif
        const auto root=files::root();
        auto executable=root/"yakumo/game/EBOOT.ELF",disc=root/"yakumo/game/game.iso";
        if(psprecomp::sha256_file(executable)!=mhp3rd::install::kExecutableSha256)throw std::runtime_error("Executable SHA-256 mismatch");
        setenv("MHP3RD_STRICT_HLE","1",1);setenv("PSPRECOMP_INTERPRETER_BUDGET","4096",1);
        mhp3rd::install::set_data_directory_override(root/"yakumo/config");
        mhp3rd::set_bundled_resource_directory(root/"yakumo/assets");
        auto& settings=mhp3rd::settings::current();
#if !YAKUMO_GAME_PERFORMANCE
        settings.internal_scale=1;settings.texture_pack=false;settings.sharp_textures=false;settings.sharp_screen=false;
        settings.frame_rate=mhp3rd::settings::FrameRate::Fps30;settings.frame_rate_auto=false;settings.touch_controls=false;
#else
        settings.touch_controls=false;
        std::cout<<"[ps5-controls] configurable=L2,R2,touchpad,L1,R1,L3,R3 menu=L3+R3-priority"
            <<" defaults=R2:Select,L2:fast-forward,L1:L,R1:R,touchpad:hide_hud"
            <<" speed="<<settings.fast_forward_speed<<" mode="<<static_cast<int>(settings.fast_forward)<<"\n";
        setenv("MHP3RD_TRACE_INTERPOLATION","1",0);
        setenv("MHP3RD_TRACE_RENDER","1",0);
        std::cout<<"[ps5-video] scale="<<settings.internal_scale<<" hd="<<settings.texture_pack
            <<" frame_rate="<<static_cast<int>(settings.frame_rate)<<" automatic="<<settings.frame_rate_auto
            <<" config=yakumo/config/settings.ini aot="<<YAKUMO_AOT_LABEL<<" scanout_timing=UNAVAILABLE\n";
#endif
        settings.font=(root/"yakumo/assets/fonts/NotoSansCJKjp-Regular.otf").string();
        if(!mhp3rd::fonts::ready())throw std::runtime_error("Japanese game font unavailable");
        const bool atrac=mhp3rd::audio::AtracDecoder::available(),avc=mhp3rd::movie::AvcDecoder::available();
        std::cout<<"[game-services] font="<<mhp3rd::fonts::active_name()<<" compressed_audio="<<(atrac?"AVAILABLE":"UNAVAILABLE")
            <<" movie_codec="<<(avc?"AVAILABLE":"UNAVAILABLE")<<" desktop_menu=UNAVAILABLE\n";
        if(!checkpoint_mode&&(!atrac||!avc))throw std::runtime_error("Menu session requires actual AVC/ATRAC decoders");
        if(!platform::initialize_devices())throw std::runtime_error("Native device initialization failed");
        mhp3rd::ensure_renderer();
        auto elf=psprecomp::Elf32Image::from_file(executable);
        psprecomp::Runtime runtime(elf.required_ram_size());
        psprecomp::register_generated_functions(runtime);
        runtime.nids().load_csv(root/"yakumo/assets/nids.csv");
        runtime.set_game_root(root/"yakumo/game");
        const auto relocated=elf.load_and_relocate(runtime.memory());
        const auto module=elf.find_module_info(runtime.memory());
        if(!module||relocated.unsupported||relocated.invalid)throw std::runtime_error("Invalid executable relocation");
        for(const auto& import:elf.scan_imports(runtime.memory(),*module)){imports.emplace(import.stub_address,import);runtime.register_function(import.stub_address,dispatch_import,import.library);}
        mhp3rd::ProfilePaths paths{disc,root/"yakumo/saves/ms0"};
        mhp3rd::install_profile(runtime,elf,paths);
#if YAKUMO_GAME_PERFORMANCE
        if(!yakumo::prepare_native_camera(runtime))throw std::runtime_error("Native camera/aspect signature validation failed");
#endif
        psprecomp::reset_interpreter_stats();psprecomp::set_interpreter_fallback_enabled(true);
        psprecomp::set_runtime_pre_dispatch_hook(dispatch_bound);
        runtime.run(runtime.cpu().pc,checkpoint_mode?10000000:YAKUMO_GAME_PERFORMANCE?std::numeric_limits<std::size_t>::max():1000000000);
        psprecomp::set_runtime_pre_dispatch_hook(nullptr);
        const auto stats=psprecomp::interpreter_stats();
        std::cout<<"[game-first-frame] calls="<<calls<<" ge_lists="<<ge_lists<<" framebuffer_requests="<<flips<<" disc_reads="<<reads<<" read_bytes="<<bytes<<" interpreted="<<stats.instructions<<" captures="<<captures<<" native_presents="<<platform::present_count()<<" stop="<<runtime.stop_reason()<<" first_correct_frame=UNVERIFIED\n";
        std::cout<<"[game-codecs] mpeg_calls="<<mpeg_calls<<" atrac_calls="<<atrac_calls<<" menu_navigation=UNVERIFIED\n";
        worker_result=yakumo::game_session::successful_stop(runtime.stop_reason(),checkpoint_mode,run_seconds!=0)&&captures>0?0:1;
        if(!checkpoint_mode)std::cout<<"[game-menu] session_status="<<worker_result<<" menu_navigation=UNVERIFIED\n";
#if YAKUMO_NATIVE
        if(platform::present_count()==0)worker_result=1;
        if(worker_result==0&&checkpoint_mode){
            std::cout<<"[game-view] Holding the game's last frame for 20 seconds; MPEG/ATRAC remain unavailable\n";
            for(unsigned i=0;i<20;++i){heartbeat=platform::milliseconds();std::this_thread::sleep_for(std::chrono::seconds(1));}
        }
#endif
    }catch(const std::exception& error){std::cerr<<"[game-first-frame] FAIL "<<error.what()<<'\n';worker_result=1;}
    psprecomp::set_runtime_pre_dispatch_hook(nullptr);
    cleanup_started=platform::milliseconds();heartbeat=cleanup_started.load();
    try { yakumo::text_input::service().shutdown(); }
    catch(const std::exception& error){std::cerr<<"[game-cleanup] text input FAIL "<<error.what()<<'\n';worker_result=1;}
    std::cout<<"[game-cleanup] renderer begin cleanup_limit_seconds=120 no_progress_seconds=30\n";
    try { if(auto* renderer=mhp3rd::active_renderer())renderer->shutdown(); }
    catch(const std::exception& error){std::cerr<<"[game-cleanup] renderer FAIL "<<error.what()<<'\n';worker_result=1;}
    std::cout<<"[game-cleanup] renderer complete; devices begin\n";
    platform::shutdown_devices();
    heartbeat=platform::milliseconds();
    std::cout<<"[game-cleanup] devices complete; permissions begin\n";
    try{
#if YAKUMO_NATIVE
        files::repair_title_permissions([]{heartbeat=platform::milliseconds();});
#else
        files::repair_generated_permissions([]{heartbeat=platform::milliseconds();});
#endif
        std::cout<<"[game-permissions] files=0666 directories=0777 programs=0777 policy=native-loadables-v1 repair=PASS\n";
    }
    catch(const std::exception& error){std::cerr<<"[game-permissions] FAIL "<<error.what()<<'\n';worker_result=1;}
    if(platform::audio_error()<0)worker_result=1;
    std::cout<<"[game-cleanup] worker completed status="<<worker_result<<"\n";
    done=true;return nullptr;
}
}
int main(int argc,char** argv){
    try{
        unsigned host_seconds=0;
#if YAKUMO_NATIVE
        yakumo::ps5::Log early;yakumo::ps5::TitlePaths paths;
        early.write("[game-bootstrap] identity=%s phase=entry",YAKUMO_GAME_BUILD);
        const std::string title_root=std::string("/data/homebrew/")+YAKUMO_TITLE_ID;
        const char* candidates[]={title_root.c_str(),"/app0"};
        if(!paths.resolve(candidates,2,YAKUMO_TITLE_ID,&early)||!paths.prepare_logs(&early))throw std::runtime_error(std::string("Title identity/log-directory validation failed: ")+paths.diagnostics);
        files::set_root(paths.root);
#else
        if(argc<2||argc>4){std::cerr<<"Usage: game_first_frame <private title root> [--checkpoint=150] [--seconds=10..600]\n";return 2;}
        for(int i=2;i<argc;++i){
            const std::string option=argv[i];
            if(option=="--checkpoint=150")checkpoint_mode=true;
            else if(option.starts_with("--seconds=")){
                const auto value=option.substr(10);std::size_t end=0;
                const auto parsed=std::stoul(value,&end);
                if(end!=value.size()||parsed<10||parsed>600)throw std::runtime_error("Host duration must be 10..600 seconds");
                host_seconds=static_cast<unsigned>(parsed);
            }else throw std::runtime_error("Unknown host option");
        }
        files::set_root(argv[1]);
#endif
        run_seconds=checkpoint_mode?120:host_seconds?host_seconds:(YAKUMO_NATIVE&&YAKUMO_GAME_PERFORMANCE)?0:600;
        FILE* log_file=yakumo::game_bootstrap::open_log();
        if(!log_file)throw std::runtime_error("Cannot append bootstrap log errno="+std::to_string(errno));
        const bool sync_log=std::getenv("YAKUMO_SYNC_LOG")!=nullptr;
#if YAKUMO_NATIVE
        auto kernel_log=+[](const char* line){sceKernelDebugOutText(0,line);};
#else
        void (*kernel_log)(const char*)=nullptr;
#endif
        yakumo::GameLog log(log_file,stdout,sync_log,kernel_log);RedirectLog redirect(log);
        std::cout<<"[game-boot] identity="<<YAKUMO_GAME_BUILD<<" title="<<YAKUMO_TITLE_ID<<" root="<<files::root().string()
            <<" limit_seconds="<<run_seconds<<" checkpoint="<<(checkpoint_mode?150:0)<<" main_stack=67108864\n";
        const char* phase="permission-repair";
        try{
        std::cout<<"[game-bootstrap] phase="<<phase<<" begin\n";
#if YAKUMO_NATIVE
        files::repair_title_permissions();
#else
        files::repair_generated_permissions();
#endif
        std::cout<<"[game-bootstrap] phase="<<phase<<" complete\n";
        phase="title-storage";std::cout<<"[game-bootstrap] phase="<<phase<<" begin\n";
#if YAKUMO_NATIVE
        if(!paths.prepare(&early))throw std::runtime_error(std::string("Title storage validation failed: ")+paths.diagnostics);
        files::repair_title_permissions(); // Include directories just created by prepare().
#endif
        files::create_directories(files::root()/"yakumo/cache/first-frame");
        const auto run=std::to_string(platform::milliseconds());
        for(unsigned attempt=0;;++attempt){
            capture_folder=files::root()/"yakumo/cache/first-frame"/(run+(attempt?"-"+std::to_string(attempt):""));
            std::error_code ec;
            if(files::create_directory(capture_folder,ec))break;
            if(ec)throw std::runtime_error("Cannot reserve capture folder: "+ec.message());
        }
        files::create_directories(files::root()/"yakumo/saves/ms0");
#if YAKUMO_GAME_PERFORMANCE
        const auto config=mhp3rd::install::read_settings_file(files::root()/"yakumo/config");
        if(const auto it=config.find("ps5.sync_log");it!=config.end()) {
            if(it->second!="0"&&it->second!="1")throw std::runtime_error("ps5.sync_log must be 0 or 1");
            log.set_synchronous(it->second=="1");
        }
        if(const auto it=config.find("ps5.qa_seconds");it!=config.end()) {
            const auto seconds=yakumo::game_session::qa_seconds(it->second);
#if YAKUMO_NATIVE
            if(!checkpoint_mode)run_seconds=seconds;
#else
            (void)seconds; // Automated host runs always retain their own bound.
#endif
        }
        if(const auto it=config.find("ps5.cache_profile");it!=config.end()) {
            if(!yakumo::game_performance::valid_cache_profile(it->second))throw std::runtime_error("Invalid ps5.cache_profile: use 1..32 lowercase letters, digits, '-' or '_'");
            yakumo::game_performance::cache_profile=it->second;
        }
        const auto cache=yakumo::game_performance::cache_directory();
        files::create_directories(cache/"radv");
        setenv("MESA_SHADER_CACHE_DIR",(cache/"radv").c_str(),1);
        std::cout<<"[ps5-cache] profile="<<yakumo::game_performance::cache_profile<<" existing_caches=preserved\n";
#else
        setenv("MESA_SHADER_CACHE_DIR",(files::root()/"yakumo/cache/radv").c_str(),1);
#endif
        setenv("PS5_VIDEOOUT_PARAM_JSON",(files::root()/"sce_sys/param.json").c_str(),1);
        setenv("MHP3RD_GPU_DECODE","0",1);
        phase="worker-create";std::cout<<"[game-bootstrap] phase="<<phase<<" begin\n";
        std::cout<<"[ps5-session] mode="<<(checkpoint_mode?"checkpoint":run_seconds?"qa":"normal")
            <<" limit_seconds="<<run_seconds<<" no_progress_seconds=30 cleanup_seconds=120\n";
        deadline=run_seconds?std::chrono::steady_clock::now()+std::chrono::seconds(run_seconds):std::chrono::steady_clock::time_point{};
        heartbeat=platform::milliseconds();
        pthread_attr_t attributes;
        int rc=pthread_attr_init(&attributes);
        int create_cleanup=0;
        pthread_t worker{};
        if(!rc){
            rc=pthread_attr_setstacksize(&attributes,yakumo::game_worker::stack_bytes);
            if(!rc)rc=pthread_create(&worker,&attributes,execute,nullptr);
            create_cleanup=pthread_attr_destroy(&attributes);
        }
        if(rc){std::cerr<<"[game-worker-create] FAIL rc="<<rc<<" destroy_rc="<<create_cleanup<<"\n";throw std::runtime_error("Worker create/stack rc="+std::to_string(rc));}
        while(!done){
            const auto now=platform::milliseconds();
            const auto deadline_ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
            const auto* reason=yakumo::game_session::timeout(now,heartbeat.load(),deadline_ms,cleanup_started.load());
            if(reason&&!watchdog_failed){
                watchdog_failed=true;
                std::cerr<<"[game-watchdog] FAIL "<<reason<<"\n";
#if YAKUMO_NATIVE
                sceSystemServiceLoadExec("exit",nullptr);
                for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(250));
#endif
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        rc=pthread_join(worker,nullptr);
        std::cout<<"[game-worker-create] rc=0 destroy_rc="<<create_cleanup<<"\n";
        std::cout<<"[game-watchdog] joined="<<rc<<"\n";
        return rc||create_cleanup||watchdog_failed?1:worker_result;
        }catch(const std::exception& error){
            std::cerr<<"[game-bootstrap] FAIL phase="<<phase<<" errno="<<errno<<" reason="<<error.what()<<'\n';
#if YAKUMO_NATIVE
            char message[512];std::snprintf(message,sizeof(message),"%s: startup failed at %s. Check game-first-frame.log and klog.",YAKUMO_GAME_BUILD,phase);yakumo::ps5::notify_user(message);
#endif
            return 1;
        }
    }catch(const std::exception& error){
        std::fprintf(stderr,"[game-first-frame] bootstrap FAIL %s\n",error.what());
#if YAKUMO_NATIVE
        yakumo::ps5::Log early;early.write("[game-bootstrap] identity=%s FAIL before-log reason=%s",YAKUMO_GAME_BUILD,error.what());
        char message[512];std::snprintf(message,sizeof(message),"%s: %s. Capture klog.",YAKUMO_GAME_BUILD,error.what());yakumo::ps5::notify_user(message);
#endif
        return 1;
    }
}
