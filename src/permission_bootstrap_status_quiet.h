/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Kernel-log-only pre-relocation status; user notifications are reserved for
 * the identified payload's single start and final result messages. */
#ifndef MHP3RD_PERMISSION_BOOTSTRAP_STATUS_QUIET_H
#define MHP3RD_PERMISSION_BOOTSTRAP_STATUS_QUIET_H
static int permission_bootstrap_status(payload_args_t *args, const char *text) {
    if (!args || !args->sys_dynlib_dlsym || !text) return -1;
    int (*debug_text)(int, const char *) = 0;
    const int handles[] = {0x1, 0x2001};
    for (unsigned i = 0; i < 2 && !debug_text; ++i) {
        void *symbol = 0;
        if (!args->sys_dynlib_dlsym(handles[i], "sceKernelDebugOutText", &symbol))
            debug_text = (int (*)(int, const char *))symbol;
    }
    if (debug_text) debug_text(0, text);
    return debug_text ? 0 : -1;
}
#endif
