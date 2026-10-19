// SPDX-License-Identifier: GPL-3.0-or-later
// Inject faults at the actual descriptor syscalls used by game_files.cpp.
#include "game_files.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
namespace safe=yakumo::game_files;
namespace {
enum class Fault { None, DenyChmod, BeforeStat, AfterStat, WrongType, WrongMode, WrongObject, Hardlink };
Fault fault{};
ino_t target{};
unsigned stats{},changes{};
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __real_fchmod(int,mode_t);
extern "C" int __wrap_fstat(int fd,struct stat* st){
    const int rc=__real_fstat(fd,st);
    if(rc||st->st_ino!=target)return rc;
    ++stats; // open_file validation, set_mode precheck, set_mode readback.
    if((fault==Fault::BeforeStat&&stats==2)||(fault==Fault::AfterStat&&stats==3)){errno=EIO;return -1;}
    if(fault==Fault::WrongType&&stats==2)st->st_mode=(st->st_mode&~S_IFMT)|S_IFDIR;
    if(stats==3){
        if(fault==Fault::WrongMode)st->st_mode=(st->st_mode&~07777)|0600;
        if(fault==Fault::WrongObject)++st->st_ino;
        if(fault==Fault::Hardlink)st->st_nlink=2;
    }
    return rc;
}
extern "C" int __wrap_fchmod(int fd,mode_t mode){
    struct stat st{};
    if(!__real_fstat(fd,&st)&&st.st_ino==target){
        ++changes;if(fault==Fault::DenyChmod){errno=EPERM;return -1;}
    }
    return __real_fchmod(fd,mode);
}
int main(){
    char temp[]="/tmp/yakumo-modes-XXXXXX";
    const char* created=mkdtemp(temp);if(!created)return 1;
    fs::path root=created;
    try{
        fs::create_directories(root/"yakumo/cache");safe::set_root(root);
        const auto name=root/"yakumo/cache/file";
        int fd=open(name.c_str(),O_CREAT|O_WRONLY|O_EXCL,0666);
        check(fd>=0&&write(fd,"keep",4)==4,"fixture content");
        struct stat st{};check(!__real_fstat(fd,&st),"fixture identity");target=st.st_ino;close(fd);
        auto run=[&](mode_t mode,Fault inject,bool success,int error,unsigned expected_changes){
            check(!chmod(name.c_str(),mode),"fixture mode");fault=inject;stats=changes=0;
            const int opened=safe::open_file(name,O_WRONLY|O_TRUNC);
            const int saved=errno;
            check((opened>=0)==success,"correct syscall outcome");
            if(opened>=0){check(write(opened,"keep",4)==4,"restore successful truncation");close(opened);}
            else check(saved==error&&fs::file_size(name)==4,"error and bytes preserved before truncation");
            check(changes==expected_changes,"exact mutation count");
        };
        run(0666,Fault::DenyChmod,true,0,0);
        run(0600,Fault::DenyChmod,false,EPERM,1);
        run(0666,Fault::BeforeStat,false,EIO,0);
        run(0666,Fault::WrongType,false,EPERM,0);
        run(0600,Fault::AfterStat,false,EIO,1);
        run(0600,Fault::WrongMode,false,EACCES,1);
        run(0600,Fault::WrongObject,false,EACCES,1);
        run(0600,Fault::Hardlink,false,EACCES,1);
        run(04666,Fault::None,true,0,1);
        check(!stat(name.c_str(),&st)&&(st.st_mode&07777)==0666,"special bits cleared");
        fault=Fault::None;stats=changes=0;
        fs::create_hard_link(name,root/"yakumo/cache/alias");
        check(safe::open_file(name,O_WRONLY|O_TRUNC)<0&&errno==EPERM&&changes==0&&fs::file_size(name)==4,
              "already-correct hardlink rejects before content or mode mutation");
        fs::remove(root/"yakumo/cache/alias");
        fault=Fault::None;target=0;
        check(!chmod(root.c_str(),01777),"sticky root fixture");
        safe::repair_title_permissions();
        check(!stat(root.c_str(),&st)&&(st.st_mode&07777)==0777,"sticky directory is not already correct");
        // A loader input already at 0777 must never reach fchmod, including
        // a foreign-owner EPERM environment. Missing execute bits still fail.
        const auto program=root/"eboot.bin";
        {int out=open(program.c_str(),O_CREAT|O_WRONLY,0777);check(out>=0,"program fixture");close(out);}
        check(!chmod(program.c_str(),0777)&&!stat(program.c_str(),&st),"program identity");
        target=st.st_ino;fault=Fault::DenyChmod;stats=changes=0;
        safe::repair_title_permissions();check(changes==0,"exact native program needs no mutation");
        check(!chmod(program.c_str(),0666),"missing program execute bits");stats=changes=0;
        bool denied=false;try{safe::repair_title_permissions();}catch(const fs::filesystem_error& e){denied=e.code().value()==EPERM;}
        check(denied&&changes==1,"missing execute bits do not silently pass EPERM");
        fault=Fault::None;target=0;safe::repair_title_permissions();
        fs::remove(program);fs::create_directory(program);
        bool wrong_type=false;try{safe::repair_title_permissions();}catch(const fs::filesystem_error&){wrong_type=true;}
        check(wrong_type,"native program directory rejected");
        fs::remove_all(root);
        std::cout<<"mode adapter: exact no-op, denied change, stat/readback faults, identity and special bits PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';fault=Fault::None;target=0;fs::remove_all(root);return 1;}
}
