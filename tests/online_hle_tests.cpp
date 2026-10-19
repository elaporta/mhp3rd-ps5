// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse upstream's actual HLE transport contracts with isolated title storage;
// unrelated filesystem/system scenarios remain in their original test suite.
#define main upstream_kernel_suite_main
#include "kernel_hle_tests.cpp"
#undef main
#include <game_files.hpp>
#include <unistd.h>
int main(int argc,char** argv) { try {
    const auto root=std::filesystem::temp_directory_path()/("yakumo-online-hle-"+std::to_string(getpid()));
    std::filesystem::create_directories(root/"yakumo/config");
    yakumo::game_files::set_root(root);
    mhp3rd::install::set_data_directory_override(root/"yakumo/config");
    if(argc==1) {
        network_contracts();
        mhp3rd::adhoc_shutdown();
        std::cout<<"actual guest PDP/PTP/Netconf waits, cancellation, relay and reconnect contracts PASS\n";
    } else {
        Fixture f;
        auto& settings=mhp3rd::settings::current();
        const bool existing=std::string(argv[1])=="existing-mac";
        const std::string configured="02:12:34:56:78:9a";
        settings.adhoc_mac=existing?configured:"";
        check(!settings.adhoc,"MAC creation does not opt in to online play");
        mhp3rd::HleRegistrar hle(f.runtime); mhp3rd::register_adhoc(hle);
        check(f.call("sceNet","sceNetGetLocalEtherAddr",{Fixture::output})==0,"guest obtains virtual MAC");
        mhp3rd::adhoc::Mac mac{};
        for(unsigned i=0;i<6;++i)mac[i]=f.runtime.memory().load8(Fixture::output+i);
        const auto identity=mhp3rd::adhoc::format_mac(mac);
        check(settings.adhoc_mac==identity,"guest and stored MAC agree");
        if(existing) check(identity==configured,"configured identity preserved exactly");
        else {
            check((mac[0]&3)==2,"generated identity is local unicast");
            check(mhp3rd::install::read_settings_file(root/"yakumo/config").at("network.mac")==identity,"new virtual MAC persisted through real settings writer");
        }
        check(f.call("sceWlanDrv","sceWlanGetEtherAddr",{Fixture::output+8})==0,"WLAN gets same identity");
        for(unsigned i=0;i<6;++i)check(f.runtime.memory().load8(Fixture::output+8+i)==mac[i],"MAC stable across guest APIs");
        mhp3rd::adhoc_shutdown();
        std::cout<<"per-install virtual MAC creation/preservation and guest readback PASS\n";
    }
    std::filesystem::remove_all(root);
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
