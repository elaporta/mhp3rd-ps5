// SPDX-License-Identifier: GPL-3.0-or-later
// Independent SceNet ABI fixture. Virtual handles deliberately cannot be used
// with POSIX functions; the synthetic peer still uses real loopback TCP.
#include "adhoc_socket.hpp"
#include "adhoc_socket_fixture.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>
namespace socket_fixture {
Faults faults;
int creates{},closes{},epoll_creates{},epoll_destroys{},last_timeout{},last_flags{};
int sends{};
std::atomic<int> waits{};
namespace {
std::mutex mutex;
std::map<int,int> sockets;
std::map<int,std::map<int,yakumo::adhoc_socket::Event>> pollers;
int next_socket=50000,next_poller=90000;
thread_local int native_errno{};
int bsd_errno(int error) {
    switch(error) {
    case 0:return 0;
    case EINTR:return 4;
    case EBADF:return 9;
    case EACCES:return 13;
    case EINVAL:return 22;
    case EAGAIN:return 35;
    case EINPROGRESS:return 36;
    case ECONNRESET:return 54;
    case ECONNREFUSED:return 61;
    default:return 5;
    }
}
int fail(int code) {
    native_errno=code;
    return faults.direct_error ? static_cast<int>(0x80410100u|code) : -1;
}
int result(int rc) { return rc<0 ? fail(bsd_errno(errno)) : rc; }
void require(bool value) { if(!value) throw std::runtime_error("Independent native socket ABI mismatch"); }
}
int real_fd(int handle) { std::lock_guard lock(mutex); auto it=sockets.find(handle); return it==sockets.end()?-1:it->second; }
int live_sockets() { std::lock_guard lock(mutex); return sockets.size(); }
int live_pollers() { std::lock_guard lock(mutex); return pollers.size(); }
}
using namespace socket_fixture;
extern "C" {
int* sceNetErrnoLoc() { return faults.no_errno ? nullptr : &native_errno; }
int sceNetSocket(const char* name,int family,int type,int protocol) {
    require(std::strcmp(name,"yakumo-adhoc")==0 && family==2 && type==1 && protocol==6);
    if(faults.create) return fail(faults.create);
    int fd=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(fd<0) return result(fd);
    std::lock_guard lock(mutex); const int id=next_socket++; sockets[id]=fd; ++creates; return id;
}
int sceNetSocketClose(int id) {
    std::lock_guard lock(mutex);
    auto it=sockets.find(id); require(it!=sockets.end());
    ::close(it->second); sockets.erase(it); ++closes;
    for(auto& [unused,entries]:pollers) entries.erase(id);
    return faults.close ? fail(faults.close) : 0;
}
int sceNetSetsockopt(int id,int level,int option,const void* data,socklen_t size) {
    require(size==4 && *static_cast<const int*>(data)==1);
    if(faults.option) return fail(faults.option);
    const int fd=real_fd(id);
    if(level==0xffff && option==0x1200) return result(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK));
    if(level==0xffff && option==8) return result(::setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,data,size));
    if(level==0xffff && option==0x800) return 0; // Linux send uses MSG_NOSIGNAL below.
    require(level==6 && option==1);
    return result(::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,data,size));
}
int sceNetConnect(int id,const sockaddr* address,socklen_t length) {
    if(faults.connect) return fail(faults.connect);
    return result(::connect(real_fd(id),address,length));
}
int sceNetGetsockopt(int id,int level,int option,void* output,socklen_t* length) {
    require(level==0xffff && option==0x1007 && *length==4);
    if(faults.get) return fail(faults.get);
    if(faults.short_option) { *length=0; return 0; }
    const int rc=::getsockopt(real_fd(id),SOL_SOCKET,SO_ERROR,output,length);
    if(rc==0) *static_cast<int*>(output)=bsd_errno(*static_cast<int*>(output));
    return result(rc);
}
int sceNetSend(int id,const void* data,std::size_t size,int flags) {
    ++sends;
    last_flags=flags; require(flags==0);
    if(faults.send) return fail(faults.send);
    if(faults.fragment) size=std::min(size,faults.fragment);
    return result(::send(real_fd(id),data,size,MSG_NOSIGNAL));
}
int sceNetRecv(int id,void* data,std::size_t size,int flags) {
    require(flags==0);
    if(faults.recv) return fail(faults.recv);
    if(faults.fragment) size=std::min(size,faults.fragment);
    return result(::recv(real_fd(id),data,size,0));
}
int sceNetEpollCreate(const char* name,int flags) {
    require(std::strcmp(name,"yakumo-adhoc")==0 && flags==0);
    if(faults.epoll_create) return fail(faults.epoll_create);
    std::lock_guard lock(mutex); const int id=next_poller++; pollers[id]={}; ++epoll_creates; return id;
}
int sceNetEpollControl(int poller,int operation,int socket,yakumo::adhoc_socket::Event* event) {
    if(faults.control) return fail(faults.control);
    std::lock_guard lock(mutex); auto& entries=pollers.at(poller);
    // Independent SceNet opcode oracle; do not import adapter constants.
    if(operation==3) { require(!event); entries.erase(socket); return 0; }
    require(event && event->reserved==0 && event->ident==0 && event->events!=0 && (event->events&~0x1bu)==0);
    if(operation==2 && faults.modify_missing) { entries.erase(socket); return fail(2); }
    if(operation==2 && !entries.contains(socket)) return fail(2);
    require(operation==1 || operation==2);
    entries[socket]=*event; return 0;
}
int sceNetEpollWait(int poller,yakumo::adhoc_socket::Event* output,int capacity,int timeout) {
    ++waits;
    require(capacity>0 && capacity<=128 && timeout>=0 && timeout<=1000000);
    last_timeout=timeout;
    if(faults.wait) return fail(faults.wait);
    if(faults.bad_count) return 129;
    std::vector<pollfd> descriptors;
    std::vector<std::pair<int,yakumo::adhoc_socket::Event>> registrations;
    {
        std::lock_guard lock(mutex);
        for(const auto& [socket,event]:pollers.at(poller)) {
            registrations.emplace_back(socket,event);
            descriptors.push_back({sockets.at(socket),static_cast<short>(((event.events&1)?POLLIN:0)|((event.events&2)?POLLOUT:0)),0});
        }
    }
    int rc=::poll(descriptors.data(),descriptors.size(),timeout/1000);
    if(rc<0) return result(rc);
    int found{};
    for(std::size_t i=0;i<descriptors.size() && found<capacity;++i) if(descriptors[i].revents) {
        auto event=registrations[i].second; const auto bits=descriptors[i].revents;
        event.events=((bits&POLLIN)?1:0)|((bits&POLLOUT)?2:0)|((bits&(POLLERR|POLLNVAL))?8:0)|((bits&POLLHUP)?16:0);
        event.ident=registrations[i].first;
        if(faults.stale_event) event.data=999;
        output[found++]=event;
    }
    return found;
}
int sceNetEpollDestroy(int id) {
    std::lock_guard lock(mutex); require(pollers.erase(id)==1); ++epoll_destroys;
    return faults.destroy ? fail(faults.destroy) : 0;
}
}
