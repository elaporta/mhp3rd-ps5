// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>

// Owner-thread API. Independent of the running ModSession: saving never activates files.
namespace yakumo::game_mods {
std::string catalog();
std::string save(const std::map<std::string,std::string>& fields);
std::string preview(const std::string& id);
}
