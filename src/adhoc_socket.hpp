// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <sys/socket.h>

namespace yakumo::adhoc_socket {
// SceNet ABI facts from the pinned PS5 reference in ONLINE_ADHOC.md.
struct Event {
    std::uint32_t events{}, reserved{};
    std::uint64_t ident{}, data{};
};
static_assert(sizeof(Event)==24 && offsetof(Event,ident)==8 && offsetof(Event,data)==16);
inline constexpr std::size_t kMaxSockets=128;
int create(int family, int type, int protocol);
bool set_nonblocking(int socket);
void tune(int socket);
int connect(int socket,const sockaddr* address,socklen_t length);
int get_error(int socket,int* error,socklen_t* length);
ssize_t send(int socket,const void* bytes,std::size_t length,int flags);
ssize_t recv(int socket,void* bytes,std::size_t length,int flags);
void close_socket(int socket);
// Owned by the client network thread's run stack. Destruction precedes join.
class Poller {
public:
    Poller()=default;
    ~Poller();
    Poller(const Poller&)=delete;
    Poller& operator=(const Poller&)=delete;
    int wait(pollfd* entries,std::size_t count,int timeout_ms);
private:
    int id_{-1};
    std::array<int,kMaxSockets> registered_{};
    std::size_t registered_count_{};
    bool reported_idle_timeout_{};
    void reset();
};
}
