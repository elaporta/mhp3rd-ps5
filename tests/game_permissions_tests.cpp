// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_bootstrap.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
namespace fs=std::filesystem;
namespace safe=yakumo::game_files;
unsigned progress_events{};
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int other_user(const fs::path& root){
    const auto pid=fork();check(pid>=0,"fork permission client");
    if(!pid){
        if(setgid(1)||setuid(1))_exit(3);
        int fd=open((root/"eboot.bin").c_str(),O_RDWR);if(fd<0)_exit(2);close(fd);
        if(rename((root/"eboot.bin").c_str(),(root/"eboot.bin.old").c_str()))_exit(4);
        fd=open((root/"eboot.bin").c_str(),O_CREAT|O_EXCL|O_WRONLY,0666);if(fd<0)_exit(5);
        if(write(fd,"new",3)!=3)_exit(6);close(fd);
        if(unlink((root/"eboot.bin.old").c_str())||unlink((root/"yakumo/cache/owned/file").c_str())||rmdir((root/"yakumo/cache/owned").c_str()))_exit(7);
        _exit(0);
    }
    int status=0;check(waitpid(pid,&status,0)==pid&&WIFEXITED(status),"client exited");return WEXITSTATUS(status);
}
int loader_probe(const fs::path& root){
    std::cout.flush();
    const auto pid=fork();check(pid>=0,"fork loader probe");
    if(!pid){
        if(setgid(1)||setuid(1))_exit(3);
        execl((root/"eboot.bin").c_str(),"fixture",static_cast<char*>(nullptr));
        _exit(errno==EACCES?2:4);
    }
    int status=0;check(waitpid(pid,&status,0)==pid&&WIFEXITED(status),"loader probe exited");return WEXITSTATUS(status);
}
int foreign_owner_repair(bool expect_success){
    std::cout.flush(); // Do not duplicate parent evidence through child stderr ties.
    const auto pid=fork();check(pid>=0,"fork foreign-owner repair");
    if(!pid){
        if(setgid(1)||setuid(1))_exit(3);
        try{
            safe::repair_title_permissions();_exit(expect_success?0:4);
        }
        catch(const fs::filesystem_error& e){_exit(!expect_success&&e.code().value()==EPERM?0:5);}
    }
    int status=0;check(waitpid(pid,&status,0)==pid&&WIFEXITED(status),"repair client exited");return WEXITSTATUS(status);
}
int main(int argc,char**){
    if(argc>1&&geteuid()!=0){std::cerr<<"Cross-user acceptance requires root on Linux fixture only\n";return 1;}
    char temp[]="/tmp/yakumo-permissions-XXXXXX";const char* name=mkdtemp(temp);if(!name)return 1;
    const fs::path root=name;
    try{
        for(const auto* folder:{"config","saves","cache/owned","logs","game","assets"})fs::create_directories(root/"yakumo"/folder);
        {std::ofstream(root/"eboot.bin")<<"fixture";std::ofstream(root/"yakumo/game/game.iso")<<"owned fixture disc";std::ofstream(root/"yakumo/cache/owned/file")<<"cache";}
        safe::set_root(root);
        if(geteuid()==0)check(other_user(root)==2,"r1-style owner permissions deny another FTP identity");
        safe::repair_title_permissions([]{++progress_events;});
        check(progress_events>=10,"permission progress follows real validated objects");
        if(geteuid()==0){
            check(foreign_owner_repair(true)==0,"foreign owner with exact modes needs no chmod");
            {std::ofstream(root/"eboot.bin")<<"#!/bin/sh\nexit 0\n";}
            check(!chmod((root/"eboot.bin").c_str(),0666),"old data-only program mode");
            check(loader_probe(root)==2,"0666 executable rejected before program entry under UID 1");
            check(foreign_owner_repair(false)==0,"foreign application cannot restore missing execute bits");
            safe::repair_title_permissions();
            check(loader_probe(root)==0,"0777 native program can execute under another UID");
            std::cout<<"loader_0666_EACCES_0777_exec=PASS\n";
            check(chmod(root.c_str(),0755)==0,"foreign root needs real change");
            check(foreign_owner_repair(false)==0,"foreign owner required chmod returns EPERM");
            check(chmod(root.c_str(),0777)==0,"restore title root fixture");
            std::cout<<"foreign_owner_exact_noop_and_required_change_EPERM=PASS\n";
            fs::create_directories(root/"sce_module");
            const auto runtime=root/"sce_module/libc.prx";
            {std::ofstream(runtime)<<"runtime fixture bytes";}
            check(!chmod((root/"sce_module").c_str(),0777)&&!chmod(runtime.c_str(),0666),"console runtime mode fixture");
            check(foreign_owner_repair(false)==0,"root exact 0777 does not fix foreign runtime 0666 EPERM");
            // Installer/FTP owner changes metadata, never application content.
            const int fd=open(runtime.c_str(),O_RDONLY|O_NOFOLLOW);
            check(fd>=0&&!fchmod(fd,0777),"owner-side descriptor metadata repair");close(fd);
            check(foreign_owner_repair(true)==0,"UID 1 app accepts exact owner-prepared runtime without chmod");
            {std::ifstream in(runtime);std::string text((std::istreambuf_iterator<char>(in)),{});check(text=="runtime fixture bytes","owner-side repair preserves runtime bytes");}
            std::cout<<"foreign_runtime_0666_EPERM_owner_0777_repair=PASS\n";
        }
        for(const auto& item:fs::recursive_directory_iterator(root)){
            struct stat st{};check(stat(item.path().c_str(),&st)==0,"inspect repaired mode");
            check((st.st_mode&0777)==(S_ISDIR(st.st_mode)||item.path()==root/"eboot.bin"||item.path()==root/"sce_module/libc.prx"?0777:0666),"exact shared title modes");
        }
        check(safe::file_size(root/"yakumo/game/game.iso")==18,"repair preserves fixture data bytes");
        check(safe::open_file(root/"yakumo/game/game.iso",O_WRONLY|O_TRUNC)<0&&errno==EROFS,"runtime game content stays read-only");
        check(safe::open_file(root/"eboot.bin",O_WRONLY|O_TRUNC)<0&&errno==EROFS,"executable mode does not authorize runtime program writes");
        if(geteuid()==0){check(other_user(root)==0,"different UID can replace native program and delete app-owned cache");std::cout<<"cross_uid_replace_delete=PASS\n";}
        else std::cout<<"cross_uid_replace_delete=NOT_RUN (run same binary as root on Linux fixture)\n";
        const auto logfile=root/"yakumo/logs/game-first-frame.log";
        {std::ofstream(logfile)<<"old evidence\n";}
        FILE* log=yakumo::game_bootstrap::open_log();check(log,"bootstrap append opens before repair");
        fprintf(log,"bootstrap before failing permission repair\n");fflush(log);
        fs::create_symlink(root/"eboot.bin",root/"yakumo/config/blocked");
        bool rejected=false;try{safe::repair_title_permissions();}catch(const fs::filesystem_error& error){rejected=true;fprintf(log,"FAIL %s\n",error.what());fflush(log);}
        fclose(log);check(rejected,"whole title repair rejects symbolic links");
        {std::ifstream in(logfile);std::string text((std::istreambuf_iterator<char>(in)),{});check(text.starts_with("old evidence\nbootstrap")&&text.find("FAIL")!=text.npos,"early error retained without losing prior evidence");}
        fs::remove(root/"yakumo/config/blocked");fs::rename(logfile,root/"yakumo/logs/previous.log");
        fs::create_symlink(root/"eboot.bin",logfile);check(!yakumo::game_bootstrap::open_log(),"bootstrap refuses log symlink");fs::remove(logfile);
        const auto outside=root.parent_path()/(root.filename().string()+"-outside");
        {std::ofstream(outside)<<"reference";}check(chmod(outside.c_str(),0600)==0,"hardlink fixture");
        fs::create_hard_link(outside,root/"yakumo/cache/alias");
        check(safe::open_file(root/"yakumo/cache/alias",O_WRONLY|O_TRUNC)<0&&errno==EPERM,"hardlink content write rejected");
        check(chmod(outside.c_str(),0400)==0,"read-only external reference");
        rejected=false;try{safe::repair_title_permissions();}catch(const fs::filesystem_error&){rejected=true;}
        struct stat st{};check(rejected&&stat(outside.c_str(),&st)==0&&(st.st_mode&0777)==0400&&st.st_size==9,"hardlink external bytes and mode preserved");
        fs::remove(root/"yakumo/cache/alias");fs::remove(outside);
        safe::repair_title_permissions();fs::remove_all(root);
        std::cout<<"shared title modes, bootstrap evidence, containment and link rejection PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}
