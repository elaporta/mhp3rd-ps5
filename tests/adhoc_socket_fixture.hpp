// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <atomic>
namespace socket_fixture {
struct Faults {
    int create{}, option{}, connect{}, get{}, send{}, recv{}, close{}, epoll_create{}, control{}, wait{}, destroy{};
    bool short_option{}, stale_event{}, bad_count{}, no_errno{}, direct_error{}, modify_missing{};
    std::size_t fragment{};
};
extern Faults faults;
extern int creates, closes, epoll_creates, epoll_destroys, last_timeout, last_flags;
extern int sends;
extern std::atomic<int> waits;
int real_fd(int handle);
int live_sockets();
int live_pollers();
}
