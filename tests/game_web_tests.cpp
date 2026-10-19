// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_web.hpp"
#include "game_web_protocol.hpp"
#include "game_web_native.hpp"
#include "game_text_observer.hpp"
#include "game_settings.hpp"
#include "game_files.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#if YAKUMO_NATIVE
namespace web_fixture {
extern std::atomic<int> launches,launch_rc;
extern int fail_create,fail_bind,fail_listen,fail_nonblock,fail_entropy;
extern std::map<int,int> descriptors;
}
#endif
namespace {
void check(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
std::string exchange(const std::string& text) {
    const int fd=socket(AF_INET,SOCK_STREAM,0);
    check(fd>=0,"client socket");
    timeval timeout{10,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(8844);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"loopback connect");
    std::size_t at=0;
    while(at<text.size()) { auto n=send(fd,text.data()+at,text.size()-at,MSG_NOSIGNAL);check(n>0,"request send");at+=n; }
    std::string response;char buffer[4096];
    for(;;) {const auto n=recv(fd,buffer,sizeof(buffer),0);check(n>=0,"response receive");if(!n)break;response.append(buffer,n);}
    close(fd);return response;
}
}
int main(int argc,char** argv) {try {
    namespace web=yakumo::game_web;namespace menu=yakumo::game_settings;namespace settings=mhp3rd::settings;
    namespace fs=std::filesystem;
    bool launch=false;web::Capture capture;
    check(!capture.sample(true,false,false,0,launch)&&!launch,"held initial chord requires fresh release");
    capture.sample(false,true,false,1,launch);
    check(capture.sample(true,false,false,2,launch)&&launch,"fresh chord launches and consumes sample");
    check(capture.sample(true,false,false,10000,launch)&&!launch,"held chord never repeats");
    capture.sample(false,true,false,10001,launch);
    capture.ping=10001;
    check(capture.sample(true,false,false,10002,launch)&&capture.active,"live page retains input");
    capture.sample(false,true,false,14000,launch);
    check(capture.sample(true,false,false,14001,launch)&&!capture.active&&capture.release,"stale page explicit return");
    check(capture.sample(false,true,false,14002,launch)&&!capture.release,"neutral sample swallowed on return");
    check(!capture.sample(false,true,false,14003,launch),"input resumes after neutral");
    capture.sample(true,false,true,15000,launch);check(!launch,"IME blocks opening");
    check(web::quote("\"\n\\")=="\"\\\"\\u000a\\\\\"","JSON escaping");
    check(web::form("name=%C3%B1+ok").at("name")=="ñ ok","UTF-8 form round trip");
    for(const auto* bad:{"a=%","a=%GG","a=%00","a=1&a=2","missing"}) {
        bool rejected=false;try{web::form(bad);}catch(...){rejected=true;}check(rejected,"bad form rejected");
    }
    const auto root=fs::temp_directory_path()/("yakumo-web-"+std::to_string(getpid()));
    fs::create_directories(root/"yakumo/config");fs::create_directories(root/"yakumo/assets/web");
    for(const char* name:{"index.html","menu.css","menu.js"}) fs::copy_file(fs::path(WEB_ASSETS)/name,root/"yakumo/assets/web"/name);
    yakumo::game_files::set_root(root);mhp3rd::install::set_data_directory_override(root/"yakumo/config");
    const auto live=settings::to_entries(settings::current());
    menu::Entries initial{{"settings.version","2"},{"input.name","Hunter"},{"network.nickname","OldAlias"},{"video.internal_scale","3"},{"future.key","preserved"}};
    mhp3rd::install::write_settings_file(root/"yakumo/config",initial);
#if YAKUMO_WEB_HOST_WEAK_FIXTURE
    bool absent=false;
    try{web::start();}catch(const std::exception& error){absent=std::string(error.what()).find("Unbound web import: sceNetBind")!=std::string::npos;}
    check(absent,"missing real GOT entry is detected without a null call");
    web::initialize();
    check(!web::blocks_input()&&!yakumo::game_text_observer::notice.empty(),"optional menu failure leaves gameplay running with a notice");
    check(!web::suppress_sample(false,false,true,false,1)&&web::suppress_sample(true,false,false,false,2),"failed menu consumes only its reserved chord");
    check(web_fixture::descriptors.empty(),"no listener is created with incomplete imports");
    web::stop();fs::remove_all(root);std::cout<<"unbound native GOT regression and optional startup PASS\n";return 0;
#endif
#if YAKUMO_NATIVE
    for(auto failure:{&web_fixture::fail_create,&web_fixture::fail_bind,&web_fixture::fail_listen,&web_fixture::fail_nonblock,&web_fixture::fail_entropy}) {
        *failure=1;bool rejected=false;try{web::start();}catch(...){rejected=true;}*failure=0;
        check(rejected&&web_fixture::descriptors.empty(),"listener failure has no native handle leak");
    }
    web_fixture::fail_entropy=1;web::initialize();web_fixture::fail_entropy=0;
    check(!web::blocks_input()&&web_fixture::descriptors.empty(),"entropy failure cannot stop gameplay or leak a listener");
#endif
    web::start();
    web::suppress_sample(false,false,true,false,1);
    check(web::suppress_sample(true,false,false,false,2)&&web::blocks_input(),"real service captures opening chord");
#if YAKUMO_NATIVE
    for(int i=0;i<100&&web_fixture::launches==0;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(web_fixture::launches==1,"browser direct import receives session URL");
    web::suppress_sample(true,false,false,false,3);check(web_fixture::launches==1,"held service chord never reopens browser");
#endif
    if(argc>1) {
        std::cout<<web::browser_url()<<std::endl;
        for(int i=0;i<30000;++i) {web::tick(i);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        web::stop();fs::remove_all(root);return 0;
    }
    auto request=[&](const std::string& route,const std::string& body="",bool post=false,const std::string& extra="",bool auth=true) {
        const auto url=web::browser_url();const auto token=url.substr(url.find('#')+1);
        const std::string text=std::string(post?"POST ":"GET ")+route+" HTTP/1.1\r\nHost: 127.0.0.1:8844\r\n"+
            (auth?"X-Yakumo-Token: "+token+"\r\n":"")+"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+std::to_string(body.size())+"\r\n"+extra+"\r\n"+body;
        auto result=std::async(std::launch::async,[&]{return exchange(text);});
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        while(result.wait_for(std::chrono::milliseconds(2))!=std::future_status::ready) {
            web::tick(0);check(std::chrono::steady_clock::now()<until,"bounded HTTP completion");
        }
        return result.get();
    };
    check(request("/").find("<title>mhp3rd-ps5 · Settings</title>")!=std::string::npos,"packaged HTML served");
    check(request("/menu.js").find("network.nickname")!=std::string::npos,"packaged JS served");
    check(request("/../config/settings.ini").find("404 Not Found")!=std::string::npos,"traversal not routed");
    check(request("/api/settings","",false,"",false).find("Invalid session")!=std::string::npos,"session authentication");
    check(request("/api/settings","",false,"Origin: http://evil.invalid\r\n").find("Invalid origin")!=std::string::npos,"cross origin rejected");
    check(request("/api/settings","",false,"Host: evil.invalid\r\n").find("Duplicate header")!=std::string::npos,"duplicate host rejected");
    check(request("/api/settings","",true).find("Invalid API method")!=std::string::npos,"GET/POST contract");
    const auto get=request("/api/settings");
    const auto marker=get.find("\"revision\":\"");check(marker!=get.npos,"snapshot revision");
    const auto begin=marker+12,end=get.find('"',begin);const auto rev=get.substr(begin,end-begin);
    check(get.find("Online nickname")!=get.npos&&get.find("OldAlias")!=get.npos,"online fields serialized");
    auto saved=request("/api/save","_revision="+rev+"&network.nickname=Nuevo%20Alias&video.internal_scale=6&ps5.network_trace=0",true);
    check(saved.find("\"ok\":true")!=saved.npos,"save transaction via HTTP");
    check(menu::read().at("network.nickname")=="Nuevo Alias"&&menu::read().at("input.name")=="Hunter","nickname never changes hunter");
    check(settings::to_entries(settings::current())==live,"HTTP save is restart only");
    settings::current().adhoc_mac="02:11:22:33:44:55";settings::save();
    const auto retained=menu::read();
    check(retained.at("network.nickname")=="Nuevo Alias"&&retained.at("video.internal_scale")=="6"&&retained.at("future.key")=="preserved","later internal save retains pending edits and unknown preferences");
    check(retained.at("network.mac")=="02:11:22:33:44:55","internal MAC save still effective");
    check(request("/api/save","_revision="+rev+"&audio.volume=1",true).find("\"ok\":false")!=std::string::npos,"stale save rejected");
    check(request("/api/status").find("\"peers\":")!=std::string::npos,"read only diagnostics");
    check(request("/api/ping","",true).find("\"ok\":true")!=std::string::npos,"heartbeat");
    check(request("/api/focus","editing=1",true).find("\"ok\":true")!=std::string::npos,"editable focus report");
    web::suppress_sample(false,true,false,false,99);check(web::blocks_input(),"Circle in an editable field does not close the browser session");
    check(request("/api/focus","editing=0",true).find("\"ok\":true")!=std::string::npos,"non-editing focus report");
    web::suppress_sample(false,true,false,false,100);check(web::blocks_input(),"Circle closes the browser session outside editable fields");
    web::suppress_sample(false,false,true,false,101);check(!web::blocks_input(),"one Circle close returns controls after release");
    check(request("/api/close","",true).find("\"ok\":true")!=std::string::npos,"close notification");
    check(web::suppress_sample(false,false,false,false,102)&&web::blocks_input(),"close retains held input guard");
    web::suppress_sample(false,false,true,false,103);check(!web::blocks_input(),"close waits for released buttons and triggers");
    check(request("/api/hud","",true).find("\"ok\":true")!=std::string::npos,"HUD action");
    web::stop();web::stop();check(!web::blocks_input(),"joined shutdown releases capture");
#if YAKUMO_NATIVE
    check(web_fixture::descriptors.empty(),"listener and accepted handles all closed");
    web_fixture::launch_rc=-1;web::start();web::suppress_sample(false,false,true,false,200);
    web::suppress_sample(true,false,false,false,201);
    for(int i=0;i<100&&web_fixture::launches<2;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    web::suppress_sample(false,false,true,false,202);web::suppress_sample(false,false,true,false,203);
    check(!web::blocks_input(),"failed browser launch releases input safely");web::stop();
#endif
    fs::remove_all(root);
    std::cout<<"HTTP sessions, assets, save/conflict, nickname/MAC preservation and chord lifecycle PASS\n";
    return 0;
}catch(const std::exception& error){yakumo::game_web::stop();std::cerr<<error.what()<<'\n';return 1;}}
