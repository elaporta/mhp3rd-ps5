// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_log.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
unsigned notices{};
void notice(const char*){++notices;}
int main(){
    for(bool synchronous:{true,false}) {
        FILE* output=std::tmpfile();if(!output)throw std::runtime_error("tmpfile");
        const int duplicate=dup(fileno(output));
        const auto start=std::chrono::steady_clock::now();
        {
            yakumo::GameLog log(output,nullptr,synchronous,notice);
            std::ostream stream(&log);
            for(unsigned i=0;i<10000;++i)stream<<"[fixture] complete diagnostic record "<<i<<'\n';
            stream<<"[fixture] FAIL flush-now\n";
            if(lseek(duplicate,0,SEEK_END)<=0)throw std::runtime_error("failure did not flush");
            stream<<"trailing partial record";
            const auto micros=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"log_benchmark synchronous="<<synchronous<<" producer_us="<<micros<<'\n';
        }
        lseek(duplicate,0,SEEK_SET);FILE* check=fdopen(duplicate,"r");
        char line[128];unsigned count=0;std::string last;
        while(std::fgets(line,sizeof(line),check)){++count;last=line;}
        std::fclose(check);
        if(count!=10002||last!="trailing partial record\n")throw std::runtime_error("log records lost or reordered");
    }
    if(notices!=10002+65)throw std::runtime_error("startup/error kernel logging policy");
}
