// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cerrno>
#include <sys/stat.h>

// The pinned SDK's ps5_fstatat retains an optional lstat branch. The console
// refuses that operation with EPERM. Keep that failure explicit and local;
// never substitute following stat/open or invent metadata for a symlink.
extern "C" int __wrap_lstat(const char* path, struct stat*) {
    yakumo::ps5::Log log;
    log.write("[vulkan-compat] lstat unavailable path=%s errno=%d", path ? path : "<null>", EPERM);
    errno = EPERM;
    return -1;
}
