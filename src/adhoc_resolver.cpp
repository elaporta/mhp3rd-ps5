// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_resolver.hpp"
#include <array>
#include <charconv>
#include <cstring>
#include <mutex>
#if YAKUMO_NATIVE
// Signatures follow the locked SDK libc/netdb.c and SceNet import stubs.
extern "C" {
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
int sceNetResolverCreate(const char*, int, int);
int sceNetResolverDestroy(int);
int sceNetResolverStartNtoa(int, const char*, std::uint32_t*, int, int, int);
int sceNetResolverAbort(int, int);
}
#else
#include <netdb.h>
#include <netinet/in.h>
#endif

namespace yakumo::adhoc_resolver {
namespace {
struct State {
    std::mutex mutex;
    bool stopped{};
    int resolver{-1};
};
State& state() {
    // Explicit shutdown owns the lifetime; no race with static destruction.
    static auto* value = new State;
    return *value;
}
bool hostname(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.back() == '.') host.remove_suffix(1);
    if (host.empty()) return false;
    std::size_t label{};
    char previous{};
    for (char c : host) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (c == '.') {
            if (label == 0 || previous == '-') return false;
            label = 0;
        } else {
            if ((!alnum && c != '-') || (label == 0 && c == '-') || ++label > 63) return false;
        }
        previous = c;
    }
    return label != 0 && previous != '-';
}
bool numeric(std::string_view host, std::uint32_t& address) {
    std::array<unsigned char, 4> bytes{};
    std::size_t start{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
        const auto end = host.find('.', start);
        if ((i == 3) != (end == std::string_view::npos)) return false;
        const auto part = host.substr(start, end == std::string_view::npos ? host.size() - start : end - start);
        unsigned value{};
        const auto [last, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0') ||
            error != std::errc{} || last != part.data() + part.size() || value > 255) return false;
        bytes[i] = static_cast<unsigned char>(value);
        start = end + 1;
    }
    std::memcpy(&address, bytes.data(), bytes.size());
    return true;
}
}

Result resolve_ipv4(std::string_view host) {
    Result result;
    if (!hostname(host)) return result;
    auto& s = state();
    // Serial callers by contract: Yakumo has one client network thread.
    // The mutex also protects abort versus resolver destruction.
    std::unique_lock lock(s.mutex);
    if (s.stopped) return {0, "cancelled", -1, 0};
    const bool digits = host.find_first_not_of("0123456789.") == std::string_view::npos;
    if (digits) {
        if (!numeric(host, result.address)) return result;
        result.stage = "numeric-ipv4";
        result.rc = 0;
        return result;
    }
    std::array<char, 254> name{};
    std::memcpy(name.data(), host.data(), host.size());
#if YAKUMO_NATIVE
    const int pool = sceNetPoolCreate("yakumo-adhoc", 0x4000, 0);
    if (pool < 0) return {0, "pool-create", pool, 0};
    const int resolver = sceNetResolverCreate("yakumo-adhoc", pool, 0);
    result.stage = "resolver-create";
    result.rc = resolver;
    if (resolver >= 0) {
        s.resolver = resolver;
        lock.unlock();
        result.stage = "native-dns-ipv4";
        result.rc = sceNetResolverStartNtoa(resolver, name.data(), &result.address, kTimeoutMicroseconds, kRetries, 0);
        lock.lock();
        s.resolver = -1;
        const int destroy = sceNetResolverDestroy(resolver);
        if (destroy < 0) result.cleanup_rc = destroy;
    }
    const int destroy_pool = sceNetPoolDestroy(pool);
    if (result.cleanup_rc == 0 && destroy_pool < 0) result.cleanup_rc = destroy_pool;
#else
    lock.unlock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list{};
    result.stage = "host-dns-ipv4";
    result.rc = getaddrinfo(name.data(), nullptr, &hints, &list);
    if (result.rc == 0 && list != nullptr) result.address = reinterpret_cast<sockaddr_in*>(list->ai_addr)->sin_addr.s_addr;
    else if (result.rc == 0) result.rc = EAI_FAIL;
    if (list != nullptr) freeaddrinfo(list);
    lock.lock();
#endif
    if (s.stopped) { result.stage = "cancelled"; result.rc = -1; }
    if (!result.ok()) result.address = 0;
    return result;
}

void shutdown() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.stopped = true;
#if YAKUMO_NATIVE
    if (s.resolver >= 0) (void)sceNetResolverAbort(s.resolver, 0);
#endif
}
}
