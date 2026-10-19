// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_camera_setup.hpp"
#include "psprecomp/elf32.hpp"
#include "game_files.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() { try {
    std::ifstream file(YAKUMO_TEST_EBOOT, std::ios::binary);
    if (!file) throw std::runtime_error("private camera fixture unavailable");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    const auto elf = psprecomp::Elf32Image::from_bytes(std::move(bytes));
    psprecomp::Runtime runtime(elf.required_ram_size());
    const auto loaded = elf.load_and_relocate(runtime.memory());
    if (loaded.unsupported || loaded.invalid) throw std::runtime_error("invalid private executable");
    psprecomp::register_generated_functions(runtime);
    if (!runtime.has_function(0x08878B70u) || !yakumo::prepare_native_camera(runtime)) throw std::runtime_error("real camera/aspect signatures or original registration failed");
    runtime.memory().store32(0x08878B70u, 0);
    if (yakumo::prepare_native_camera(runtime)) throw std::runtime_error("invalid camera signature accepted");
    std::cout << "actual executable/AOT camera and aspect preparation; invalid signature rejection PASS\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
