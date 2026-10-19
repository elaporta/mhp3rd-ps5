// SPDX-License-Identifier: GPL-3.0-or-later
#define main inherited_adhoc_main
#include "adhoc_ppsspp_peer_tests.cpp"
#undef main
#include "adhoc_socket_fixture.hpp"
#include "adhoc_log.hpp"
#include <sstream>
int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--log-contention") {
        struct SlowSink:std::stringbuf {
            std::atomic<bool> release{};
            std::atomic<unsigned> syncs{};
            int sync() override {
                ++syncs;
                while(!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return 0;
            }
        } sink;
        auto* old_out=std::cout.rdbuf(&sink);
        auto* old_err=std::cerr.rdbuf(&sink);
        auto& client=mhp3rd::adhoc::Client::get();
        std::atomic<bool> started{},probed{};
        std::thread starter([&]{mhp3rd::adhoc::Identity identity; client.start(identity); started=true;});
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!started && sink.syncs==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::thread probe([&]{client.pdp_open(10000,8192); probed=true;});
        deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(150);
        while(!probed && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool isolated=started && probed && sink.syncs==0;
        sink.release=true; starter.join(); probe.join(); client.shutdown();
        std::cout.rdbuf(old_out); std::cerr.rdbuf(old_err);
        yakumo::adhoc_log::finish();
        if(socket_fixture::live_sockets() || socket_fixture::live_pollers()) return 2;
        if(!isolated) {
            std::cerr<<"native client logging must not flush a sink while holding the transport mutex\n";
            return 1;
        }
        std::cout<<"native client stays responsive with a blocked synchronous diagnostic sink\n";
        return 0;
    }
    if(argc>1 && std::string(argv[1])=="--offset-regression") {
        mhp3rd::adhoc::Server server;
        const auto port=start_server(server);
        if(!port) return 2;
        mhp3rd::adhoc::Identity identity;
        identity.server="127.0.0.1:"+std::to_string(port);
        identity.nickname="NativeOffset"; identity.mac=kMacA; identity.product=kProduct;
        auto& client=mhp3rd::adhoc::Client::get();
        client.start(identity); client.join(kGroup);
        if(!wait_until([&]{return client.diagnostics().group.has_value();})) { client.shutdown(); return 2; }
        Raw peer(port);
        peer.send(ctl::login(kMacB,"IndependentPPSSPP",kProduct)+ctl::connect(kGroup));
        if(!wait_until([&]{return client.diagnostics().peers.size()==1;})) { client.shutdown(); return 2; }
        Raw datagrams(relay_port_for(port));
        datagrams.send(relay::init(relay::kInitPdp,kMacB,20000,Mac{},0));
        const auto handle=client.pdp_open(10000,8192);
        if(!wait_until([&]{return client.diagnostics().relay_links_up==1;})) { client.shutdown(); return 2; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string payload="offset probe";
        client.pdp_send(handle,kMacB,10000,payload.data(),payload.size());
        const auto header=datagrams.read(14,1000);
        const auto body=header ? datagrams.read(wire::get32(header->data()+10),1000) : std::nullopt;
        const bool sent=header && wire::get16(header->data()+8)==20000 && body && *body==payload;
        datagrams.send(relay::pdp_header(kMacA,20000,payload.size())+payload);
        std::optional<Datagram> response;
        const bool received=wait_until([&]{return (response=client.pdp_receive(handle)).has_value();},1000)
            && response->port==10000 && response->data==payload;
        client.shutdown(); server.stop();
        if(socket_fixture::live_sockets() || socket_fixture::live_pollers()) return 2;
        if(!sent || !received) {
            std::cerr<<"PPSSPP default-offset bidirectional datagrams require guest 10000 / wire 20000\n";
            return 1;
        }
        std::cout<<"PPSSPP default-offset bidirectional datagrams preserve guest port and release all resources\n";
        return 0;
    }
    if(argc>1 && std::string(argv[1])=="--poll-failure") {
        using namespace socket_fixture;
        mhp3rd::adhoc::Server server;
        const auto port=start_server(server);
        if(!port) return 1;
        faults.wait=13;
        mhp3rd::adhoc::Identity identity;
        identity.server="127.0.0.1:"+std::to_string(port);
        identity.nickname="NativePollFailure"; identity.mac=kMacA; identity.product=kProduct;
        auto& client=mhp3rd::adhoc::Client::get();
        client.start(identity);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(waits.load()==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        client.shutdown(); server.stop();
        if(waits.load()==0 || sends!=0 || live_sockets()!=0 || live_pollers()!=0) {
            std::cerr<<"fatal native poll must never synthesize readiness or send login\n"; return 1;
        }
        std::cout<<"fatal native poll sent zero login bytes and joined without live resources\n";
        return 0;
    }
    const int rc=inherited_adhoc_main(argc,argv);
    if(argc>1) return rc; // Retain the upstream exit/hosting subprocess scenarios.
    mhp3rd::adhoc::Client::get().shutdown();
    using namespace socket_fixture;
    if(creates==0 || epoll_creates==0 || live_sockets()!=0 || live_pollers()!=0 || creates!=closes || epoll_creates!=epoll_destroys) {
        std::cerr<<"SceNet client must exercise native handles and join with no live resources\n";
        return 1;
    }
    std::cout<<"SceNet protocol exercised "<<creates<<" virtual handles; explicit shutdown released all resources\n";
    return rc;
}
