// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the pinned SDK C++ destructor backend and compiler-rt emulated TLS.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>

extern "C" int ps5___cxa_thread_atexit_impl(void(*)(void*),void*,void*);
extern "C" int __cxa_thread_atexit(void(*dtor)(void*),void* object,void* dso){
    return ps5___cxa_thread_atexit_impl(dtor,object,dso);
}
namespace {
struct Block {void* address;std::size_t bytes;};
Block blocks[4096]{};
pthread_mutex_t allocation_lock=PTHREAD_MUTEX_INITIALIZER;
std::atomic<unsigned> lifetime_errors{},destroyed{},freed_payloads{};
bool alive(const void* p,std::size_t bytes){
    const auto address=reinterpret_cast<std::uintptr_t>(p);
    bool result=false;pthread_mutex_lock(&allocation_lock);
    for(const auto& block:blocks){
        const auto base=reinterpret_cast<std::uintptr_t>(block.address);
        if(block.address&&address>=base&&bytes<=block.bytes&&address-base<=block.bytes-bytes)result=true;
    }
    pthread_mutex_unlock(&allocation_lock);return result;
}
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void __real_free(void*);
extern "C" void* __wrap_malloc(std::size_t bytes){
    void* p=__real_malloc(bytes);if(!p)return nullptr;
    pthread_mutex_lock(&allocation_lock);
    for(auto& block:blocks)if(!block.address){block={p,bytes};pthread_mutex_unlock(&allocation_lock);return p;}
    std::abort();
}
extern "C" void __wrap_free(void* p){
    pthread_mutex_lock(&allocation_lock);
    for(auto& block:blocks)if(block.address==p){block={};break;}
    pthread_mutex_unlock(&allocation_lock);__real_free(p);
}
namespace {
struct Owned {
    char* payload=static_cast<char*>(malloc(512));
    ~Owned(){
        ++destroyed;
        // Detect the broken order before dereferencing released TLS storage.
        if(!alive(this,sizeof(*this))){++lifetime_errors;return;}
        if(!alive(payload,512)){++lifetime_errors;return;}
        free(payload);++freed_payloads;
    }
};
thread_local Owned owned;
void* worker(void*){owned.payload[0]=42;return nullptr;}
}
int main(){
    for(unsigned run=0;run<8;++run){
        pthread_t thread{};
        if(pthread_create(&thread,nullptr,worker,nullptr)||pthread_join(thread,nullptr))return 2;
    }
    unsigned remaining=0;for(const auto& block:blocks)remaining+=block.address!=nullptr;
    std::printf("destroyed=%u payloads_freed=%u tls_lifetime_errors=%u remaining_allocations=%u\n",destroyed.load(),freed_payloads.load(),lifetime_errors.load(),remaining);
    return destroyed==8&&freed_payloads==8&&lifetime_errors==0&&remaining==0?0:1;
}
