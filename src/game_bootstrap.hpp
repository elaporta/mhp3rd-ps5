// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "game_files.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace yakumo::game_bootstrap {
// Emergency diagnostics must survive a failed mode repair. This is restricted
// to the fixed title log, preserves append-only bytes and validates both parent
// and final handles. It never reports permission repair success.
inline FILE* open_log(){
    namespace files=yakumo::game_files;
    const auto name=files::root()/"yakumo/logs/game-first-frame.log";
    std::error_code ec;struct stat parent{};
    if(!files::permitted(name,true,ec)||!files::inspect(name.parent_path(),parent,ec))return nullptr;
    if(!S_ISDIR(parent.st_mode)){errno=ENOTDIR;return nullptr;}
    const int fd=::open(name.c_str(),O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_NONBLOCK,0666);
    if(fd<0)return nullptr;
    struct stat st{};const int rc=::fstat(fd,&st);
    if(rc||!S_ISREG(st.st_mode)||st.st_nlink>1){const int saved=rc?errno:EINVAL;::close(fd);errno=saved;return nullptr;}
    FILE* log=::fdopen(fd,"ab");
    if(!log){const int saved=errno;::close(fd);errno=saved;}
    return log;
}
}
