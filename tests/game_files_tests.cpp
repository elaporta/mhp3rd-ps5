// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_files.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
namespace safe=yakumo::game_files;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){
    char temp[]="/tmp/yakumo-game-files-XXXXXX";const char* name=mkdtemp(temp);if(!name)return 1;
    fs::path root=name;
    try{
        for(const auto* folder:{"config","saves","cache","logs","game","assets"})fs::create_directories(root/"yakumo"/folder);
        safe::set_root(root);
        const mode_t old_mask=umask(0077);
        const auto legacy=root/"yakumo/cache/legacy";
        fs::create_directories(legacy);
        {std::ofstream out(legacy/"old.log");out<<"preserved";}
        safe::repair_generated_permissions();
        struct stat permissions{};
        check(::stat(legacy.c_str(),&permissions)==0&&(permissions.st_mode&0777)==0777,"legacy directory FTP traversal");
        check(::stat((legacy/"old.log").c_str(),&permissions)==0&&(permissions.st_mode&0777)==0666,"legacy cache FTP readability");
        check(safe::file_size(legacy/"old.log")==9,"permission repair preserves bytes");
        const auto immutable=root/"yakumo/game/untouched";
        {std::ofstream out(immutable);out<<"reference";}
        check(::chmod(immutable.c_str(),0400)==0,"immutable permission fixture");
        fs::create_symlink(immutable,root/"yakumo/config/forbidden-mode-link");
        bool link_rejected=false;try{safe::repair_generated_permissions();}catch(const fs::filesystem_error&){link_rejected=true;}
        check(link_rejected,"repair cannot follow link to immutable resource");
        check(::stat(immutable.c_str(),&permissions)==0&&(permissions.st_mode&0777)==0400,"immutable target permissions preserved");
        fs::remove(root/"yakumo/config/forbidden-mode-link");
        safe::create_directories(root/"yakumo/cache/new/nested");
        check(::stat((root/"yakumo/cache/new/nested").c_str(),&permissions)==0&&(permissions.st_mode&0777)==0777,"new directories ignore restrictive mask safely");
        const auto file=root/"yakumo/config/settings.ini";
        {safe::ofstream out(file);out<<"abcdef";out.close();check(bool(out),"contained stream write");}
        check(::stat(file.c_str(),&permissions)==0&&(permissions.st_mode&0777)==0666,"new file shared read/write without executable bits");
        check(::chmod(file.c_str(),0600)==0,"restrict existing log mode fixture");
        {FILE* log=safe::fopen(file,"ab");check(log,"reopen legacy log");std::fclose(log);}
        check(::stat(file.c_str(),&permissions)==0&&(permissions.st_mode&0777)==0666,"append repairs existing file mode");
        {safe::fstream io(file,std::ios::in|std::ios::out|std::ios::binary);char a[3]{};io.read(a,2);check(std::string(a)=="ab","buffered read");io.seekp(2);io<<"XY";io.flush();io.seekg(0);std::string text;io>>text;check(text=="abXYef","read/write seek preserves descriptor cursor");}
        {safe::ifstream in(file,std::ios::ate);check(in.tellg()==6,"ate seek");in.seekg(4);std::string text;in>>text;check(text=="ef","tail read");}
        std::error_code ec;
        check(!safe::exists(root/"yakumo/config/missing",ec)&&!ec,"ENOENT semantics");
        check(!safe::create_directories(root/"yakumo/game/new",ec)&&ec.value()==EROFS,"game data read-only");
        check(safe::open_file(root/"yakumo/game/../config/settings.ini",O_RDONLY)<0,"traversal rejected");
        check(safe::open_file(root/"yakumo/config/../../outside",O_WRONLY|O_CREAT)<0,"write escape rejected");
        fs::create_symlink(file,root/"yakumo/config/link");
        bool rejected=false;try{safe::repair_generated_permissions();}catch(const fs::filesystem_error&){rejected=true;}
        check(rejected,"permission repair rejects leaf symlink");
        check(::stat(file.c_str(),&permissions)==0&&(permissions.st_mode&0777)==0666,"permission repair never chmods symlink target");
        check(safe::open_file(root/"yakumo/config/link",O_WRONLY|O_TRUNC)<0,"leaf symlink rejected before truncation");
        check(safe::file_size(file)==6,"symlink rejection preserves target");
        fs::create_directory_symlink(root/"yakumo/config",root/"yakumo/cache/alias");
        check(safe::open_file(root/"yakumo/cache/alias/settings.ini",O_RDONLY)<0,"parent symlink rejected");
        const auto renamed=root/"yakumo/config/renamed";safe::rename(file,renamed,ec);check(!ec&&safe::file_size(renamed)==6,"contained rename");
        safe::rename(renamed,root/"yakumo/config/link",ec);check(bool(ec)&&safe::exists(renamed),"rename rejects symlink destination");
        safe::create_directories(root/"yakumo/saves/ms0/PSP/SAVEDATA/test",ec);check(!ec,"memory stick parents");
        {safe::ofstream out(root/"yakumo/saves/ms0/PSP/SAVEDATA/test/data");out<<"save";}
        check(safe::remove_all(root/"yakumo/saves/ms0/PSP/SAVEDATA/test",ec)==2&&!ec,"save subtree removal");
        check(!safe::remove(root/"yakumo/game",ec)&&ec.value()==EROFS,"immutable game preserved");
        check(safe::open_file(root/"yakumo/config",O_RDONLY)<0,"directory cannot be used as regular stream");
        std::cout<<"game_files: descriptor streams, traversal, links, containment, rename and memory stick PASS\n";
        umask(old_mask);fs::remove_all(root);return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';fs::remove_all(root);return 1;}
}
