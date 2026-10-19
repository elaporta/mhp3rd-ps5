// SPDX-License-Identifier: GPL-3.0-or-later
// Loopback integration must resolve numeric addresses locally. Accidentally
// reaching the SDK DNS path fails instead of consulting any external server.
#include <cstdint>
extern "C" {
int sceNetPoolCreate(const char*, int, int) { return -1001; }
int sceNetPoolDestroy(int) { return -1002; }
int sceNetResolverCreate(const char*, int, int) { return -1003; }
int sceNetResolverDestroy(int) { return -1004; }
int sceNetResolverStartNtoa(int, const char*, std::uint32_t*, int, int, int) { return -1005; }
int sceNetResolverAbort(int, int) { return -1006; }
}
