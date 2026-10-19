// SPDX-License-Identifier: GPL-3.0-or-later
// Independent listener/browser boundary: native handles cannot be BSD fds.
#include "adhoc_socket.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace web_fixture {
std::atomic<int> launches{};
std::atomic<int> launch_rc{};
int fail_create{},fail_bind{},fail_listen{},fail_nonblock{},fail_entropy{};
std::mutex lock;
std::map<int,int> descriptors;
int next=40000;
void check(bool ok) { if(!ok) throw std::runtime_error("Native web ABI contract mismatch"); }
int real(int handle) { std::lock_guard guard(lock);check(descriptors.count(handle));return descriptors.at(handle); }
int adopt(int fd) { if(fd<0)return fd;std::lock_guard guard(lock);const int handle=++next;descriptors[handle]=fd;return handle; }
}
namespace yakumo::adhoc_socket {
int create(int family,int type,int protocol) {
    using namespace web_fixture;
    check(family==2&&type==1&&protocol==6);
    if(fail_create)return -1;
    return adopt(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
}
bool set_nonblocking(int fd) {
    using namespace web_fixture;
    return !fail_nonblock&&fcntl(real(fd),F_SETFL,fcntl(real(fd),F_GETFL,0)|O_NONBLOCK)==0;
}
void tune(int fd) {web_fixture::check(web_fixture::real(fd)>=0);}
ssize_t send(int fd,const void* data,std::size_t length,int flags) {
    web_fixture::check(flags==0);return ::send(web_fixture::real(fd),data,length,MSG_NOSIGNAL);
}
ssize_t recv(int fd,void* data,std::size_t length,int flags) {
    web_fixture::check(flags==0);return ::recv(web_fixture::real(fd),data,length,0);
}
void close_socket(int fd) {
    if(fd<0)return;
    std::lock_guard guard(web_fixture::lock);
    web_fixture::check(web_fixture::descriptors.count(fd));
    ::close(web_fixture::descriptors.at(fd));web_fixture::descriptors.erase(fd);
}
}
extern "C" {
#if !YAKUMO_WEB_HOST_WEAK_FIXTURE
int sceNetBind(int fd,const sockaddr* address,socklen_t size) {
    using namespace web_fixture;
    check(size==16&&address->sa_family==2);
    const auto& in=*reinterpret_cast<const sockaddr_in*>(address);
    check(in.sin_addr.s_addr==htonl(0x7f000001)&&in.sin_port==htons(8844));
    if(fail_bind)return -1;
    const int one=1;setsockopt(real(fd),SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    return ::bind(real(fd),address,size);
}
int sceNetListen(int fd,int backlog) {
    web_fixture::check(backlog==4);
    return web_fixture::fail_listen ? -1 : ::listen(web_fixture::real(fd),backlog);
}
int sceNetAccept(int fd,sockaddr* address,socklen_t* size) {
    web_fixture::check(address==nullptr&&size==nullptr);
    return web_fixture::adopt(::accept(web_fixture::real(fd),nullptr,nullptr));
}
int sceSystemServiceLaunchWebBrowser(const char* uri,const void* options) {
    web_fixture::check(options==nullptr&&std::string(uri).starts_with("http://127.0.0.1:8844/#")&&std::string(uri).size()==87);
    ++web_fixture::launches;
    return web_fixture::launch_rc;
}
int sysctl(const int* mib,unsigned count,void* output,std::size_t* size,const void* input,std::size_t input_size) {
    web_fixture::check(mib&&mib[0]==1&&mib[1]==37&&count==2&&output&&size&&*size>0&&*size<=256&&!input&&!input_size);
    if(web_fixture::fail_entropy)return -1;
    auto* data=static_cast<unsigned char*>(output);
    static unsigned seed=0;
    for(std::size_t i=0;i<*size;++i)data[i]=static_cast<unsigned char>(i+seed);
    ++seed;return 0;
}
#endif
}
