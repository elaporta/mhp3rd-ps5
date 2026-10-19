// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace yakumo::ps5 {
bool storage_probe(const TitlePaths& paths, Log& log) {
    char destination[path_capacity], temporary[path_capacity];
    if (!join_path(destination, sizeof(destination), paths.cache, "probe-persistence.txt") ||
        !join_path(temporary, sizeof(temporary), paths.cache, "probe-persistence.tmp")) return false;
    constexpr char sentinel[] = "Yakumo-PS5 persistent storage probe v1\n";
    FILE* old = std::fopen(destination, "rb");
    if (old) {
        char data[sizeof(sentinel)]{};
        const auto n = std::fread(data, 1, sizeof(data), old);
        std::fclose(old);
        if (n != sizeof(sentinel) - 1 || std::memcmp(data, sentinel, n)) {
            log.write("[storage] FAIL existing sentinel differs; preserving it");
            return false;
        }
        log.write("[storage] previous-run sentinel verified");
    } else if (errno != ENOENT) {
        log.write("[storage] FAIL read errno=%d", errno);
        return false;
    } else log.write("[storage] first run; persistence needs a second launch");
    // O_EXCL prevents clobbering an unexpected file or following a symlink.
    const int fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) { log.write("[storage] FAIL create errno=%d", errno); return false; }
    bool ok = write(fd, sentinel, sizeof(sentinel) - 1) == static_cast<ssize_t>(sizeof(sentinel) - 1);
    if (ok) ok = fsync(fd) == 0;
    const int error = errno;
    if (close(fd)) ok = false;
    if (ok) ok = rename(temporary, destination) == 0;
    if (!ok) {
        log.write("[storage] FAIL write/fsync/close/rename errno=%d current=%d; temp retained", error, errno);
        return false;
    }
    FILE* readback = std::fopen(destination, "rb");
    char bytes[sizeof(sentinel)]{};
    if (!readback) return false;
    const auto n = std::fread(bytes, 1, sizeof(bytes), readback);
    std::fclose(readback);
    ok = n == sizeof(sentinel) - 1 && !std::memcmp(bytes, sentinel, n);
    log.write("[storage] %s create/write/fsync/rename/read", ok ? "PASS" : "FAIL");
    return ok;
}
}
