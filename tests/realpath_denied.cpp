// SPDX-License-Identifier: GPL-3.0-or-later
#include <cerrno>
#include <sys/stat.h>
int realpath_calls = 0;
int lstat_calls = 0;
bool deny_stat = false;
extern "C" char* __wrap_realpath(const char*, char*) {
    ++realpath_calls;
    errno = EPERM;
    return nullptr;
}
extern "C" int __wrap_lstat(const char*, struct stat*) {
    ++lstat_calls;
    errno = EPERM;
    return -1;
}
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* path, struct stat* status) {
    if (!deny_stat) return __real_stat(path, status);
    errno = EPERM;
    return -1;
}
