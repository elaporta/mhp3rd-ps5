// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_worker.hpp"
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace yakumo::game_worker;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Api {
    bool live{};
    int init_error{},query_error{},size_error{},destroy_error{};
    std::size_t bytes=stack_bytes;
    std::string calls;
    int init(pthread_attr_t*){calls+='I';live=!init_error;return init_error;}
    int query(pthread_attr_t*){calls+='Q';return live?query_error:EINVAL;}
    int size(pthread_attr_t*,std::size_t* out){calls+='S';check(live,"size requires initialized attributes");if(!size_error)*out=bytes;return size_error;}
    int destroy(pthread_attr_t*){calls+='D';check(live,"destroy requires initialized attributes");live=false;return destroy_error;}
};
int main(){
    try{
        Api ok;check(inspect_stack(ok).valid()&&ok.calls=="IQSD"&&!ok.live,"initialized query and cleanup");
        Api init;init.init_error=ENOMEM;check(!inspect_stack(init).valid()&&init.calls=="I","init failure never queries or destroys");
        Api query;query.query_error=EINVAL;auto q=inspect_stack(query);check(!q.valid()&&q.query==22&&query.calls=="IQD","reported console error preserved and cleaned");
        Api size;size.size_error=EINVAL;check(!inspect_stack(size).valid()&&size.calls=="IQSD","size failure rejects");
        Api destroy;destroy.destroy_error=EINVAL;check(!inspect_stack(destroy).valid(),"destroy failure rejects");
        Api short_stack;short_stack.bytes=2u*1024*1024;check(!inspect_stack(short_stack).valid(),"default stack cannot replace requested 64 MiB");
        std::cout<<"game_worker: initialized SDK attributes, failures, cleanup and stack floor PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
