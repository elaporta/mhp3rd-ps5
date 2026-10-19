// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "game_observer.hpp"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
namespace yakumo {
// Bounded stdio buffering: startup/errors flush immediately, routine records
// flush within one second. An opt-in synchronous mode supports matched A/B runs.
class GameLog final:public std::streambuf {
    FILE* file_{};FILE* console_{};
    void (*kernel_)(const char*){};
    bool synchronous_{};
    unsigned lines_{};
    std::mutex lock_;
    std::string line_;
    std::atomic<bool> stopping_{};
    std::thread flusher_;
    void flush(){if(console_)std::fflush(console_);if(file_)std::fflush(file_);}
    void emit(){
        if(line_.empty())return;
        game_observer::Timer timer{game_observer::log_us};++game_observer::log_lines;
        const bool immediate=synchronous_ || ++lines_<=64 || line_.find("FAIL")!=std::string::npos ||
            line_.find("[game-cleanup]")!=std::string::npos || line_.find("[game-watchdog]")!=std::string::npos;
        for(auto output:{console_,file_})if(output){std::fwrite(line_.data(),1,line_.size(),output);std::fputc('\n',output);}
        if(immediate){flush();if(kernel_)kernel_(line_.c_str());}
        line_.clear();
    }
    std::streamsize xsputn(const char* data,std::streamsize size)override{
        std::lock_guard<std::mutex> guard(lock_);
        for(std::streamsize i=0;i<size;++i){if(data[i]=='\n')emit();else{line_+=data[i];if(line_.size()>=2048)emit();}}
        return size;
    }
    int_type overflow(int_type value)override{
        if(traits_type::eq_int_type(value,traits_type::eof()))return traits_type::not_eof(value);
        const char c=traits_type::to_char_type(value);xsputn(&c,1);return value;
    }
    int sync()override{std::lock_guard<std::mutex> guard(lock_);flush();return 0;}
public:
    void set_synchronous(bool value){std::lock_guard<std::mutex> guard(lock_);synchronous_=value;}
    GameLog(FILE* file,FILE* console,bool synchronous,void (*kernel)(const char*)=nullptr)
        :file_(file),console_(console),kernel_(kernel),synchronous_(synchronous),flusher_([this]{
            unsigned ticks=0;
            while(!stopping_){
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if(++ticks==10){std::lock_guard<std::mutex> guard(lock_);flush();ticks=0;}
            }
        }){}
    ~GameLog()override{stopping_=true;if(flusher_.joinable())flusher_.join();emit();flush();if(file_)std::fclose(file_);}
};
}
