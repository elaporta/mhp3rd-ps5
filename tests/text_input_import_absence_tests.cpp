// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_input_ps5.hpp"
#include <iostream>
// Deliberately provide no IME symbols. The executable must start and the
// resolver's volatile reads must deliver Error instead of jumping to zero.
// Only this Linux fixture uses weak declarations to simulate empty GOT slots;
// it does not establish native loader behavior for global/loose imports.
static int hints{};
extern "C" int sceSysmoduleLoadModule(std::uint16_t){++hints;return hints==2?-63:0;}
int main(){
    using namespace yakumo::text_input;
    initialize_ps5(4);auto& s=service();
    for(int attempt=0;attempt<3;++attempt){
        const auto id=s.submit(Request{}).id;const auto result=s.take(id);
        if(!result||result->outcome!=Outcome::Error||!result->text.empty()||
           result->failure.operation!=(attempt==1?"sceSysmoduleLoadModule":"unbound-sceImeDialogInit"))return 1;
        s.suppress_sample(true);
    }
    std::cout<<"absent simulated GOT imports: startup, error, release and retry PASS\n";
}
