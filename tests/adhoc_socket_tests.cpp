// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_socket.hpp"
#include "adhoc_socket_fixture.hpp"
#include "adhoc_log.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sstream>
#include <unistd.h>
namespace native=yakumo::adhoc_socket;
using socket_fixture::faults;
void check(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
int main(int argc,char** argv) { try {
    const bool timeout_only=argc>1 && std::string(argv[1])=="--timeout-only";
    const bool check_logging=argc==1;
    using namespace socket_fixture;
    std::ostringstream captured;
    struct Redirect { std::streambuf* previous; ~Redirect(){std::cout.rdbuf(previous);} } redirect{std::cout.rdbuf(captured.rdbuf())};
    for(int error:{2,4,9,13,22,35,36,54,60,61,123}) {
        faults.create=error;
        check(native::create(2,1,6)==-1 && errno!=0,"native create errors normalize and preserve failure");
    }
    faults.direct_error=true; faults.create=13;
    check(native::create(2,1,6)==-1 && errno==EACCES,"encoded native return independent of CRT errno");
    yakumo::adhoc_log::finish();
    if(check_logging) check(captured.str().find("stage=create rc=")!=std::string::npos && captured.str().find("rc_hex=0x8041010d")!=std::string::npos,"native errors reach the exact C++ stream captured by GameLog");
    faults={}; faults.create=13; faults.no_errno=true;
    check(native::create(2,1,6)==-1 && errno==EIO,"missing native errno fails explicitly");
    faults={};
    const int listener=::socket(AF_INET,SOCK_STREAM,0);
    check(listener>=0,"real loopback listener");
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && ::listen(listener,1)==0,"listen");
    socklen_t size=sizeof(address); check(::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)==0,"ephemeral port");
    int socket=native::create(2,1,6);
    check(socket>=50000 && ::fcntl(socket,F_GETFL)==-1,"native handles cannot escape into BSD I/O");
    faults.option=13; check(!native::set_nonblocking(socket),"nonblocking error is separate from create");
    native::tune(socket); faults.option=0;
    check(native::set_nonblocking(socket),"PS5 SO_NBIO 0x1200"); native::tune(socket);
    if(timeout_only) {
        bool idle{};
        { native::Poller poller; pollfd entry{socket,POLLOUT,0}; faults.wait=60; idle=poller.wait(&entry,1,2)==0; }
        faults={}; native::close_socket(socket); ::close(listener);
        check(idle,"native idle timeout preserves pending connection and epoll");
        std::cout.rdbuf(redirect.previous);
        std::cout<<"native idle timeout regression PASS\n";
        return 0;
    }
    faults.connect=61; check(native::connect(socket,reinterpret_cast<sockaddr*>(&address),size)==-1 && errno==ECONNREFUSED,"real connect failure");
    faults.connect=0;
    const int connected=native::connect(socket,reinterpret_cast<sockaddr*>(&address),size);
    check(connected==0 || errno==EINPROGRESS,"nonblocking pending connect translated from native errno");
    {
        native::Poller poller;
        pollfd entries[2]{{socket,POLLOUT,0},{-1,POLLIN,0}};
        faults.epoll_create=13; check(poller.wait(entries,2,2)==-1,"epoll creation failure"); faults.epoll_create=0;
        faults.control=13; check(poller.wait(entries,2,2)==-1 && live_pollers()==0,"registration error releases epoll"); faults.control=0;
        check(poller.wait(entries,2,2)>0 && (entries[0].revents&POLLOUT) && !entries[1].revents,"native OUT bit 2 maps to POSIX OUT");
        check(last_timeout==2000,"epoll microseconds independent oracle");
        const int before=epoll_creates;
        check(poller.wait(entries,2,0)>0 && epoll_creates==before,"persistent poller reused");
        faults.modify_missing=true; check(poller.wait(entries,2,0)>0,"closed/reused registration recovers with ADD"); faults.modify_missing=false;
        faults.stale_event=true; check(poller.wait(entries,2,0)==0,"stale index cannot target another link"); faults.stale_event=false;
        faults.wait=4; check(poller.wait(entries,2,0)==-1 && errno==EINTR,"interrupted native wait"); faults.wait=0;
        const int live=live_pollers();
        for(bool direct:{false,true}) {
            faults.direct_error=direct; faults.wait=60;
            check(poller.wait(entries,2,2)==0 && !entries[0].revents && live_pollers()==live,"native idle timeout preserves pending connection and epoll");
        }
        faults.direct_error=false; faults.wait=13;
        check(poller.wait(entries,2,0)==-1 && errno==EACCES,"fatal native wait stays an error");
        yakumo::adhoc_log::finish();
        check(captured.str().find("stage=epoll-wait rc=-1")!=std::string::npos,"raw wait failures reach game stream");
        faults.wait=0;
        entries[0].events=0;
        check(poller.wait(entries,2,0)==0,"paused receive retains error-only native interest without an invalid zero mask");
        entries[0].events=POLLOUT;
        int error{}; socklen_t error_size=4;
        faults.get=13; check(native::get_error(socket,&error,&error_size)==-1,"SO_ERROR failure"); faults.get=0;
        faults.short_option=true; check(native::get_error(socket,&error,&error_size)==0 && error_size==0,"short SO_ERROR preserved for Link rejection"); faults.short_option=false;
        error_size=4; check(native::get_error(socket,&error,&error_size)==0 && error==0,"successful connect completion");
        const int peer=::accept(listener,nullptr,nullptr); check(peer>=0,"accepted native socket via real TCP");
        char bytes[32]{};
        check(native::recv(socket,bytes,sizeof(bytes),0)==-1 && errno==EWOULDBLOCK,"empty nonblocking read");
        faults.send=4; check(native::send(socket,"abcd",4,123)==-1 && errno==EINTR,"interrupted send normalized"); faults.send=0;
        faults.recv=54; check(native::recv(socket,bytes,4,0)==-1 && errno==ECONNRESET,"receive failure normalized"); faults.recv=0;
        faults.fragment=2;
        check(native::send(socket,"abcd",4,123)==2 && last_flags==0,"partial native write and no BSD flags");
        check(::recv(peer,bytes,4,0)==2 && std::memcmp(bytes,"ab",2)==0,"exact written bytes");
        check(::send(peer,"reply",5,0)==5,"peer reply");
        entries[0].events=POLLIN; check(poller.wait(entries,2,2)>0 && (entries[0].revents&POLLIN),"native IN event");
        check(native::recv(socket,bytes,5,0)==2 && std::memcmp(bytes,"re",2)==0,"fragmented native read");
        faults.fragment=0;
        check(native::recv(socket,bytes,5,0)==3,"remaining bytes");
        ::close(peer);
        check(poller.wait(entries,2,2)>0 && native::recv(socket,bytes,5,0)==0,"peer disconnect");
        entries[0].fd=-1; faults.control=13;
        check(poller.wait(entries,2,0)==-1 && live_pollers()==0,"delete failure releases stale registrations"); faults.control=0;
        entries[0].fd=socket; check(poller.wait(entries,2,0)>=0,"recreate registration");
        entries[0].fd=-1; check(poller.wait(entries,2,0)==0,"stale registration removed");
        entries[0].fd=socket; faults.bad_count=true;
        check(poller.wait(entries,2,0)==-1 && live_pollers()==0,"invalid native result count closes poller"); faults.bad_count=false;
        check(poller.wait(nullptr,0,0)==0,"empty wait without poller");
        check(poller.wait(nullptr,1,0)==-1 && poller.wait(entries,129,0)==-1 && poller.wait(entries,2,-1)==-1 && poller.wait(entries,2,1001)==-1,"bounded poll input");
        check(poller.wait(entries,2,0)>=0,"poller recreated after failure");
        faults.destroy=13; check(poller.wait(nullptr,0,0)==0 && live_pollers()==0,"destroy failure reported during empty reset"); faults.destroy=0;
        check(poller.wait(entries,2,0)>=0,"poller destructor owns remaining resource");
    }
    faults.close=13; native::close_socket(socket); native::close_socket(-1); faults={}; ::close(listener);
    check(live_sockets()==0 && live_pollers()==0 && creates==closes && epoll_creates==epoll_destroys,"all socket and epoll resources released");
    std::cout.rdbuf(redirect.previous);
    std::cout<<"SceNet ABI, game-stream errors, nonblocking, event translation, idle timeouts, virtual handles, fragmentation and cleanup PASS\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; } }
