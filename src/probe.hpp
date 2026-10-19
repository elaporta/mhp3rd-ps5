// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdio>

namespace yakumo::ps5 {
constexpr std::size_t path_capacity = 1024;
class Log;
struct TitlePaths {
    char root[path_capacity]{};
    char game[path_capacity]{}, textures[path_capacity]{}, assets[path_capacity]{};
    char config[path_capacity]{}, saves[path_capacity]{}, logs[path_capacity]{}, cache[path_capacity]{};
    char diagnostics[2048]{};
    bool resolve(const char* const* candidates, std::size_t count, const char* title_id, Log* log = nullptr);
    bool prepare_logs(Log* log = nullptr);
    bool prepare(Log* log = nullptr);
    bool fail(const char* operation, const char* path, int error, Log* log);
};
class Log {
public:
    ~Log();
    void write(const char* format, ...);
    bool attach(const TitlePaths& paths);
private:
    FILE* file_{};
    char early_[16384]{};
    std::size_t used_{};
};
bool join_path(char* out, std::size_t capacity, const char* root, const char* relative);
bool storage_probe(const TitlePaths& paths, Log& log);
bool input_audio_probe(Log& log);
bool graphics_probe(const TitlePaths& paths, Log& log);
void notify_user(const char* message);
void filesystem_diagnostics(const char* root, const char* label, char* summary, std::size_t capacity, Log& log);
}
