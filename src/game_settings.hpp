// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "settings/settings.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

// Transport-independent contract for the future web menu. Call on the guest
// worker, never from an HTTP/browser thread: Yakumo settings are thread-owned.
namespace yakumo::game_settings {
using Entries = mhp3rd::settings::Entries;
enum class Section { Video, Speed, Controls, Camera, Audio, Advanced, Online };
enum class Kind { Toggle, Integer, Number, Choice, Binding, Text };
enum class Ps5Assignment { None, L, R, HideHud, FastForward, Select };
struct Option {
    std::string key, label;
    Section section;
    Kind kind;
    double minimum{}, maximum{};
    std::vector<std::string> choices;
    std::string initial;
    // File changes currently take effect on the next manual application start.
    bool restart_required{true};
};
const std::vector<Option>& catalog();
Entries values(const Entries& persisted);
std::array<Ps5Assignment, 5> ps5_assignments();
std::array<Ps5Assignment, 5> ps5_assignments(const Entries& persisted);
// Apply the online nickname default only when no saved or environment value exists.
void apply_defaults();
bool enabled(const Option&, const Entries& values);
std::string resolution_label(unsigned scale);
struct Prepared {
    Entries entries;
    std::vector<std::string> changed;
    bool restart_required{};
};
// Rejects unknown/read-only keys and invalid input before making any changes.
// Restore changes only this menu's fields in the selected section. Controls
// restore also removes custom gamepad combos, retaining their keyboard binds.
Prepared prepare(const Entries& persisted, const Entries& changes,
                 std::optional<Section> restore = std::nullopt);
Entries read();
// Optimistic conflict check, checked close/fsync, then atomic title-local rename.
// Does not change settings::current(): live application belongs to integration.
Prepared save(const Entries& expected, const Entries& changes,
              std::optional<Section> restore = std::nullopt);
bool toggle_hud();
// Owner-thread overlay protects restart-only edits from later internal saves.
Entries retain_pending(Entries entries);
} // namespace yakumo::game_settings
