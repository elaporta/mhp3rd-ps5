/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "payload.h"
#include "permission_bootstrap_status.h"
static int secondary, missing, refused, sends, debug_calls;
static char received[3075];
static int notification(int device, void *buffer, unsigned long size, int blocking) {
    assert(device == 0 && blocking == 0 && size == 3120);
    unsigned char *bytes = buffer;
    for (unsigned i=0; i<45; ++i) assert(bytes[i] == 0);
    memcpy(received, bytes+45, sizeof(received));
    assert(received[3074] == 0);
    ++sends;
    return refused ? -42 : 0;
}
static int debug(int device, const char *text) {
    assert(device == 0 && text && *text);
    ++debug_calls;
    return 0;
}
static int resolve(int handle, const char *name, void *result) {
    if (secondary && handle != 0x2001) return -1;
    if (!strcmp(name, "sceKernelSendNotificationRequest")) {
        if (missing) return -1;
        *(void **)result = (void *)notification; return 0;
    }
    if (!strcmp(name, "sceKernelDebugOutText")) {
        *(void **)result = (void *)debug; return 0;
    }
    return -1;
}
int main(void) {
    payload_args_t args = {.sys_dynlib_dlsym=resolve};
    assert(permission_bootstrap_status(NULL, "test") == -1);
    payload_args_t absent = {0};
    assert(permission_bootstrap_status(&absent, "test") == -1);
    assert(permission_bootstrap_status(&args, NULL) == -1);
    assert(permission_bootstrap_status(&args, "ENTRY") == 0);
    assert(sends == 1 && debug_calls == 1 && !strcmp(received, "ENTRY"));
    secondary = 1;
    assert(permission_bootstrap_status(&args, "SECONDARY") == 0);
    assert(sends == 2 && !strcmp(received, "SECONDARY"));
    refused = 1;
    assert(permission_bootstrap_status(&args, "REFUSED") == -42);
    assert(debug_calls == 4);
    missing = 1;
    assert(permission_bootstrap_status(&args, "MISSING") == -1);
    assert(sends == 3 && debug_calls == 6);
    missing = refused = 0;
    char long_text[4096]; memset(long_text, 'x', sizeof(long_text)); long_text[4095] = 0;
    assert(permission_bootstrap_status(&args, long_text) == 0);
    assert(strlen(received) == 3074);
    puts("permission bootstrap delivery/fallback/bounds PASS");
    return 0;
}
