// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cstdarg>
#include <cstring>
#if YAKUMO_NATIVE
extern "C" int sceKernelDebugOutText(int, const char*);
#endif

namespace yakumo::ps5 {
Log::~Log() { if (file_) std::fclose(file_); }
void Log::write(const char* format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::printf("%s\n", line);
    std::fflush(stdout);
#if YAKUMO_NATIVE
    char kernel_line[2100];
    std::snprintf(kernel_line, sizeof(kernel_line), "[yakumo-probe] %s\n", line);
    sceKernelDebugOutText(0, kernel_line);
#endif
    if (file_) {
        std::fprintf(file_, "%s\n", line);
        std::fflush(file_);
    } else {
        const auto n = std::strlen(line);
        if (used_ + n + 1 < sizeof(early_)) {
            std::memcpy(early_ + used_, line, n);
            used_ += n;
            early_[used_++] = '\n';
        }
    }
}
bool Log::attach(const TitlePaths& paths) {
    if (file_) return false;
    char name[path_capacity];
    if (!join_path(name, sizeof(name), paths.logs, "probe.log")) return false;
    file_ = std::fopen(name, "ab");
    if (!file_) return false;
    const bool ok = std::fwrite(early_, 1, used_, file_) == used_ && std::fflush(file_) == 0;
    used_ = 0;
    return ok;
}
}
