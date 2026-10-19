// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>

namespace yakumo::adhoc_resolver {
// Address is in network byte order. No allocation or libc addrinfo ownership
// crosses the native resolver boundary. An error never supplies an address.
struct Result {
    std::uint32_t address{};
    const char* stage{"invalid-host"};
    int rc{-1};
    int cleanup_rc{};
    bool ok() const { return rc == 0 && cleanup_rc == 0; }
};
// Native PS5 SceNet uses microseconds; the guest PSP resolver uses seconds.
inline constexpr int kTimeoutMicroseconds = 2'000'000;
inline constexpr int kRetries = 1;
Result resolve_ipv4(std::string_view host);
// Terminal application shutdown; cancel the current native lookup before
// joining its owning network thread. Never destroys another thread's buffers.
void shutdown() noexcept;
}
