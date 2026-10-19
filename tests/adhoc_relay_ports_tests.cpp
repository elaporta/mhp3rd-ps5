// SPDX-License-Identifier: GPL-3.0-or-later
#include "adhoc_relay_ports.hpp"
#include <iostream>
#include <utility>
int main() {
    using namespace yakumo::adhoc_relay_ports;
    // Independent literal wire vectors, including PPSSPP's zero-wrap sentinel.
    for (const auto [guest, wire] : {std::pair{0,0}, {1,10001}, {10000,20000},
                                    {20001,30001}, {55535,65535}, {55536,65535},
                                    {55537,1}, {65535,9999}}) {
        if (to_wire(guest) != wire) return 1;
    }
    for (const auto [wire, guest] : {std::pair{20000,10000}, {30001,20001},
                                    {1,55537}, {9999,65535}, {65535,55535}}) {
        if (from_wire(wire) != guest) return 1;
    }
    std::cout << "PPSSPP default relay offset and 16-bit boundary vectors PASS\n";
}
