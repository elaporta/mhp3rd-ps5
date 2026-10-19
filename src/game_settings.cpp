// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_settings.hpp"
#include "game_files.hpp"
#include "game_pad.hpp"
#include "gpu/game_hud.hpp"
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace yakumo::game_settings {
namespace {
namespace settings = mhp3rd::settings;
namespace input = mhp3rd::input;
Entries pending;
[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("Settings: " + why); }
constexpr std::array<const char*,5> kPs5PadKeys={
    "ps5.input.l2","ps5.input.r2","ps5.input.touchpad","ps5.input.l1",
    "ps5.input.r1"};
constexpr std::array<const char*,2> kLegacyPs5StickKeys={"ps5.input.l3","ps5.input.r3"};
constexpr std::array<Ps5Assignment,5> kPs5PadDefaults={
    Ps5Assignment::FastForward,Ps5Assignment::Select,Ps5Assignment::HideHud,
    Ps5Assignment::L,Ps5Assignment::R};
constexpr std::array<const char*,6> kPs5AssignmentValues={
    "none","l","r","hide_hud","fast_forward","select"};
const char* assignment_name(Ps5Assignment assignment) {
    return kPs5AssignmentValues[static_cast<std::size_t>(assignment)];
}
Ps5Assignment parse_assignment(const std::string& value) {
    for (std::size_t i=0;i<kPs5AssignmentValues.size();++i)
        if (value==kPs5AssignmentValues[i]) return static_cast<Ps5Assignment>(i);
    fail("invalid PS5 button assignment");
}
std::optional<input::Action> action_of(const std::string& key) {
    for (unsigned i = 0; i < input::kActions; ++i) {
        const auto action = static_cast<input::Action>(i);
        if (key == std::string("input.pad.") + input::info(action).key) return action;
    }
    return std::nullopt;
}
const Option& option_of(const std::string& key) {
    for (const auto& option : catalog()) if (option.key == key) return option;
    fail("unknown or reserved option: " + key);
}
input::Slots validate_binding(const std::string& text) {
    input::Slots slots{};
    if (!input::parse(text, slots)) fail("invalid gamepad binding");
#if YAKUMO_GAME_WEB
    for (const auto& chord : slots) {
        bool left = false, right = false;
        for (const auto b : chord.inputs) {
            left |= b == input::pad(input::PadInput::LeftStick);
            right |= b == input::pad(input::PadInput::RightStick);
        }
        if (left && right) fail("L3 + R3 is reserved for the settings menu");
    }
#endif
    for (const auto& chord : slots) for (const auto binding : chord.inputs) {
        if (binding == input::kNone) continue;
        // The native ABI exposes the four face buttons, sticks, Start and D-pad.
        // Guide, paddles, touch aliases and the reserved shoulders/triggers
        // cannot become editable inputs or steal the platform controls.
        if (!game_pad::editable_input(binding))
            fail("unavailable or reserved gamepad input");
    }
    return slots;
}
void validate(const Option& option, const std::string& text) {
    if (settings::overridden_by(option.key.c_str())) fail("environment override: " + option.key);
    if (option.kind == Kind::Binding) { (void)validate_binding(text); return; }
    if (option.kind == Kind::Text) {
        if (text.size() > static_cast<std::size_t>(option.maximum) || (!text.empty() && (text.front() == ' ' || text.back() == ' ')))
            fail("text exceeds the UTF-8 byte limit or has surrounding spaces");
        // Reject malformed, overlong UTF-8, surrogate code points and controls.
        for (std::size_t at = 0; at < text.size();) {
            const unsigned char lead = text[at++];
            unsigned code = lead, count = 0, minimum = 0;
            if (lead >= 0xc2 && lead <= 0xdf) { code &= 31; count = 1; minimum = 0x80; }
            else if (lead >= 0xe0 && lead <= 0xef) { code &= 15; count = 2; minimum = 0x800; }
            else if (lead >= 0xf0 && lead <= 0xf4) { code &= 7; count = 3; minimum = 0x10000; }
            else if (lead >= 0x80) fail("invalid UTF-8");
            for (unsigned i = 0; i < count; ++i) {
                if (at == text.size() || (static_cast<unsigned char>(text[at]) & 0xc0) != 0x80) fail("invalid UTF-8");
                code = code * 64 + (static_cast<unsigned char>(text[at++]) & 63);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
                code < 32 || (code >= 127 && code <= 159) || code == 0x2028 || code == 0x2029)
                fail("unsupported text character");
        }
        if (option.key == "network.server" && !text.empty()) {
            const auto split = text.find(':');
            const auto host = text.substr(0, split);
            if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') fail("invalid server");
            for (const unsigned char c : host)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '-')) fail("invalid server hostname");
            if (split != text.npos) {
                unsigned port = 0;
                const auto value = text.substr(split + 1);
                if (value.empty() || value.size() > 5) fail("invalid server port");
                for (char c : value) { if (c < '0' || c > '9') fail("invalid server port"); port = port * 10 + c - '0'; }
                if (!port || port > 65535) fail("invalid server port");
            }
        }
        return;
    }
    if (!option.choices.empty()) {
        if (std::find(option.choices.begin(), option.choices.end(), text) == option.choices.end())
            fail("invalid choice: " + option.key);
        return;
    }
    // Small nonnegative decimals are sufficient for every menu field. Parse
    // without locale or floating from_chars (absent from the pinned PS5 libc++).
    if (text.empty() || text.size() > 32) fail("invalid number: " + option.key);
    double number{};
    double divisor = 1;
    bool fractional = false, digit = false;
    for (const char c : text) {
        if (c == '.' && !fractional && option.kind == Kind::Number) { fractional = true; continue; }
        if (c < '0' || c > '9') fail("invalid number: " + option.key);
        digit = true;
        if (fractional) { divisor *= 10; number += (c - '0') / divisor; }
        else number = number * 10 + c - '0';
    }
    if (!digit || !std::isfinite(number) ||
        number < option.minimum || number > option.maximum ||
        (option.kind == Kind::Integer && number != std::floor(number))) fail("invalid number: " + option.key);
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
struct Fd {
    int value;
    ~Fd() { if (value >= 0) ::close(value); }
    void close() { const int old = value; value = -1; if (::close(old)) fail("close failed"); }
};
auto config_path() { return game_files::root() / "yakumo/config/settings.ini"; }
}

const std::vector<Option>& catalog() {
    static const auto options = [] {
        std::vector<Option> out;
        const auto add = [&](const char* key, const char* label, Section section, Kind kind,
                             double lo = 0, double hi = 0, std::vector<std::string> choices = {}) {
            out.push_back({key, label, section, kind, lo, hi, std::move(choices), {}});
        };
        const auto toggle = [&](const char* key, const char* label, Section section) {
            add(key, label, section, Kind::Toggle, 0, 1, {"0", "1"});
        };
        using S = Section; using K = Kind;
        add("video.internal_scale", "Internal resolution", S::Video, K::Integer, 1, 6);
        add("video.frame_rate", "Presentation FPS (30 FPS game)", S::Video, K::Choice, 0, 0, {"30", "45", "60"});
        toggle("video.frame_rate_auto", "Lower FPS when behind", S::Video);
        add("video.aspect", "Aspect ratio", S::Video, K::Choice, 0, 0, {"original", "stretch", "fill"});
        toggle("video.texture_pack", "Installed HD textures", S::Video);
        toggle("video.sharp_screen", "Sharp screen filter", S::Video);
        toggle("video.sharp_textures", "Sharp texture filter", S::Video);
        add("video.ui_textures", "UI texture filter", S::Video, K::Choice, 0, 0, {"off", "sharp", "mmpx"});
        toggle("text.crisp", "Crisp text", S::Video);
        add("text.weight", "Text weight", S::Video, K::Integer, 0, 2);
        add("video.performance", "Performance display and logging", S::Video, K::Choice, 0, 0,
            {"off", "compact", "overlay", "log", "compact+log", "overlay+log"});
        add("video.fast_forward", "L2 fast-forward mode", S::Speed, K::Choice, 0, 0, {"hold", "toggle", "off"});
        add("video.fast_forward_speed", "Fast-forward multiplier", S::Speed, K::Integer, 2, 8);
        toggle("video.fast_loading", "Fast loading", S::Speed);
        add("input.confirm", "Confirm button", S::Controls, K::Choice, 0, 0, {"east", "south"});
        for (std::size_t i=0;i<kPs5PadKeys.size();++i)
            add(kPs5PadKeys[i], kPs5PadKeys[i], S::Controls, K::Choice, 0, 0,
                {"l", "r", "hide_hud", "fast_forward", "select", "none"});
        add("input.right_stick", "Right stick", S::Controls, K::Choice, 0, 0, {"camera", "dpad", "off"});
        toggle("input.analog_camera", "Analog camera", S::Controls);
        toggle("input.invert_camera_x", "Invert camera X", S::Controls);
        toggle("input.invert_camera_y", "Invert camera Y", S::Controls);
        add("audio.volume", "Volume (%)", S::Audio, K::Integer, 0, 100);
        toggle("audio.mute", "Mute", S::Audio);
        toggle("network.adhoc", "Online enabled", S::Online);
        add("network.nickname", "Online nickname (not the hunter name)", S::Online, K::Text, 0, 127);
        add("network.server", "Ad hoc server (host or host:port)", S::Online, K::Text, 0, 259);
        toggle("ps5.network_trace", "Detailed online logging", S::Online);
        const auto defaults = settings::to_entries(settings::defaults());
        for (auto& option : out) {
            if (option.key == "ps5.network_trace") option.initial = "0";
            else if (option.key == "video.performance") option.initial = "off";
            else if (option.key == "network.nickname") option.initial = "mh3rd-ps5";
            else if (option.key.rfind("ps5.input.",0)==0) {
                for (std::size_t i=0;i<kPs5PadKeys.size();++i) if (option.key==kPs5PadKeys[i])
                    option.initial=assignment_name(kPs5PadDefaults[i]);
            } else option.initial=defaults.at(option.key);
        }
        return out;
    }();
    return options;
}
Entries values(const Entries& persisted) {
    auto result = settings::to_entries(settings::from_entries(persisted));
    if (!persisted.count("network.nickname")) result["network.nickname"] = "mh3rd-ps5";
    const auto trace = persisted.find("ps5.network_trace");
    result["ps5.network_trace"] = trace == persisted.end() ? "0" : trace->second;
    for(std::size_t i=0;i<kPs5PadKeys.size();++i) {
        const auto found=persisted.find(kPs5PadKeys[i]);
        result[kPs5PadKeys[i]]=found==persisted.end()?assignment_name(kPs5PadDefaults[i]):found->second;
    }
    for(const auto* key:kLegacyPs5StickKeys) {
        const auto found=persisted.find(key);
        if(found!=persisted.end()) result[key]=found->second;
    }
    return result;
}
std::array<Ps5Assignment,5> ps5_assignments() {
    static const auto result=[] {
        if(game_files::root().empty()) return kPs5PadDefaults;
        return ps5_assignments(read());
    }();
    return result;
}
std::array<Ps5Assignment,5> ps5_assignments(const Entries& persisted) {
    const auto configured=values(persisted);
    std::array<Ps5Assignment,5> result{};
    for(std::size_t i=0;i<result.size();++i) {
        try { result[i]=parse_assignment(configured.at(kPs5PadKeys[i])); }
        catch(const std::exception&) { result[i]=kPs5PadDefaults[i]; }
    }
    return result;
}
void apply_defaults() {
    if (settings::overridden_by("network.nickname")) return;
    const auto persisted=read();
    if (!persisted.count("network.nickname")) settings::current().adhoc_nickname="mh3rd-ps5";
}
bool enabled(const Option& option, const Entries& entries) {
    const auto get = [&](const char* key) { const auto it = entries.find(key); return it == entries.end() ? std::string{} : it->second; };
    if (settings::overridden_by(option.key.c_str())) return false;
    if ((option.key == "video.fast_forward" || option.key == "video.fast_forward_speed" ||
         option.key == "video.frame_rate" || option.key == "video.frame_rate_auto") && get("video.unthrottled") == "1") return false;
    if (option.key == "video.fast_forward_speed") return get("video.fast_forward") != "off";
    if (option.key == "video.frame_rate_auto") return get("video.frame_rate") != "30";
    if (option.key == "input.analog_camera" || option.key == "input.invert_camera_x" || option.key == "input.invert_camera_y")
        return get("input.right_stick") == "camera";
    return true;
}
std::string resolution_label(unsigned scale) {
    if (scale < 1 || scale > 6) fail("unsupported internal scale");
    return std::to_string(480 * scale) + "x" + std::to_string(272 * scale);
}
Prepared prepare(const Entries& persisted, const Entries& changes, std::optional<Section> restore) {
    Entries patch;
    if (restore) for (const auto& option : catalog())
        if (option.section == *restore && !settings::overridden_by(option.key.c_str())) patch[option.key] = option.initial;
    for (const auto& [key, value] : changes) { validate(option_of(key), value); patch[key] = value; }
    auto current = settings::from_entries(persisted);
    const auto before = values(persisted);
    Entries ordinary = before;
    for (const auto& [key, value] : patch) if (option_of(key).kind != Kind::Binding) ordinary[key] = value;
    if (!patch.empty()) ordinary["settings.version"] = "3";
    current = settings::from_entries(ordinary);
    bool bindings = false;
    for (const auto& [key, value] : patch) bindings |= option_of(key).kind == Kind::Binding;
    if (bindings) {
        if (current.control_preset.shipped && current.user_presets.size() >= input::kMaxUserPresets)
            fail("preset storage is full; no existing preset will be removed");
        (void)settings::prepare_controls_edit(current);
        for (const auto& [key, value] : patch) if (option_of(key).kind == Kind::Binding)
            current.controls.pad[static_cast<unsigned>(*action_of(key))] = validate_binding(value);
        current.controls = game_pad::native_layout(current.controls);
        settings::controls_edited(current);
    }
    auto after = settings::to_entries(current);
    after["ps5.network_trace"] = ordinary.at("ps5.network_trace");
    for(const auto* key:kPs5PadKeys) after[key]=ordinary.at(key);
    Prepared result{persisted, {}, false};
    // Merge only the semantic delta, never rewrite unrelated/unknown preferences.
    for (const auto& [key, value] : after) {
        const auto old = before.find(key);
        if (old == before.end() || old->second != value) result.entries[key] = value;
    }
    // An explicit choice is saved even when it equals a missing key's default.
    for (const auto& [key, value] : patch) result.entries[key] = after.at(key);
    if (!patch.empty()) {
        result.entries["settings.version"] = after.at("settings.version");
        const auto old_server = persisted.find("network.server");
        if (!patch.count("network.server") && before.at("network.server") == "socom.cc" &&
            (old_server == persisted.end() || old_server->second.empty()))
            result.entries["network.server"] = "socom.cc";
    }
    for (const auto& [key, value] : result.entries) {
        const auto old = persisted.find(key);
        if (old == persisted.end() || old->second != value) result.changed.push_back(key);
    }
    result.restart_required = !result.changed.empty();
    return result;
}
Entries read() {
    const auto target = config_path();
    Fd fd{game_files::open_file(target, O_RDONLY)};
    if (fd.value < 0) { if (errno == ENOENT) return {}; fail("cannot read settings.ini"); }
    std::string text;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(fd.value, buffer, sizeof(buffer));
        if (count < 0) { if (errno == EINTR) continue; fail("read failed"); }
        if (!count) break;
        text.append(buffer, static_cast<std::size_t>(count));
        if (text.size() > 1024 * 1024) fail("settings.ini exceeds 1 MiB");
    }
    fd.close();
    Entries entries; std::istringstream lines(text); std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto split = line.find('=');
        if (split == line.npos) continue;
        auto key = trim(line.substr(0, split));
        if (!key.empty()) entries[key] = trim(line.substr(split + 1));
    }
    return entries;
}
Prepared save(const Entries& expected, const Entries& changes, std::optional<Section> restore) {
    auto prepared = prepare(expected, changes, restore);
    if (read() != expected) fail("settings changed on disk; reload before saving");
    if (!prepared.restart_required) return prepared;
    const auto target = config_path();
    game_files::create_directories(target.parent_path());
    // Validate an existing destination for writing, including hardlink rejection.
    Fd destination{game_files::open_file(target, O_RDWR)};
    if (destination.value < 0 && errno != ENOENT) fail("settings.ini is not writable");
    if (destination.value >= 0) destination.close();
    const auto partial = target.parent_path() / "settings.ini.part";
    Fd fd{game_files::open_file(partial, O_WRONLY | O_CREAT | O_EXCL)};
    if (fd.value < 0) fail("cannot create settings.ini.part; existing files were kept");
    try {
        std::string output = "# MHP3rd PS5 settings; saved changes apply on next start.\n";
        for (const auto& [key, value] : prepared.entries) {
            if (key.find_first_of("\r\n=") != key.npos || key.find('\0') != key.npos ||
                value.find_first_of("\r\n") != value.npos || value.find('\0') != value.npos) fail("invalid INI entry");
            output += key + "=" + value + "\n";
        }
        std::size_t at{};
        while (at < output.size()) {
            const auto n = ::write(fd.value, output.data() + at, output.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) fail("write failed");
            at += static_cast<std::size_t>(n);
        }
        if (::fsync(fd.value)) fail("fsync failed");
        fd.close();
        if (read() != expected) fail("settings changed during save; reload before saving");
        game_files::rename(partial, target);
    } catch (...) {
        std::error_code ec; game_files::remove(partial, ec);
        throw;
    }
    for (const auto& key : prepared.changed) pending[key] = prepared.entries.at(key);
    return prepared;
}
Entries retain_pending(Entries entries) {
    try {
        const auto persisted=read();
        const auto trace=persisted.find("ps5.network_trace");
        if(trace!=persisted.end()) entries[trace->first]=trace->second;
        for(const auto* key:kPs5PadKeys) {
            const auto value=persisted.find(key);
            if(value!=persisted.end()) entries[value->first]=value->second;
        }
        for(const auto* key:kLegacyPs5StickKeys) {
            const auto value=persisted.find(key);
            if(value!=persisted.end()) entries[value->first]=value->second;
        }
    } catch(const std::exception&) {
        // Keep an internal Yakumo settings save working if menu-only reads fail.
    }
    for (const auto& [key, value] : pending) entries[key] = value;
    return entries;
}
bool toggle_hud() {
    if (!mhp3rd::gpu::hud::available()) return false;
    mhp3rd::gpu::hud::toggle();
    return true;
}
} // namespace yakumo::game_settings
