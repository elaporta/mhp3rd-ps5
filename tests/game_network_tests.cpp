// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_network.hpp"
#include <game_files.hpp>
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
int main() { try {
    const auto root=std::filesystem::temp_directory_path()/("yakumo-network-"+std::to_string(getpid()));
    std::filesystem::create_directories(root/"yakumo/config");
    yakumo::game_files::set_root(root);
    mhp3rd::install::set_data_directory_override(root/"yakumo/config");
    auto& settings=mhp3rd::settings::current();
    auto check=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    check(!settings.adhoc&&settings.adhoc_server=="socom.cc"&&settings.adhoc_mac.empty(),"installation keeps networking off while using the default relay");
    std::ostringstream output;
    struct Redirect { std::streambuf* old; ~Redirect(){std::cout.rdbuf(old);} } redirect{std::cout.rdbuf(output.rdbuf())};
    yakumo::game_network::initialize();
    check(output.str().find("enabled=0")!=std::string::npos,"offline startup logged honestly");
    const auto before=output.str(); yakumo::game_network::tick(10000);
    check(output.str()==before,"offline tick stays quiet");
    settings.adhoc=true; settings.adhoc_server="socom.cc";
    yakumo::game_network::tick(10000); const auto first=output.str();
    check(first.find("relay_up=0")!=std::string::npos&&first.find("tcp_rtt_ms=UNAVAILABLE")!=std::string::npos,"empty snapshot does not invent relay or latency");
    yakumo::game_network::tick(10001); check(output.str()==first,"rate limited snapshot");
    yakumo::game_network::tick(15000); check(output.str().size()>first.size(),"next snapshot emitted");
    const auto config=root/"yakumo/config/settings.ini";
    for (const char* value : {"1","0","invalid"}) {
        {std::ofstream f(config); f<<"ps5.network_trace="<<value<<'\n';}
        bool rejected{};
        try { yakumo::game_network::initialize(); } catch(const std::runtime_error&){rejected=true;}
        check(rejected==(std::string(value)=="invalid"),"trace option accepts only 0 or 1");
    }
    yakumo::game_network::shutdown(); yakumo::game_network::shutdown();
    check(output.str().find("adhoc threads joined")!=std::string::npos,"explicit shutdown completes before renderer cleanup");
    std::cout.rdbuf(redirect.old);
    std::filesystem::remove_all(root);
    std::cout<<"offline defaults, diagnostics cadence, trace validation and shutdown PASS\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
