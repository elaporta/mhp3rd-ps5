// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_network.hpp"
#include "adhoc_log.hpp"
#include "adhoc/client.hpp"
#include "adhoc/session.hpp"
#include <game_files.hpp>
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include <iostream>
#include <stdexcept>
namespace yakumo::game_network {
void initialize() {
    const auto config = mhp3rd::install::read_settings_file(game_files::root()/"yakumo/config");
    if (const auto it = config.find("ps5.network_trace"); it != config.end()) {
        if (it->second != "0" && it->second != "1") throw std::runtime_error("ps5.network_trace must be 0 or 1");
        mhp3rd::adhoc::Client::set_tracing(it->second == "1");
    }
    const auto& settings = mhp3rd::settings::current();
    std::cout << "[ps5-adhoc] adapter=ipv4-native-r6 enabled=" << settings.adhoc
              << " server=" << settings.adhoc_server << " transport=AemuPostoffice tcp=27312,27313"
#if YAKUMO_NATIVE
              << " socket_backend=SceNet-r6 relay_port_offset=10000 guest_pdp=10000 wire_pdp=20000 log_backend=bounded-owner-pump"
#endif
              << " hardware_verified=0\n";
}
void tick(std::uint64_t now_ms) {
    yakumo::adhoc_log::pump(now_ms);
    static std::uint64_t next{};
    if (now_ms < next || !mhp3rd::settings::current().adhoc) return;
    next = now_ms + 5000;
    const auto d = mhp3rd::adhoc::Client::get().diagnostics();
    const auto logs = yakumo::adhoc_log::stats();
    std::cout << "[ps5-adhoc-status] state=" << static_cast<int>(d.state)
              << " server=" << d.server_address << " group=" << d.group.value_or("")
              << " joining=" << d.joining.value_or("") << " peers=" << d.peers.size()
              << " relay_up=" << d.relay_links_up << " relay_wanted=" << d.relay_links_wanted
              << " packets_in=" << d.total.packets_in << " packets_out=" << d.total.packets_out
              << " bytes_in=" << d.total.bytes_in << " bytes_out=" << d.total.bytes_out
              << " dropped=" << d.dropped << " timeouts=" << d.timeouts
              << " reconnects=" << d.reconnects << " failed_attempts=" << d.failed_attempts
              << " tcp_rtt_ms=" << (d.rtt_ms ? std::to_string(*d.rtt_ms) : "UNAVAILABLE")
              << " last_error=" << d.last_error << " log_pending=" << logs.pending
              << " log_suppressed=" << logs.suppressed << '\n';
}
void shutdown() noexcept {
    std::cout << "[game-cleanup] adhoc begin\n";
    mhp3rd::adhoc_shutdown();
    yakumo::adhoc_log::finish();
    std::cout << "[game-cleanup] adhoc threads joined\n";
}
}
