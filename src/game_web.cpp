// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_web.hpp"
#include "game_web_protocol.hpp"
#include "game_settings.hpp"
#include "game_files.hpp"
#include "game_text_observer.hpp"
#include "adhoc/client.hpp"
#include "settings/settings.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <thread>
#include <unistd.h>
#if YAKUMO_NATIVE
#include "adhoc_socket.hpp"
#include "game_web_native.hpp"
#if YAKUMO_WEB_HOST_WEAK_FIXTURE
#define WEB_IMPORT __attribute__((weak))
#else
#define WEB_IMPORT
#endif
extern "C" {
int sceNetBind(int,const sockaddr*,socklen_t) WEB_IMPORT;
int sceNetListen(int,int) WEB_IMPORT;
int sceNetAccept(int,sockaddr*,socklen_t*) WEB_IMPORT;
int sceSystemServiceLaunchWebBrowser(const char*,const void*) WEB_IMPORT;
int sysctl(const int*,unsigned,void*,std::size_t*,const void*,std::size_t) WEB_IMPORT;
}
#undef WEB_IMPORT
#endif
namespace yakumo::game_web {
#if YAKUMO_NATIVE
// Loose imports can remain zero on PS5 despite a valid converted import table.
// Volatile GOT reads prevent strong declarations folding the checks to true.
extern "C" __attribute__((noinline,visibility("hidden"))) void yakumo_read_native_web_imports(NativeApi* out) {
    decltype(NativeApi::bind) volatile bind=sceNetBind;
    decltype(NativeApi::listen) volatile listen=sceNetListen;
    decltype(NativeApi::accept) volatile accept=sceNetAccept;
    decltype(NativeApi::browser) volatile browser=sceSystemServiceLaunchWebBrowser;
    decltype(NativeApi::sysctl) volatile entropy=sysctl;
    *out={bind,listen,accept,browser,entropy};
}
#endif
namespace {
namespace menu = game_settings;
using Clock = std::chrono::steady_clock;
std::uint64_t millis() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
std::atomic<bool> running{}, connected{}, close_requested{}, editing_focus{}, launch_done{true};
std::atomic<bool> queued{};
std::atomic<std::uint64_t> last_ping{};
std::atomic<int> launch_result{};
std::thread worker, launcher;
Capture capture;
std::mutex mutex;
std::condition_variable changed;
struct Request { std::string route, body, response; bool pending{}, complete{}; std::uint64_t id{}; } request;
std::uint64_t sequence{};
std::string token;
std::map<std::string,std::string> assets;
bool unavailable_ready{};
#if YAKUMO_NATIVE
NativeApi native_api;
#endif
std::string revision(const menu::Entries& entries) {
    // Optimistic identity only, never used for authentication.
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto& [key,value] : entries) for (const auto& s : {key, std::string("="), value, std::string("\n")})
        for (unsigned char c : s) { hash ^= c; hash *= 1099511628211ull; }
    return std::to_string(hash);
}
std::string object(const menu::Entries& entries) {
    std::string out = "{"; bool first = true;
    for (const auto& [key,value] : entries) { if (!first) out += ','; first=false; out += quote(key)+":"+quote(value); }
    return out + '}';
}
std::string settings_json() {
    const auto disk = menu::read(), values = menu::values(disk);
    const auto current = mhp3rd::settings::to_entries(mhp3rd::settings::current());
    std::string out = "{\"ok\":true,\"revision\":"+quote(revision(disk))+",\"values\":"+object(values)+
        ",\"current\":"+object(current)+",\"catalog\":[";
    bool first = true;
    for (const auto& option : menu::catalog()) {
        if (!first) out += ','; first = false;
        out += "{\"key\":"+quote(option.key)+",\"label\":"+quote(option.label)+",\"section\":"+
            std::to_string(static_cast<int>(option.section))+",\"kind\":"+std::to_string(static_cast<int>(option.kind))+
            ",\"min\":"+std::to_string(option.minimum)+",\"max\":"+std::to_string(option.maximum)+
            ",\"initial\":"+quote(option.initial)+",\"locked\":"+(mhp3rd::settings::overridden_by(option.key.c_str())?"true":"false")+
            ",\"choices\":[";
        bool choice_first = true;
        for (const auto& value : option.choices) { if (!choice_first) out += ','; choice_first = false; out += quote(value); }
        out += "]}";
    }
    return out + "]}";
}
std::string handle(const std::string& route, const std::string& body) {
    if (route == "/api/settings") return settings_json();
    if (route == "/api/status") {
        const auto d = mhp3rd::adhoc::Client::get().diagnostics();
        return "{\"ok\":true,\"state\":"+std::to_string(static_cast<int>(d.state))+
            ",\"server\":"+quote(d.server_address.empty()?d.server:d.server_address)+",\"nickname\":"+quote(d.nickname)+
            ",\"group\":"+quote(d.group.value_or(""))+",\"peers\":"+std::to_string(d.peers.size())+
            ",\"error\":"+quote(d.last_error)+"}";
    }
    if (route == "/api/hud") return std::string("{\"ok\":")+(menu::toggle_hud()?"true":"false")+"}";
    if (route == "/api/save") {
        auto patch = form(body);
        const auto expected = menu::read();
        const auto id = patch.find("_revision");
        if (id == patch.end() || id->second != revision(expected)) throw std::runtime_error("El archivo cambió. Recarga los ajustes antes de guardar.");
        patch.erase(id);
        bool restore = false;
        if (auto it = patch.find("_controls_reset"); it != patch.end()) {
            if (it->second != "1") throw std::runtime_error("Invalid reset");
            restore = true; patch.erase(it);
        }
        menu::save(expected, patch, restore ? std::optional{menu::Section::Controls} : std::nullopt);
        return settings_json();
    }
    throw std::runtime_error("Unknown API route");
}
int socket_create() {
#if YAKUMO_NATIVE
    return adhoc_socket::create(AF_INET,SOCK_STREAM,IPPROTO_TCP);
#else
    return ::socket(AF_INET,SOCK_STREAM,0);
#endif
}
bool nonblocking(int fd) {
#if YAKUMO_NATIVE
    return adhoc_socket::set_nonblocking(fd);
#else
    const int flags=fcntl(fd,F_GETFL); return flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0;
#endif
}
void close_socket(int fd) {
#if YAKUMO_NATIVE
    adhoc_socket::close_socket(fd);
#else
    if(fd>=0) ::close(fd);
#endif
}
ssize_t receive(int fd,char* data,std::size_t size) {
#if YAKUMO_NATIVE
    return adhoc_socket::recv(fd,data,size,0);
#else
    return ::recv(fd,data,size,0);
#endif
}
ssize_t transmit(int fd,const char* data,std::size_t size) {
#if YAKUMO_NATIVE
    return adhoc_socket::send(fd,data,size,0);
#else
    return ::send(fd,data,size,MSG_NOSIGNAL);
#endif
}
bool idle_error() { return errno==EWOULDBLOCK || errno==EAGAIN || errno==EINTR; }
std::string owner_request(const std::string& route,const std::string& body) {
    std::unique_lock lock(mutex);
    request = {route,body,{},true,false,++sequence};
    queued=true;
    if (!changed.wait_for(lock,std::chrono::seconds(5),[]{return request.complete || !running;})) {
        request.pending=false; ++sequence; throw std::runtime_error("El juego no respondió a tiempo; recarga antes de volver a guardar.");
    }
    if (!request.complete) throw std::runtime_error("Server stopping");
    return request.response;
}
// One bounded request per connection, no keep-alive/pipelining, no filesystem paths.
void serve(int fd) {
    const auto deadline=Clock::now()+std::chrono::seconds(7);
    std::string bytes; std::size_t header_end=std::string::npos, length=0;
    std::string method,route; std::map<std::string,std::string> headers;
    int status=200; std::string payload,type="application/json; charset=utf-8";
    try {
        while(running && Clock::now()<deadline) {
            char buffer[4096]; const auto n=receive(fd,buffer,sizeof(buffer));
            if(n==0) return;
            if(n<0) { if(!idle_error()) return; std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            bytes.append(buffer,static_cast<std::size_t>(n));
            if(bytes.size()>32768) throw std::runtime_error("Request too large");
            if(header_end==bytes.npos) {
                header_end=bytes.find("\r\n\r\n");
                if(header_end==bytes.npos) { if(bytes.size()>8192) throw std::runtime_error("Headers too large"); continue; }
                if(header_end>8192) throw std::runtime_error("Headers too large");
                std::istringstream lines(bytes.substr(0,header_end)); std::string line,version,extra;
                std::getline(lines,line); std::istringstream first(line);
                if(!(first>>method>>route>>version) || (first>>extra) || version!="HTTP/1.1") throw std::runtime_error("Invalid HTTP request");
                while(std::getline(lines,line)) {
                    if(!line.empty() && line.back()=='\r') line.pop_back();
                    const auto split=line.find(':'); if(split==line.npos) throw std::runtime_error("Invalid header");
                    auto key=line.substr(0,split), value=line.substr(split+1);
                    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});
                    while(!value.empty() && (value.front()==' ' || value.front()=='\t')) value.erase(0,1);
                    if(!headers.emplace(key,value).second) throw std::runtime_error("Duplicate header");
                }
                if(headers["host"]!="127.0.0.1:8844" || headers.count("transfer-encoding")) throw std::runtime_error("Invalid host or encoding");
                if(headers.count("origin") && headers["origin"]!="http://127.0.0.1:8844") throw std::runtime_error("Invalid origin");
                if(headers.count("content-length")) {
                    const auto& value=headers.at("content-length");
                    if(value.empty() || value.size()>5) throw std::runtime_error("Invalid content length");
                    for(char c:value) { if(c<'0'||c>'9') throw std::runtime_error("Invalid content length"); length=length*10+c-'0'; }
                    if(length>24000) throw std::runtime_error("Body too large");
                }
                header_end+=4;
            }
            if(bytes.size()<header_end+length) continue;
            if(bytes.size()!=header_end+length) throw std::runtime_error("Pipelining rejected");
            break;
        }
        if(!running || header_end==bytes.npos || bytes.size()!=header_end+length) return;
        if(route.rfind("/api/",0)==0) {
            if(headers["x-yakumo-token"]!=token) throw std::runtime_error("Invalid session");
            const bool reading=route=="/api/settings" || route=="/api/status";
            if(method!=(reading?"GET":"POST")) throw std::runtime_error("Invalid API method");
            if(!reading && headers["content-type"]!="application/x-www-form-urlencoded") throw std::runtime_error("Invalid content type");
            if(route=="/api/ping") { last_ping=millis(); payload="{\"ok\":true}"; }
            else if(route=="/api/close") { close_requested=true; editing_focus=false; payload="{\"ok\":true}"; }
            else if(route=="/api/focus") {
                const auto values=form(bytes.substr(header_end,length));
                const auto found=values.find("editing");
                if(found==values.end() || (found->second!="0" && found->second!="1")) throw std::runtime_error("Invalid focus state");
                editing_focus=found->second=="1"; payload="{\"ok\":true}";
            }
            else payload=owner_request(route,bytes.substr(header_end,length));
        } else {
            if(method!="GET" || length) throw std::runtime_error("Invalid asset method");
            const auto found=assets.find(route);
            if(found==assets.end()) { status=404; payload="Not found"; type="text/plain"; }
            else { payload=found->second; type=route=="/"?"text/html; charset=utf-8":route=="/menu.js"?"text/javascript; charset=utf-8":"text/css; charset=utf-8"; }
        }
    } catch(const std::exception& error) { status=400; payload="{\"ok\":false,\"error\":"+quote(error.what())+"}"; }
    const std::string response="HTTP/1.1 "+std::to_string(status)+(status==200?" OK":status==404?" Not Found":" Bad Request")+
        "\r\nContent-Type: "+type+"\r\nContent-Length: "+std::to_string(payload.size())+
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n"+payload;
    std::size_t at=0;
    while(running && at<response.size() && Clock::now()<deadline) {
        const auto n=transmit(fd,response.data()+at,response.size()-at);
        if(n>0) at+=static_cast<std::size_t>(n);
        else if(n<0 && idle_error()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        else break;
    }
}
}
void start() {
    if(running) return;
    std::cout<<"[ps5-web-start] stage=assets begin\n"<<std::flush;
    assets.clear();
    for(const auto& [route,name]:std::map<std::string,std::string>{{"/","index.html"},{"/menu.css","menu.css"},{"/menu.js","menu.js"}}) {
        game_files::ifstream file(game_files::root()/"yakumo/assets/web"/name);
        if(!file) throw std::runtime_error("Missing web asset: "+name);
        std::string data((std::istreambuf_iterator<char>(file)),{});
        if(data.size()>262144) throw std::runtime_error("Web asset too large");
        assets[route]=std::move(data);
    }
    std::cout<<"[ps5-web-start] stage=assets complete count="<<assets.size()<<'\n'<<std::flush;
    unsigned char nonce[32];
#if YAKUMO_NATIVE
    yakumo_read_native_web_imports(&native_api);
    std::cout<<"[ps5-web-start] stage=imports bind="<<(native_api.bind!=nullptr)
        <<" listen="<<(native_api.listen!=nullptr)<<" accept="<<(native_api.accept!=nullptr)
        <<" browser="<<(native_api.browser!=nullptr)<<" sysctl="<<(native_api.sysctl!=nullptr)
        <<" binding=checked-global-got-r2\n"<<std::flush;
    if(const auto* missing=native_api.missing())throw std::runtime_error(std::string("Unbound web import: ")+missing);
    std::cout<<"[ps5-web-start] stage=entropy backend=kern.arandom begin\n"<<std::flush;
    if(!kernel_entropy(native_api,nonce,sizeof(nonce))) throw std::runtime_error("Web kernel entropy unavailable");
#else
    std::random_device random;for(auto& byte:nonce) byte=static_cast<unsigned char>(random());
#endif
    token.clear(); constexpr char hex[]="0123456789abcdef";
    for(const auto byte:nonce) { token+=hex[byte>>4]; token+=hex[byte&15]; }
    std::cout<<"[ps5-web-start] stage=listener begin\n"<<std::flush;
    int listener=socket_create();
    if(listener<0) throw std::runtime_error("Web socket creation failed");
    sockaddr_in address{};
#if YAKUMO_NATIVE && !defined(__linux__)
    address.sin_len=sizeof(address);
#endif
    address.sin_family=AF_INET; address.sin_port=htons(8844); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
#if YAKUMO_NATIVE
    const bool bound=native_api.bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && native_api.listen(listener,4)==0;
#else
    int reuse=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    const bool bound=::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && ::listen(listener,4)==0;
#endif
    if(!bound || !nonblocking(listener)) { close_socket(listener); throw std::runtime_error("Loopback web listener failed"); }
    capture={}; close_requested=false; editing_focus=false; last_ping=0; connected=true; running=true;
    try { worker=std::thread([listener]{
        while(running) {
#if YAKUMO_NATIVE
            const int fd=native_api.accept(listener,nullptr,nullptr);
#else
            const int fd=::accept(listener,nullptr,nullptr);
#endif
            if(fd<0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            if(nonblocking(fd)) {
#if YAKUMO_NATIVE
                adhoc_socket::tune(fd);
#endif
                serve(fd);
            }
            close_socket(fd);
        }
        close_socket(listener);
    }); } catch(...) {running=false;close_socket(listener);throw;}
    std::cout<<"[ps5-web] loopback=127.0.0.1:8844 chord=L3+R3 apply=manual-restart\n";
}
void initialize() noexcept {
    try {start();}
    catch(const std::exception& error) {
        stop();
        std::cerr<<"[ps5-web-start] optional-menu=unavailable game=continuing reason="<<error.what()<<'\n'<<std::flush;
        game_text_observer::notice="Menu web no disponible. El juego puede continuar; conserva el log.";
    }
}
void tick(std::uint64_t) {
    if(!running || !queued.exchange(false)) return;
    std::string route,body; std::uint64_t id{};
    {
        std::lock_guard lock(mutex);
        if(!request.pending) return;
        route=request.route; body=request.body; id=request.id; request.pending=false;
    }
    std::string response;
    try { response=handle(route,body); }
    catch(const std::exception& error) { response="{\"ok\":false,\"error\":"+quote(error.what())+"}"; }
    {
        std::lock_guard lock(mutex);
        if(id!=sequence) return;
        request.response=std::move(response); request.complete=true;
    }
    changed.notify_all();
}
bool suppress_sample(bool chord,bool circle,bool neutral,bool ime_busy,std::uint64_t now,bool analog_active) {
    if(!running) {
        if(!chord)unavailable_ready=true;
        if(chord&&unavailable_ready) {
            unavailable_ready=false;
            game_text_observer::notice="Menu web no disponible. Revisa game-first-frame.log.";
        }
        return chord;
    }
    if(close_requested.exchange(false)) capture.close();
    // Circle is the PS5 browser's close/back button when no editable control
    // owns focus. Consume it and return to gameplay after its release.
    if(capture.active && circle && !editing_focus.load()) capture.close();
    if(launch_done) if(const int rc=launch_result.exchange(0);rc!=0) {
        capture.close();std::cerr<<"[ps5-web] browser launch failed rc="<<rc<<'\n';
        game_text_observer::notice="No se pudo abrir el menu web. Suelta el mando y vuelve a pulsar L3 + R3.";
    }
    capture.ping=std::max(capture.ping,last_ping.load());
    bool launch=false;
    const bool suppressed=capture.sample(chord,neutral,ime_busy,now,launch,analog_active);
    if(launch) {
        if(!launch_done) { capture.close(); return true; }
        if(launcher.joinable()) launcher.join();
        launch_done=false;
        const std::string url=browser_url();
        launcher=std::thread([url]{
#if YAKUMO_NATIVE
            launch_result=native_api.browser(url.c_str(),nullptr);
#else
            launch_result=0;
#endif
            launch_done=true;
        });
    }
    return suppressed;
}
bool blocks_input() { return capture.active || capture.release; }
std::string browser_url() { return "http://127.0.0.1:8844/#"+token; }
void stop() noexcept {
    running=false; changed.notify_all();
    if(worker.joinable()) worker.join();
    if(launcher.joinable()) launcher.join();
    capture={}; assets.clear(); connected=false; editing_focus=false;
}
}
