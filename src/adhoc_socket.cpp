// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_socket.hpp"
#include "adhoc_log.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" {
int sceNetSocket(const char*,int,int,int);
int sceNetSocketClose(int);
int sceNetConnect(int,const sockaddr*,socklen_t);
int sceNetSetsockopt(int,int,int,const void*,socklen_t);
int sceNetGetsockopt(int,int,int,void*,socklen_t*);
int sceNetSend(int,const void*,std::size_t,int);
int sceNetRecv(int,void*,std::size_t,int);
int* sceNetErrnoLoc();
int sceNetEpollCreate(const char*,int);
int sceNetEpollControl(int,int,int,yakumo::adhoc_socket::Event*);
int sceNetEpollWait(int,yakumo::adhoc_socket::Event*,int,int);
int sceNetEpollDestroy(int);
}
namespace yakumo::adhoc_socket {
namespace {
constexpr int kSocketLevel=0xffff,kNonblocking=0x1200,kError=0x1007;
constexpr unsigned kIn=1,kOut=2,kErr=8,kHup=16;
// SceNet order differs from Linux epoll: ADD=1, MOD=2, DEL=3.
constexpr int kAdd=1,kModify=2,kDelete=3;
// SceNet errno uses FreeBSD numbering even in the Linux ABI fixture.
int normalized(unsigned code) {
    if ((code&0xffffff00u)==0x80410100u) code&=255;
    switch(code) {
    case 0: return 0;
    case 2: return ENOENT;
    case 4: return EINTR;
    case 9: return EBADF;
    case 13: return EACCES;
    case 22: return EINVAL;
    case 35: return EWOULDBLOCK;
    case 36: return EINPROGRESS;
    case 54: return ECONNRESET;
    case 60: return ETIMEDOUT;
    case 61: return ECONNREFUSED;
    default: return EIO;
    }
}
void report(const char* stage,int rc,int native_errno,int error) {
    char line[256];
    std::snprintf(line,sizeof(line),"[ps5-adhoc-socket] backend=scenet stage=%s rc=%d rc_hex=0x%08x net_errno=%d errno=%d\n",
        stage,rc,static_cast<unsigned>(rc),native_errno,error);
    // Socket operations may run under the transport mutex. Defer all sink I/O.
    yakumo::adhoc_log::enqueue(std::string_view(line,std::strlen(line)-1),true);
    errno=error;
}
int native_errno() { const int* location=sceNetErrnoLoc(); return location ? *location : 0; }
int checked(int rc,const char* stage=nullptr) {
    if (rc>=0) return rc;
    const int raw_errno=native_errno();
    const unsigned code=rc==-1 ? static_cast<unsigned>(raw_errno) : static_cast<unsigned>(rc);
    const int error=normalized(code);
    errno=error ? error : EIO;
    if (stage) report(stage,rc,raw_errno,errno);
    return -1;
}
int failure(int error) { errno=error; return -1; }
}
int create(int family,int type,int protocol) {
    return checked(sceNetSocket("yakumo-adhoc",family,type,protocol),"create");
}
bool set_nonblocking(int socket) {
    const int enabled=1;
    return checked(sceNetSetsockopt(socket,kSocketLevel,kNonblocking,&enabled,sizeof(enabled)),"nonblocking")==0;
}
void tune(int socket) {
    const int enabled=1;
    // Optional hints: errors are logged but never turn a working link into a failure.
    checked(sceNetSetsockopt(socket,6,1,&enabled,sizeof(enabled)),"tcp-nodelay");
    checked(sceNetSetsockopt(socket,kSocketLevel,8,&enabled,sizeof(enabled)),"keepalive");
    checked(sceNetSetsockopt(socket,kSocketLevel,0x800,&enabled,sizeof(enabled)),"no-sigpipe");
}
int connect(int socket,const sockaddr* address,socklen_t length) {
    const int raw=sceNetConnect(socket,address,length);
    const int rc=checked(raw);
    if (rc<0 && errno!=EINPROGRESS && errno!=EINTR)
        report("connect",raw,native_errno(),errno);
    return rc;
}
int get_error(int socket,int* error,socklen_t* length) {
    const int rc=checked(sceNetGetsockopt(socket,kSocketLevel,kError,error,length),"connect-complete");
    if (rc==0 && *length==sizeof(*error)) *error=normalized(static_cast<unsigned>(*error));
    return rc;
}
ssize_t send(int socket,const void* bytes,std::size_t length,int) {
    // No BSD MSG_NOSIGNAL bit crosses this boundary; SO_NOSIGPIPE is set above.
    return checked(sceNetSend(socket,bytes,std::min(length,static_cast<std::size_t>(INT_MAX)),0));
}
ssize_t recv(int socket,void* bytes,std::size_t length,int) {
    return checked(sceNetRecv(socket,bytes,std::min(length,static_cast<std::size_t>(INT_MAX)),0));
}
void close_socket(int socket) {
    if (socket>=0) checked(sceNetSocketClose(socket),"close");
}
void Poller::reset() {
    if (id_>=0) checked(sceNetEpollDestroy(id_),"epoll-destroy");
    id_=-1;
    registered_count_=0;
}
Poller::~Poller() { reset(); }
int Poller::wait(pollfd* entries,std::size_t count,int timeout_ms) {
    if (count>kMaxSockets || (count && !entries) || timeout_ms<0 || timeout_ms>1000)
        return failure(EINVAL);
    for (std::size_t i=0;i<count;++i) entries[i].revents=0;
    if (count==0) {
        reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return 0;
    }
    if (id_<0) {
        id_=checked(sceNetEpollCreate("yakumo-adhoc",0),"epoll-create");
        if (id_<0) return -1;
    }
    // Remove registrations no longer present. A socket closed during the
    // unlocked wait may already have been removed by SceNet.
    for (std::size_t j=0;j<registered_count_;++j) {
        bool present=false;
        for (std::size_t i=0;i<count;++i) present|=entries[i].fd==registered_[j];
        if (!present) {
            const int raw=sceNetEpollControl(id_,kDelete,registered_[j],nullptr);
            if (checked(raw)<0 && errno!=ENOENT && errno!=EBADF) {
                const int error=errno;
                report("epoll-delete",raw,native_errno(),error);
                reset();
                return failure(error);
            }
        }
    }
    std::array<int,kMaxSockets> next{};
    std::size_t next_count{};
    for (std::size_t i=0;i<count;++i) {
        if (entries[i].fd<0) continue;
        Event event{};
        event.events=((entries[i].events&POLLIN)?kIn:0)|((entries[i].events&POLLOUT)?kOut:0);
        if (event.events==0) event.events=kErr|kHup;
        event.data=i;
        const bool known=std::find(registered_.begin(),registered_.begin()+registered_count_,entries[i].fd)!=registered_.begin()+registered_count_;
        // MOD then ADD handles a reused numeric socket whose old registration
        // was removed by close. Every wait refreshes the caller's index token.
        int rc=sceNetEpollControl(id_,known?kModify:kAdd,entries[i].fd,&event);
        if (rc<0 && known) rc=sceNetEpollControl(id_,kAdd,entries[i].fd,&event);
        if (checked(rc,"epoll-control")<0) {
            const int error=errno;
            reset();
            return failure(error);
        }
        next[next_count++]=entries[i].fd;
    }
    registered_=next;
    registered_count_=next_count;
    if (next_count==0) {
        reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return 0;
    }
    std::array<Event,kMaxSockets> ready{};
    const int raw=sceNetEpollWait(id_,ready.data(),static_cast<int>(next_count),timeout_ms*1000);
    const int found=checked(raw);
    if (found<0) {
        if (errno==ETIMEDOUT) {
            if (!reported_idle_timeout_) {
                report("epoll-idle-timeout",raw,native_errno(),errno);
                reported_idle_timeout_=true;
            }
            return 0;
        }
        report("epoll-wait",raw,native_errno(),errno);
        return -1;
    }
    if (found>static_cast<int>(next_count)) { reset(); return failure(EIO); }
    int changed{};
    for (int i=0;i<found;++i) {
        const Event& event=ready[i];
        if (event.data>=count || entries[event.data].fd<0 || event.ident!=static_cast<std::uint64_t>(entries[event.data].fd)) continue;
        auto& output=entries[event.data];
        const short flags=static_cast<short>(((event.events&kIn)?POLLIN:0)|((event.events&kOut)?POLLOUT:0)|
            ((event.events&kErr)?POLLERR:0)|((event.events&kHup)?POLLHUP:0));
        if (!output.revents && flags) ++changed;
        output.revents|=flags;
    }
    return changed;
}
}
