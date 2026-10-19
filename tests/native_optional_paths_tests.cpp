// SPDX-License-Identifier: GPL-3.0-or-later
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern "C" int __wrap_lstat(const char*, struct stat*);
int main() {
    struct stat status;
    std::memset(&status, 0x5a, sizeof(status));
    unsigned char original[sizeof(status)];
    std::memcpy(original, &status, sizeof(status));
    errno = 0;
    if (__wrap_lstat("/an/unsupported/optional/path", &status) != -1 || errno != EPERM ||
        std::memcmp(original, &status, sizeof(status)) != 0) return 1;
    errno = 0;
    if (__wrap_lstat(nullptr, nullptr) != -1 || errno != EPERM) return 2;
    std::puts("Optional native lstat fails closed without fabricated metadata");
    return 0;
}
