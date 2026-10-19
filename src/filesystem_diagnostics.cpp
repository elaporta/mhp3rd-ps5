// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace yakumo::ps5 {
void filesystem_diagnostics(const char* root, const char* label, char* summary, std::size_t capacity, Log& log) {
    // Independent, read-only operations: do not let a refused preflight hide
    // whether actual opens/reads work. 0 means success; -1 means not attempted.
    struct stat st{};
    int results[7]{};
    results[0] = stat(root, &st) == 0 ? 0 : errno;
    const int flags[] = {O_RDONLY, O_RDONLY | O_DIRECTORY, O_RDONLY | O_DIRECTORY | O_NOFOLLOW};
    results[4] = -1;
    for (int i = 0; i < 3; ++i) {
        const int fd = open(root, flags[i]);
        results[i + 1] = fd >= 0 ? 0 : errno;
        if (fd >= 0) {
            if (i == 0) results[4] = fstat(fd, &st) == 0 ? 0 : errno;
            if (close(fd)) log.write("[fs-api] close path=%s errno=%d", root, errno);
        }
    }
    char marker[path_capacity];
    results[5] = ENAMETOOLONG;
    results[6] = -1;
    if (join_path(marker, sizeof(marker), root, "yakumo/title.id")) {
        FILE* file = std::fopen(marker, "rb");
        results[5] = file ? 0 : errno;
        if (file) {
            char byte;
            errno = 0;
            results[6] = std::fread(&byte, 1, 1, file) == 1 ? 0 : (errno ? errno : EIO);
            if (std::fclose(file)) log.write("[fs-api] fclose path=%s errno=%d", marker, errno);
        }
    }
    std::snprintf(summary, capacity, "%s S/O/D/N/F/M/R=%d/%d/%d/%d/%d/%d/%d",
                  label, results[0], results[1], results[2], results[3], results[4], results[5], results[6]);
    log.write("[fs-api] root=%s %s (stat/open/directory/nofollow/fstat/marker-fopen/marker-read)", root, summary);
}
}
