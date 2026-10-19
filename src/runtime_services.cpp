// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_probe.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#if YAKUMO_NATIVE
#include <pthread.h>
#include <pthread_np.h>
extern "C" void ps5_thread_stacks(unsigned*, unsigned*);
#endif

namespace yakumo::ps5 {
namespace {
using Clock = std::chrono::steady_clock;
thread_local unsigned tls_value = 17;
std::atomic<unsigned> tls_destructors{0};
struct TlsLifetime { ~TlsLifetime() { tls_destructors.fetch_add(1); } };
thread_local TlsLifetime tls_lifetime;
std::uint64_t monotonic_ns() {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) throw std::runtime_error("CLOCK_MONOTONIC failed");
    return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(time.tv_nsec);
}
bool threads_and_clock(Log& log, bool& cleaned) {
    std::mutex mutex;
    std::condition_variable wake;
    bool ready = false, release = false, worker_failed = false;
    unsigned shared = 0;
    std::atomic<unsigned> published{0};
    const auto before_tls = tls_destructors.load();
#if YAKUMO_NATIVE
    unsigned live_before = 0, cache_before = 0;
    std::size_t worker_stack = 0;
    int stack_result = 0;
    ps5_thread_stacks(&live_before, &cache_before);
#endif
    const auto begin = Clock::now();
    const auto mono_begin = monotonic_ns();
    std::thread worker([&] {
        // Every worker exception is reported to the main thread, never terminate().
        try {
            (void)&tls_lifetime;
            std::unique_lock<std::mutex> lock(mutex);
            worker_failed = tls_value != 17;
            tls_value = 91;
#if YAKUMO_NATIVE
            pthread_attr_t attributes;
            const int init = pthread_attr_init(&attributes);
            if (init == 0) {
                const int get = pthread_attr_get_np(pthread_self(), &attributes);
                const int size = get == 0 ? pthread_attr_getstacksize(&attributes, &worker_stack) : get;
                const int destroy = pthread_attr_destroy(&attributes);
                stack_result = get != 0 ? get : size != 0 ? size : destroy;
                worker_failed = worker_failed || stack_result != 0 || worker_stack < 2u * 1024u * 1024u;
            } else { worker_failed = true; stack_result = init; }
#endif
            ready = true;
            wake.notify_all();
            if (!wake.wait_for(lock, std::chrono::seconds(5), [&] { return release; })) worker_failed = true;
            for (unsigned n = 0; n < 1000; ++n) ++shared;
            published.store(42, std::memory_order_release);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            worker_failed = true;
            ready = true;
            wake.notify_all();
        }
    });
    bool handshake = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        handshake = wake.wait_for(lock, std::chrono::seconds(5), [&] { return ready; });
        for (unsigned n = 0; n < 1000; ++n) ++shared;
        release = true;
        wake.notify_all();
    }
    worker.join();
    cleaned = true;
    const bool exchange = handshake && !worker_failed && shared == 2000 &&
        published.load(std::memory_order_acquire) == 42 && tls_value == 17 &&
        tls_destructors.load() == before_tls + 1;
    // Empty predicate must actually wait, not merely return a timeout code.
    const auto wait_begin = Clock::now();
    const auto mono_wait_begin = monotonic_ns();
    bool premature = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        premature = wake.wait_for(lock, std::chrono::milliseconds(100), [] { return false; });
    }
    const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wait_begin).count();
    const auto mono_wait_end = monotonic_ns();
    const auto mono_end = monotonic_ns();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    bool monotonic = mono_end > mono_begin && elapsed > 0 && mono_wait_end >= mono_wait_begin;
    auto previous = monotonic_ns();
    auto previous_steady = Clock::now();
    for (unsigned n = 0; n < 32; ++n) {
        const auto now = monotonic_ns();
        const auto steady = Clock::now();
        monotonic = monotonic && now >= previous && steady >= previous_steady;
        previous = now;
        previous_steady = steady;
    }
    const auto measured_wait = mono_wait_end - mono_wait_begin;
    const bool timed_wait = !premature && wait_ns >= 50000000 && wait_ns < 5000000000ll &&
        measured_wait >= 50000000 && measured_wait < 5000000000ull;
    const auto mono_elapsed = mono_end - mono_begin;
    const auto difference = elapsed > static_cast<std::int64_t>(mono_elapsed) ?
        elapsed - static_cast<std::int64_t>(mono_elapsed) : static_cast<std::int64_t>(mono_elapsed) - elapsed;
    monotonic = monotonic && difference < 50000000;
#if YAKUMO_NATIVE
    unsigned live_after = 0, cache_after = 0;
    ps5_thread_stacks(&live_after, &cache_after);
    cleaned = live_after == live_before;
    log.write("[sdk-services] stacks live=%u->%u cached=%u->%u worker_bytes=%llu expected_minimum=2097152 attr_rc=%d cleanup=%s",
              live_before, live_after, cache_before, cache_after, static_cast<unsigned long long>(worker_stack),
              stack_result, cleaned ? "PASS" : "FAIL");
#endif
    log.write("[sdk-services] threads=%s shared=%u expected=2000 publication=%u expected=42 tls_destructors=%u expected=1",
              exchange ? "PASS" : "FAIL", shared, published.load(), tls_destructors.load() - before_tls);
    log.write("[sdk-services] timed_wait=%s steady_ns=%lld monotonic_ns=%llu expected=[50000000,5000000000)",
              timed_wait ? "PASS" : "FAIL", static_cast<long long>(wait_ns), static_cast<unsigned long long>(measured_wait));
    log.write("[sdk-services] clock=%s steady_ns=%lld monotonic_ns=%llu", monotonic ? "PASS" : "FAIL",
              static_cast<long long>(elapsed), static_cast<unsigned long long>(mono_elapsed));
    return exchange && timed_wait && monotonic && cleaned;
}
bool files(const TitlePaths& paths, Log& log, bool& cleaned) {
    char directory[path_capacity], original[path_capacity], renamed[path_capacity];
    if (!join_path(directory, sizeof(directory), paths.cache, "runtime-probe")) return false;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        log.write("[sdk-services] files FAIL mkdir errno=%d", errno); return false;
    }
    const int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) { log.write("[sdk-services] files FAIL nofollow-directory errno=%d", errno); return false; }
    struct stat info{};
    const bool directory_ok = fstat(dir, &info) == 0 && S_ISDIR(info.st_mode);
    const bool dir_closed = close(dir) == 0;
    if (!directory_ok || !dir_closed) return false;
    char name[128];
    std::snprintf(name, sizeof(name), "synthetic-%llu-%u", static_cast<unsigned long long>(monotonic_ns()),
                  static_cast<unsigned>(getpid()));
    if (!join_path(original, sizeof(original), directory, name)) return false;
    std::strncat(name, ".renamed", sizeof(name) - std::strlen(name) - 1);
    if (!join_path(renamed, sizeof(renamed), directory, name)) return false;
    constexpr std::array<unsigned char, 12> payload{0x59,0x41,0x4B,0x55,0x4D,0x4F,0x31,0x36,0,0xFF,0x55,0xAA};
    int fd = open(original, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { log.write("[sdk-services] files FAIL exclusive-create errno=%d", errno); return false; }
    bool ok = write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size());
    ok = (fsync(fd) == 0) && ok;
    ok = (close(fd) == 0) && ok;
    // Reserve the destination too: an existing unexpected file must not be overwritten.
    const int reservation = open(renamed, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    const bool owns_destination = reservation >= 0;
    bool moved = false;
    if (owns_destination) {
        ok = (close(reservation) == 0) && ok;
        moved = rename(original, renamed) == 0;
        ok = moved && ok;
    } else ok = false;
    if (moved) {
        fd = open(renamed, O_RDONLY | O_NOFOLLOW);
        if (fd >= 0) {
            std::array<unsigned char, 13> actual{};
            ok = (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == static_cast<off_t>(payload.size())) && ok;
            ok = (read(fd, actual.data(), actual.size()) == static_cast<ssize_t>(payload.size()) &&
                  std::memcmp(actual.data(), payload.data(), payload.size()) == 0) && ok;
            ok = (close(fd) == 0) && ok;
        } else ok = false;
    }
    const bool first_removed = moved || unlink(original) == 0;
    const bool second_removed = !owns_destination || unlink(renamed) == 0;
    cleaned = first_removed && second_removed;
    log.write("[sdk-services] files=%s bytes=12 expected=12 create/write/fsync/rename/open/fstat/read cleanup=%s path=%s",
              ok && cleaned ? "PASS" : "FAIL", cleaned ? "PASS" : "FAIL", directory);
    return ok && cleaned;
}
}
bool runtime_services(const TitlePaths& paths, Log& log, const RuntimeProbeOptions& options, bool& cleanup) {
    bool threads_clean = false, files_clean = false;
    const bool threads = threads_and_clock(log, threads_clean);
    const bool filesystem = files(paths, log, files_clean);
    cleanup = threads_clean && files_clean && !options.fail_cleanup;
    if (options.fail_sdk) log.write("[sdk-services] FAIL injected SDK result");
    if (options.fail_cleanup) log.write("[sdk-services] FAIL injected cleanup result");
    return threads && filesystem && !options.fail_sdk;
}
}
