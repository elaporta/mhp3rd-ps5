// SPDX-License-Identifier: GPL-3.0-or-later
// Include the actual projected transport: fault injection exercises the code
// linked on PS5, including the formerly unchecked SO_ERROR query.
#include "adhoc/client.cpp"
#include <cstdarg>
#include <stdexcept>
#include <sys/socket.h>
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool fail_socket{}, fail_flags{}, fail_connect{}, fail_getopt{}, short_getopt{}, refuse{}, interrupted{};
bool fragmented{};
unsigned writes{}, reads{};
}
extern "C" {
int __real_socket(int,int,int);
int __real_fcntl(int,int,...);
int __real_connect(int,const sockaddr*,socklen_t);
int __real_getsockopt(int,int,int,void*,socklen_t*);
ssize_t __real_send(int,const void*,size_t,int);
ssize_t __real_recv(int,void*,size_t,int);
int __wrap_socket(int d,int t,int p) { if (fail_socket) { errno=EMFILE; return -1; } return __real_socket(d,t,p); }
int __wrap_fcntl(int fd,int command,...) {
    if (fail_flags) { errno=EBADF; return -1; }
    if (command==F_GETFL) return __real_fcntl(fd,command);
    va_list args; va_start(args,command); const int value=va_arg(args,int); va_end(args);
    return __real_fcntl(fd,command,value);
}
int __wrap_connect(int fd,const sockaddr* a,socklen_t n) {
    if (fail_connect) { errno=ECONNREFUSED; return -1; } return __real_connect(fd,a,n);
}
int __wrap_getsockopt(int fd,int level,int name,void* value,socklen_t* size) {
    if (fail_getopt) { errno=EBADF; return -1; }
    if (short_getopt) { *size=0; return 0; }
    if (refuse) { *static_cast<int*>(value)=ECONNREFUSED; return 0; }
    return __real_getsockopt(fd,level,name,value,size);
}
ssize_t __wrap_send(int fd,const void* data,size_t size,int flags) {
    ++writes;
    if (interrupted) { interrupted=false; errno=EINTR; return -1; }
    if (fragmented) size=std::min<std::size_t>(size,3);
    return __real_send(fd,data,size,flags);
}
ssize_t __wrap_recv(int fd,void* data,size_t size,int flags) {
    ++reads;
    if (fragmented) size=std::min<std::size_t>(size,5);
    return __real_recv(fd,data,size,flags);
}
}
int main() { try {
    using mhp3rd::adhoc::Link;
    const auto address=mhp3rd::adhoc::net::Address::ipv4_address(htonl(INADDR_LOOPBACK),1);
    Link link;
    fail_socket=true; check(!link.start(address)&&link.failed&&!link.active(),"socket allocation failure"); fail_socket=false;
    fail_flags=true; check(!link.start(address)&&link.failed&&!link.active(),"nonblocking setup failure closes socket"); fail_flags=false;
    fail_connect=true; check(!link.start(address)&&link.failed&&!link.active(),"immediate connect failure closes socket"); fail_connect=false;
    for(int mode=0;mode<3;++mode) {
        link.socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); link.open=false; link.failed=false;
        fail_getopt=mode==0; short_getopt=mode==1; refuse=mode==2;
        link.service(POLLOUT); check(link.failed&&!link.active()&&!link.open,"failed/short/refused connect completion cannot report success");
        fail_getopt=short_getopt=refuse=false;
    }
    int pair[2]; check(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0,"real stream pair");
    mhp3rd::adhoc::net::set_nonblocking(pair[0]); mhp3rd::adhoc::net::set_nonblocking(pair[1]);
    link.socket=pair[0]; link.open=true; link.failed=false; link.last_write=mhp3rd::adhoc::Clock::now();
    link.output="fragmented transport payload"; fragmented=true; interrupted=true;
    link.flush(); check(link.active()&&!link.output.empty(),"interrupted send preserves queued bytes");
    link.flush(); check(link.output.empty()&&writes>3,"partial writes drain exact queue");
    char bytes[128]{}; const auto count=__real_recv(pair[1],bytes,sizeof(bytes),0);
    check(count==28&&std::string(bytes,count)=="fragmented transport payload","partial writes have no loss or duplication");
    const std::string reply="partial receive payload";
    check(__real_send(pair[1],reply.data(),reply.size(),0)==static_cast<ssize_t>(reply.size()),"peer reply");
    for(int i=0;i<20&&link.input.size()<reply.size();++i)link.service(POLLIN);
    check(link.input==reply&&reads>1,"partial reads preserve exact bytes");
    close(pair[1]); link.service(POLLIN|POLLHUP); check(link.failed&&!link.active(),"peer close disconnects without SIGPIPE");
    link.close(); link.close();
    std::cout<<"native projected socket errors, partial I/O, interruption, disconnect and idempotent close PASS\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
