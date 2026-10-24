// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_settings.hpp"
#include "game_files.hpp"
#include "game_pad.hpp"
#include "gpu/game_hud.hpp"
#include "install/user_data.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unistd.h>
#include <cerrno>
#ifndef YAKUMO_GAME_WEB
#define YAKUMO_GAME_WEB 0
#endif

namespace { bool fail_sync{}, fail_rename{}; }
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) { if (fail_sync) { errno=EIO; return -1; } return __real_fsync(fd); }
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (fail_rename) { errno=EIO; return -1; } return __real_rename(from,to);
}

namespace {
void check(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
template<class F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "invalid transaction was accepted");
}
}
int main(int argc, char**) { try {
    namespace menu = yakumo::game_settings;
    namespace settings = mhp3rd::settings;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("yakumo-settings-" + std::to_string(getpid()));
    fs::create_directories(root / "yakumo/config");
    yakumo::game_files::set_root(root);
    mhp3rd::install::set_data_directory_override(root / "yakumo/config");
    if (argc > 1) {
        check(settings::current().confirm_south, "environment override loaded");
        rejects([&] { (void)menu::prepare({}, {{"input.confirm", "east"}}); });
        for (const auto& entry : menu::catalog()) if (entry.key == "input.confirm")
            check(!menu::enabled(entry, menu::values({})), "override is read-only");
        check(!menu::prepare({}, {}, menu::Section::Controls).entries.count("input.confirm"), "restore preserves override preference");
        fs::remove_all(root);
        return 0;
    }
    const auto defaults = menu::values({});
    check(defaults.at("video.internal_scale") == "6" && defaults.at("video.frame_rate") == "60" &&
          defaults.at("video.texture_pack") == "1" && defaults.at("video.fast_forward_speed") == "4", "recommended profile");
    check(defaults.at("network.nickname")=="mh3rd-ps5"&&menu::catalog().end()!=
          std::find_if(menu::catalog().begin(),menu::catalog().end(),[](const auto& o){return o.key=="network.nickname"&&o.initial=="mh3rd-ps5";}),
          "new online nickname defaults independently of the hunter name");
    menu::apply_defaults();
    check(settings::current().adhoc_nickname=="mh3rd-ps5","runtime online nickname default is applied on a fresh title");
    check(menu::read().empty(), "missing file is an empty preference set");
    check(defaults.at("ps5.output_resolution")=="1080p" && menu::output_resolution()=="1080p", "default native output");
    check(menu::output_resolution({{"ps5.output_resolution","invalid"}})=="1080p", "invalid saved output falls back safely");
    std::set<std::string> keys;
    for (const auto& option : menu::catalog()) {
        check(keys.insert(option.key).second, "duplicate catalog key");
        check(option.initial == defaults.at(option.key) && option.restart_required, "catalog defaults and restart contract");
        (void)menu::prepare({}, {{option.key,option.initial}});
        // Every displayed choice must survive the actual Yakumo parser.
        for (const auto& choice : option.choices) {
            const auto prepared = menu::prepare({}, {{option.key, choice}});
            check(menu::values(prepared.entries).at(option.key) == choice, "choice ignored: " + option.key + "=" + choice);
        }
        if (option.kind == menu::Kind::Binding) {
            auto cleared = menu::prepare({}, {{option.key, ""}});
            check(menu::values(cleared.entries).at(option.key).empty(), "unbinding did not survive");
        } else if (option.choices.empty() && option.kind != menu::Kind::Text) {
            for (double endpoint : {option.minimum, option.maximum}) {
                const auto value = option.kind == menu::Kind::Integer ? std::to_string(int(endpoint)) : std::to_string(endpoint);
                const auto prepared = menu::prepare({}, {{option.key, value}});
                check(std::stod(menu::values(prepared.entries).at(option.key)) == endpoint, "numeric endpoint ignored");
            }
        }
    }
    check(std::count_if(keys.begin(),keys.end(),[](const auto& key){return key.rfind("ps5.input.",0)==0;})==5 &&
          !keys.count("ps5.input.l3") && !keys.count("ps5.input.r3"), "catalog exposes only five PS5 button assignments");
    check(defaults.at("ps5.input.l2")=="fast_forward"&&defaults.at("ps5.input.r2")=="select"&&
          defaults.at("ps5.input.touchpad")=="hide_hud"&&defaults.at("ps5.input.l1")=="l"&&
          defaults.at("ps5.input.r1")=="r","PS5 controls preserve their initial distribution");
    const auto mapped=menu::ps5_assignments({{"ps5.input.l2","select"},{"ps5.input.r2","none"},
        {"ps5.input.touchpad","select"},{"ps5.input.l3","hide_hud"}});
    check(mapped[0]==menu::Ps5Assignment::Select&&mapped[1]==menu::Ps5Assignment::None&&
          mapped[2]==menu::Ps5Assignment::Select&&mapped[3]==menu::Ps5Assignment::L&&
          mapped[4]==menu::Ps5Assignment::R,"only exposed PS5 assignments affect pad input");
    for(const auto& name : {std::string("Alias online"),std::string("Cazador ñ"),std::string(127,'a'),std::string{}}) {
        const auto result=menu::prepare({{"input.name","Hunter"}},{{"network.nickname",name}});
        check(result.entries.at("network.nickname")==name&&result.entries.at("input.name")=="Hunter","nickname is separate from hunter");
    }
    for(const auto& bad : {std::string(128,'a'),std::string(" newline\n"),std::string(" leading"),
                          std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xe2\x82")})
        rejects([&]{menu::prepare({},{{"network.nickname",bad}});});
    for(const char* server : {"socom.cc","127.0.0.1:27312","", "relay.example:65535"})
        check(menu::prepare({},{{"network.server",server}}).entries.at("network.server")==server,"server parser round trip");
    for(const char* server : {"https://socom.cc","host:0","host:65536","host:abc","::1","host/path","host\nother=1"})
        rejects([&]{menu::prepare({},{{"network.server",server}});});
#if YAKUMO_GAME_WEB
    rejects([&]{menu::prepare({},{{"input.pad.cross","Pad L3 + Pad R3"}});});
#endif
    for (const char* key : {"input.pad.l", "input.pad.r", "input.pad.select", "input.pad.fast_forward", "input.pad.hide_hud",
                           "input.name", "text.font", "video.fullscreen", "video.unthrottled", "video.present_mode"})
        rejects([&] { (void)menu::prepare({}, {{key, "1"}}); });
    for (const auto& changes : std::vector<menu::Entries>{
        {{"video.internal_scale", "0"}}, {{"video.internal_scale", "9"}}, {{"video.internal_scale", "4.0"}},
        {{"video.frame_rate", "240"}}, {{"ps5.output_resolution", "4k"}}, {{"input.trigger", "0.049"}}, {{"input.dead_zone", "0.51"}},
        {{"audio.volume", "101"}}, {{"audio.volume", "-1"}}, {{"audio.mute", "true"}},
        {{"input.dead_zone", "NaN"}}, {{"input.dead_zone", "inf"}}, {{"input.camera_speed", "30x"}},
        {{"input.pad.cross", "Space"}}, {{"input.pad.cross", "Pad Paddle L1"}},
        {{"input.pad.cross", "Pad LB"}}, {{"input.pad.cross", "Pad LT"}}, {{"input.pad.cross", "Pad RT"}},
        {{"input.pad.cross", "Pad Back"}}, {{"input.pad.cross", "Pad Touchpad"}},
        {{"input.pad.cross", "Pad R3 + Pad LB"}}, {{"video.fast_forward_speed", "9"}}})
        rejects([&] { (void)menu::prepare({}, changes); });
    rejects([&]{(void)menu::prepare({},{{"ps5.input.l2","launch_browser"}});});
    check(menu::resolution_label(4) == "1920x1088" && menu::resolution_label(6) == "2880x1632", "resolution labels");
    check(menu::resolution_label(7)=="3360x1904" && menu::resolution_label(8)=="3840x2176", "extended scale dimensions");
    rejects([] { (void)menu::resolution_label(9); });
    for(const char* scale:{"7","8"}) {
        const auto prepared=menu::prepare({},{{"video.internal_scale",scale},{"video.frame_rate","120"},
                                              {"ps5.output_resolution","2160p"}});
        const auto loaded=menu::values(prepared.entries);
        check(loaded.at("video.internal_scale")==scale && loaded.at("video.frame_rate")=="120" &&
              loaded.at("ps5.output_resolution")=="2160p", "extended video choices survive parsing");
    }
    const auto& options = menu::catalog();
    const auto option = [&](const char* key) -> const menu::Option& {
        for (const auto& entry : options) if (entry.key == key) return entry;
        throw std::runtime_error("missing option");
    };
    auto dependencies = defaults;
    dependencies["input.analog_camera"] = "0";
    check(menu::enabled(option("input.invert_camera_y"), dependencies), "camera inversion follows right-stick camera mode");
    dependencies["input.right_stick"] = "dpad";
    check(!menu::enabled(option("input.invert_camera_x"), dependencies), "camera inversion is disabled in D-pad mode");
    dependencies["video.frame_rate"] = "30"; dependencies["video.fast_forward"] = "off";
    check(!menu::enabled(option("video.frame_rate_auto"), dependencies) &&
          !menu::enabled(option("video.fast_forward_speed"), dependencies), "speed and FPS dependencies");
    dependencies=defaults;dependencies["video.unthrottled"]="1";
    check(!menu::enabled(option("video.frame_rate"),dependencies) &&
          !menu::enabled(option("video.fast_forward"),dependencies),"preserved unlimited mode disables paced options");
    menu::Entries original{{"settings.version", "2"}, {"network.mac", "preserved"}, {"future.preference", "untouched"},
        {"audio.volume", "77"}, {"video.internal_scale", "3"}, {"text.font", "title-font"},
        {"ps5.network_trace", "1"}, {"ps5.input.r3", "legacy-value"}, {"ps5.input.l3", "legacy-value"}};
    mhp3rd::install::write_settings_file(root / "yakumo/config", original);
    const auto retained=menu::retain_pending({{"audio.volume","77"}});
    check(retained.at("ps5.network_trace")=="1"&&retained.at("ps5.input.r3")=="legacy-value"&&
          retained.at("ps5.input.l3")=="legacy-value","internal INI writes preserve old, hidden PS5 preferences");
    auto saved = menu::save(original, {{"video.fast_forward", "toggle"}, {"video.fast_forward_speed", "6"},
        {"ps5.input.l2", "select"}, {"ps5.output_resolution", "1440p"}});
    const auto live_before = settings::to_entries(settings::current());
    check(saved.restart_required && menu::read() == saved.entries, "complete transaction saved");
    check(menu::values(menu::read()).at("ps5.output_resolution")=="1440p" && menu::output_resolution()=="1080p",
          "output selection is persisted but the running output stays unchanged");
    check(menu::retain_pending({}).at("ps5.output_resolution")=="1440p", "internal save retains pending native output");
    for (const auto& [key, value] : original) {
        if (key == "settings.version") {
            check(saved.entries.at(key) == "3", "settings schema upgrades on save");
        } else {
            check(saved.entries.at(key) == value, "unrelated preference changed");
        }
    }
    auto loaded = menu::values(menu::read());
    check(loaded.at("video.fast_forward") == "toggle" && loaded.at("video.fast_forward_speed") == "6" &&
          loaded.at("ps5.input.l2") == "select" && loaded.at("ps5.input.l3") == "legacy-value", "restart reload honors visible choices and preserves hidden legacy data");
    auto audio = menu::save(saved.entries, {}, menu::Section::Audio);
    check(menu::values(audio.entries).at("audio.volume") == "100" && audio.entries.at("video.internal_scale") == "3", "section-only restore");
    check(settings::to_entries(settings::current()) == live_before, "file save must not mutate live settings");
    rejects([&] { (void)menu::save(saved.entries, {{"audio.volume", "10"}}); });
    rejects([&] { (void)menu::save(audio.entries, {{"audio.volume", "50"}, {"text.font", "bad"}}); });
    check(menu::read() == audio.entries, "rejection is atomic");
    auto controls = menu::save(audio.entries, {}, menu::Section::Controls);
    loaded = menu::values(controls.entries);
    for (const auto& entry : options) if (entry.section == menu::Section::Controls)
        check(loaded.at(entry.key) == entry.initial, "control restore: " + entry.key);
    check(controls.entries.at("ps5.input.l2")=="fast_forward"&&controls.entries.at("ps5.input.l3")=="legacy-value",
          "control restore resets visible mappings and preserves hidden legacy keys");
    const auto ini = root / "yakumo/config/settings.ini";
    { std::ofstream partial(ini.string() + ".part"); partial << "previous interrupted write"; }
    rejects([&] { (void)menu::save(controls.entries, {{"audio.volume", "20"}}); });
    check(menu::read() == controls.entries && fs::file_size(ini.string() + ".part") == 26, "pre-existing partial preserved");
    fs::remove(ini.string() + ".part");
    for (auto flag : {&fail_sync, &fail_rename}) {
        *flag=true;
        rejects([&] { (void)menu::save(controls.entries, {{"audio.volume", "20"}}); });
        *flag=false;
        check(menu::read()==controls.entries && !fs::exists(ini.string()+".part"), "failed write preserves INI and cleans its own partial");
    }
    fs::create_hard_link(ini, root / "yakumo/config/linked.ini");
    rejects([&] { (void)menu::save(controls.entries, {{"audio.volume", "20"}}); });
    fs::remove(root / "yakumo/config/linked.ini");
    fs::rename(ini, ini.string()+".backup");
    fs::create_symlink(ini.string()+".backup", ini);
    rejects([&] { (void)menu::read(); });
    rejects([&] { (void)menu::save(controls.entries, {{"audio.volume", "20"}}); });
    fs::remove(ini); fs::rename(ini.string()+".backup", ini);
    check(!menu::save(controls.entries, {}).restart_required, "no-op save needs no restart");
    // A legacy file must still permit an explicit crisp-text choice.
    auto legacy = menu::prepare({{"text.crisp", "1"}}, {{"text.crisp", "1"}});
    check(menu::values(legacy.entries).at("text.crisp") == "1", "explicit crisp choice upgrades legacy version");
    const bool was_hidden=mhp3rd::gpu::hud::hidden();
    check(menu::toggle_hud() && mhp3rd::gpu::hud::hidden()!=was_hidden, "immediate HUD action");
    check(menu::toggle_hud() && mhp3rd::gpu::hud::hidden()==was_hidden, "HUD action round trip is not persisted");
    fs::remove_all(root);
    std::cout << "settings catalog, validation, remap, restore, preservation and checked persistence PASS\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
