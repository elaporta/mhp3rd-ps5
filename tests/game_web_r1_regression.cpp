// SPDX-License-Identifier: GPL-3.0-or-later
// Run the actual frozen startup in an isolated child with absent native GOT
// entries. The parent accepts only SIGSEGV; unrelated failures do not pass.
#include "game_web.hpp"
#include "game_files.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
int main(){try{
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("yakumo-web-r1-regression-"+std::to_string(getpid()));
    fs::create_directories(root/"yakumo/assets/web");
    for(const char* name:{"index.html","menu.css","menu.js"})fs::copy_file(fs::path(WEB_ASSETS)/name,root/"yakumo/assets/web"/name);
    yakumo::game_files::set_root(root);
    const auto child=fork();
    if(child<0)throw std::runtime_error("Cannot create isolated regression child");
    if(child==0){rlimit limit{0,0};setrlimit(RLIMIT_CORE,&limit);yakumo::game_web::start();_exit(42);}
    int status{};
    if(waitpid(child,&status,0)!=child)throw std::runtime_error("Cannot reap regression child");
    fs::remove_all(root);
    if(!WIFSIGNALED(status)||WTERMSIG(status)!=SIGSEGV)throw std::runtime_error("Frozen r1 did not reproduce the expected null-import SIGSEGV");
    std::cout<<"Frozen r1 startup reproduces SIGSEGV with absent native import; core disabled and child reaped\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
