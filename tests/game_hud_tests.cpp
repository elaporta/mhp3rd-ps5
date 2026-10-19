// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only integration with the private verified executable and existing AOT.
#include "gpu/game_hud.hpp"
#include "game_settings.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc,char** argv){try{
    // Host-only read: no title root or game installation is created/modified.
    std::ifstream input(YAKUMO_TEST_EBOOT,std::ios::binary);
    if(!input)throw std::runtime_error("missing private HUD fixture");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    const auto elf=psprecomp::Elf32Image::from_bytes(std::move(bytes));
    psprecomp::Runtime runtime(elf.required_ram_size());
    const auto loaded=elf.load_and_relocate(runtime.memory());
    if(loaded.unsupported||loaded.invalid)throw std::runtime_error("invalid test executable");
    psprecomp::register_generated_functions(runtime);
    constexpr std::array<unsigned,15> addresses={0x08876874,0x088EA9B0,0x088EB07C,0x088EBF0C,0x088EC060,
        0x088EC0F8,0x088EC1F0,0x088EC2EC,0x088EC3E0,0x088EC63C,0x088ECB74,0x088ECC38,0x088ECCE4,0x088ECD9C,0x088ECE58};

    const auto check=[](bool ok){if(!ok)throw std::runtime_error("HUD hook integration");};
    for(const auto address:addresses)check(runtime.has_function(address));
    const auto count=runtime.function_count();
    const bool invalid=argc==2&&std::string_view(argv[1])=="invalid";
    if(invalid)runtime.memory().store32(0x08876874,0);
    namespace hud=mhp3rd::gpu::hud;
    hud::frame(runtime,1);check(!hud::active());hud::toggle();hud::frame(runtime,2);
    check(hud::available()==!invalid&&hud::active()==!invalid);
    if(invalid)check(!yakumo::game_settings::toggle_hud());
    check(runtime.function_count()==count); // Wrapping replaces all original registrations in place.
    check(!hud::hides(0,0)&&!hud::hides(0x08800000,0x08800004)); // Unclassified menu draws remain visible.
    hud::toggle();check(!hud::hidden()&&!hud::active());
    hud::toggle();hud::frame(runtime,3);check(hud::active()==!invalid);
    std::cout<<"all 15 HUD originals/hook installation, signature rejection and toggle round trip PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
