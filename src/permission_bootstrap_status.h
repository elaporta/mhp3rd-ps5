/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Before libc relocation: use only the loader's documented dlsym callback.
 * The caller includes the pinned SDK payload.h first. No kernel patch, file
 * write or permission change is performed here. */
#ifndef YAKUMO_PERMISSION_BOOTSTRAP_STATUS_H
#define YAKUMO_PERMISSION_BOOTSTRAP_STATUS_H
static int permission_bootstrap_status(payload_args_t *args, const char *text) {
    if (!args || !args->sys_dynlib_dlsym || !text) return -1;
    int (*send_notification)(int, void *, unsigned long, int) = 0;
    int (*debug_text)(int, const char *) = 0;
    const int handles[] = {0x1, 0x2001};
    for (unsigned i = 0; i < 2 && !send_notification; ++i) {
        void *symbol = 0;
        if (!args->sys_dynlib_dlsym(handles[i], "sceKernelSendNotificationRequest", &symbol))
            send_notification = (int (*)(int, void *, unsigned long, int))symbol;
    }
    for (unsigned i = 0; i < 2 && !debug_text; ++i) {
        void *symbol = 0;
        if (!args->sys_dynlib_dlsym(handles[i], "sceKernelDebugOutText", &symbol))
            debug_text = (int (*)(int, const char *))symbol;
    }
    /* Volatile byte stores prevent a compiler-generated libc memset/memcpy
     * before the SDK has relocated its dynamic imports. */
    volatile struct { char prefix[45]; char message[3075]; } request;
    volatile unsigned char *bytes = (volatile unsigned char *)&request;
    for (unsigned long i = 0; i < sizeof(request); ++i) bytes[i] = 0;
    for (unsigned i = 0; i < sizeof(request.message)-1 && text[i]; ++i)
        request.message[i] = text[i];
    int rc = send_notification ? send_notification(0, (void *)&request, sizeof(request), 0) : -1;
    if (debug_text) {
        debug_text(0, text);
        if (rc) debug_text(0, "[title-permissions-0.1.2] notification delivery FAILED; repair success is not established by loader transfer status");
    }
    return rc;
}
#endif
