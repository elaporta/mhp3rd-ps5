// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_session.hpp"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    using yakumo::game_session::timeout;
    using yakumo::game_session::qa_seconds;
    check(qa_seconds("0")==0&&qa_seconds("10")==10&&qa_seconds("4200")==4200,"QA boundaries");
    for(auto text:{"", "-1", "+10", "9", "4201", "30x", "99999999999999999999"}) {
        bool refused=false;try{qa_seconds(text);}catch(const std::exception&){refused=true;}
        check(refused,"invalid QA duration rejected");
    }
    check(!timeout(1000000000,999999999,0,0),"normal play has no absolute deadline");
    check(timeout(1000000000,999969999,0,0),"unlimited play retains stall protection");
    check(yakumo::game_session::successful_stop("guest called sceKernelExitGame",false,false),"guest exit is normal");
    check(!yakumo::game_session::successful_stop("invalid instruction",false,false),"guest faults fail");
    check(!yakumo::game_session::successful_stop("Bounded menu session completed (navigation unverified)",false,false),"unlimited play cannot expire");
    check(!timeout(100000,99999,600000,0),"active guest remains within session");
    check(timeout(100000,69999,600000,0),"stalled guest fails independent of deadline");
    check(!timeout(615000,614999,600000,0)&&timeout(615001,615000,600000,0),"guest grace boundary remains fifteen seconds");
    check(!timeout(690000,689999,600000,600001),"real cleanup progress survives old absolute guest deadline");
    check(timeout(650000,619999,600000,600001),"blocked cleanup still fails after thirty seconds");
    check(!timeout(720001,720000,600000,600001)&&timeout(720002,720001,600000,600001),"cleanup has a hard 120-second bound despite progress");
    std::cout<<"session watchdog: guest deadline, real cleanup progress, stall and hard bound PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
