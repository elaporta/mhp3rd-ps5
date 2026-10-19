/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linux-only fault injection against the real program's system calls. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int is_fault(const char *name) {
    const char *fault = getenv("YAKUMO_TEST_FAULT");
    return fault && !strcmp(fault, name);
}
int fstat(int fd, struct stat *st) {
    int (*actual)(int, struct stat *) = dlsym(RTLD_NEXT, "fstat");
    int rc = actual(fd, st);
    if (!rc && S_ISREG(st->st_mode) && is_fault("unknown-links")) st->st_nlink = 0;
    if (!rc && is_fault("unknown-inode")) st->st_ino = 0;
    return rc;
}
int fchmod(int fd, mode_t mode) {
    int (*actual)(int, mode_t) = dlsym(RTLD_NEXT, "fchmod");
    static unsigned calls;
    ++calls;
    if (is_fault("ignored-chmod")) return 0;
    if (is_fault("denied-chmod")) { errno = EPERM; return -1; }
    if (is_fault("partial-chmod") && calls == 2) { errno = EPERM; return -1; }
    if (is_fault("late-file") && calls == 1) {
        const char *root = getenv("YAKUMO_TEST_ROOT");
        char path[2048];
        if (root && snprintf(path, sizeof(path), "%s/late-file", root) < (int)sizeof(path)) {
            int extra = open(path, O_WRONLY|O_CREAT|O_EXCL, 0666);
            if (extra >= 0) close(extra);
        }
    }
    return actual(fd, mode);
}
