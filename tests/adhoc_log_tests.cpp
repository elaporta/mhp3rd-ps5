// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_log.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace yakumo::adhoc_log;
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
struct Sink:std::stringbuf {
    int flushes{};
    std::atomic<bool> block{},entered{},release{};
    int sync() override { ++flushes; return 0; }
    std::streamsize xsputn(const char* data,std::streamsize size) override {
        if(block) { entered=true; while(!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        return std::stringbuf::xsputn(data,size);
    }
};
int main() { try {
    for(int i=0;i<1000;++i) enqueue("trace "+std::to_string(i),false);
    enqueue("priority error",true);
    check(stats().pending==65 && stats().suppressed==936,"trace queue is bounded and cannot evict priority errors");
    auto batch=take(1,1);
    check(batch.size()==2 && batch[0]=="priority error\n" && batch[1]=="trace 936\n","priority and newest bounded trace order");
    take(kCapacity,kCapacity);
    enqueue(std::string(9000,'x'),true);
    batch=take(0,1);
    check(batch.size()==1 && batch[0].size()==kMaxRecord+1 && batch[0].back()=='\n',"record length and newline bounded");
    for(int i=0;i<70;++i) enqueue("error "+std::to_string(i),true);
    batch=take(0,kCapacity);
    check(batch.size()==64 && batch.front()=="error 6\n" && batch.back()=="error 69\n","important overflow retains newest errors");
    std::thread producers[4];
    for(auto& worker:producers) worker=std::thread([]{for(int i=0;i<1000;++i) enqueue("parallel trace",false);});
    for(auto& worker:producers) worker.join();
    check(stats().pending==64,"concurrent producer memory stays bounded");
    take(kCapacity,kCapacity);
    Sink sink;
    struct Redirect { std::streambuf* old; ~Redirect(){std::cout.rdbuf(old);} } redirect{std::cout.rdbuf(&sink)};
    for(int i=0;i<40;++i) enqueue("sample",false);
    pump(0);
    check(stats().pending==24 && sink.flushes==0,"owner pump writes at most 16 trace lines without flushing");
    pump(999); check(stats().pending==24,"one-second pump budget");
    pump(1000); check(stats().pending==8,"next diagnostic budget");
    finish(); check(stats().pending==0 && sink.flushes==0,"finish drains bounded records without forced flush");
    enqueue("blocked sink",false); sink.block=true;
    std::thread writer([&]{pump(5000);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!sink.entered && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool entered=sink.entered;
    const auto start=std::chrono::steady_clock::now(); enqueue("producer during blocked sink",true);
    const bool fast=std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100);
    sink.release=true; writer.join();
    check(entered && fast,"a stalled output sink holds no producer queue lock");
    finish(); finish();
    std::cout.rdbuf(redirect.old);
    std::cout<<"bounded concurrent diagnostics, priority, cadence, truncation, no sync and slow-sink isolation PASS\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
