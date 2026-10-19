// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_resolver.hpp"
#include <array>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int pool_rc = 0, create_rc = 0, lookup_rc = 0, destroy_rc = 0, pool_destroy_rc = 0;
std::vector<std::string> calls;
std::mutex mutex;
std::condition_variable cv;
bool blocking{}, entered{}, aborted{};
void reset() {
    pool_rc = create_rc = lookup_rc = destroy_rc = pool_destroy_rc = 0;
    calls.clear();
}
}
extern "C" {
int sceNetPoolCreate(const char* name, int bytes, int flags) {
    check(std::string(name) == "yakumo-adhoc" && bytes == 0x4000 && flags == 0, "owned pool parameters");
    calls.push_back("pool-create"); return pool_rc;
}
int sceNetPoolDestroy(int id) { check(id == pool_rc, "owned pool released"); calls.push_back("pool-destroy"); return pool_destroy_rc; }
int sceNetResolverCreate(const char*, int pool, int flags) {
    check(pool == pool_rc && flags == 0, "resolver pool"); calls.push_back("resolver-create"); return create_rc;
}
int sceNetResolverDestroy(int id) { check(id == create_rc, "owned resolver released"); calls.push_back("resolver-destroy"); return destroy_rc; }
int sceNetResolverStartNtoa(int id, const char* host, std::uint32_t* address, int timeout, int retry, int flags) {
    check(id == create_rc && std::string(host) == "socom.cc" && retry == 1 && flags == 0, "native DNS parameters");
    calls.push_back("lookup");
    // Independent ABI oracle: reproduce the observed r1 EINVAL for 2. The
    // expected argument is deliberately not imported from the adapter header.
    if (timeout == 2) return static_cast<int>(0x80410116u);
    check(timeout == 2000000, "PS5 timeout must be two million microseconds, not two seconds units");
    const std::array<unsigned char,4> bytes{192,0,2,42}; std::memcpy(address, bytes.data(), 4);
    std::unique_lock lock(mutex);
    if (blocking) { entered = true; cv.notify_all(); cv.wait(lock, []{ return aborted; }); return -125; }
    return lookup_rc;
}
int sceNetResolverAbort(int id, int flags) {
    check(id == create_rc && flags == 0, "abort owned resolver only");
    std::lock_guard lock(mutex); aborted = true; cv.notify_all(); return 0;
}
}
int main() { try {
    using namespace yakumo::adhoc_resolver;
    for (const std::string& host : std::vector<std::string>{"", "bad host", "-bad.cc", "bad-.cc", ".bad", "bad..cc", "bad:27312", "256.0.0.1", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.4.", "1.2..4", ".", "::1", std::string("bad\0.cc",7), std::string(254,'x'), std::string(64,'a')+".cc"}) {
        auto r = resolve_ipv4(host); check(!r.ok() && r.address == 0 && calls.empty(), "invalid host performs no network call");
    }
    auto numeric = resolve_ipv4("127.0.0.1");
    const std::array<unsigned char,4> local{127,0,0,1};
    check(numeric.ok() && std::memcmp(&numeric.address, local.data(),4) == 0 && calls.empty(), "numeric IPv4 bypasses failing libc/DNS");
    check(resolve_ipv4("255.255.255.255").ok(), "IPv4 octet boundary");
    auto r = resolve_ipv4("socom.cc");
    check(r.ok() && r.address != 0 && calls == std::vector<std::string>{"pool-create","resolver-create","lookup","resolver-destroy","pool-destroy"}, "native DNS success and zero-valued handles released in order");
    reset(); pool_rc = -10; r = resolve_ipv4("socom.cc");
    check(!r.ok() && r.rc == -10 && calls.size() == 1, "pool failure owns no resources");
    reset(); create_rc = -11; r = resolve_ipv4("socom.cc");
    check(!r.ok() && r.rc == -11 && calls == std::vector<std::string>{"pool-create","resolver-create","pool-destroy"}, "resolver creation failure releases pool");
    reset(); lookup_rc = -12; r = resolve_ipv4("socom.cc");
    check(!r.ok() && r.address == 0 && r.rc == -12 && calls.size() == 5, "DNS failure discards partial address and releases resources");
    reset(); destroy_rc = -13; r = resolve_ipv4("socom.cc");
    check(!r.ok() && r.address == 0 && r.cleanup_rc == -13 && calls.back() == "pool-destroy", "resolver destroy failure still releases pool and fails closed");
    reset(); pool_destroy_rc = -14; r = resolve_ipv4("socom.cc");
    check(!r.ok() && r.cleanup_rc == -14, "pool destruction failure retained");
    reset(); destroy_rc = -13; pool_destroy_rc = -14; r = resolve_ipv4("socom.cc");
    check(r.cleanup_rc == -13, "first cleanup failure retained");
    reset(); blocking = true;
    auto pending = std::async(std::launch::async, []{ return resolve_ipv4("socom.cc"); });
    { std::unique_lock lock(mutex); check(cv.wait_for(lock, std::chrono::seconds(2),[]{ return entered; }), "lookup started"); }
    shutdown();
    check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "shutdown aborts in-flight DNS before joining");
    r = pending.get(); check(!r.ok() && std::string(r.stage) == "cancelled" && r.address == 0 && calls.size() == 5, "cancelled lookup releases both resources");
    const auto count = calls.size(); shutdown();
    check(!resolve_ipv4("socom.cc").ok() && !resolve_ipv4("127.0.0.1").ok() && calls.size() == count, "terminal shutdown is idempotent and prevents new lookups");
    std::cout << "native IPv4 resolution, fault, cleanup and concurrent cancellation contracts PASS\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
