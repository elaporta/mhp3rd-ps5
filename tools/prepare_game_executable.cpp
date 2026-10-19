// SPDX-License-Identifier: GPL-3.0-or-later
// Local preparation of the user's verified executable; no embedded game bytes.
#include "install/executable_preparation.hpp"
#include "install/game_identity.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/sha256.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read encrypted executable");
        std::vector<std::uint8_t> encrypted{std::istreambuf_iterator<char>(in), {}};
        auto bytes = mhp3rd::install::prepare_executable(encrypted);
        const auto digest = psprecomp::sha256_bytes(bytes);
        if (digest != mhp3rd::install::kExecutableSha256)
            throw std::runtime_error("Decrypted executable identity mismatch");
        auto elf = psprecomp::Elf32Image::from_bytes(bytes, "verified local EBOOT");
        psprecomp::GuestMemory memory(elf.required_ram_size());
        const auto relocations = elf.load_and_relocate(memory);
        const auto module = elf.find_module_info(memory);
        if (!module || relocations.unsupported || relocations.invalid)
            throw std::runtime_error("Invalid module or relocation result");
        const int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (output < 0) throw std::runtime_error("Cannot exclusively create verified executable");
        std::size_t written{};
        bool complete = true;
        while (written < bytes.size()) {
            const auto count = write(output, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { complete = false; break; }
            written += static_cast<std::size_t>(count);
        }
        complete = fsync(output) == 0 && complete;
        complete = close(output) == 0 && complete;
        if (!complete) throw std::runtime_error("Cannot write/sync/close verified executable; preserve partial output");
        std::cout << "[game-prepare] sha256=" << digest << " bytes=" << bytes.size()
                  << " ram=" << memory.size() << " entry=" << std::hex << elf.runtime_entry()
                  << " gp=" << module->gp << std::dec << " relocations=" << relocations.total << '\n';
        for (const auto& import : elf.scan_imports(memory, *module))
            std::cout << "[game-import] " << import.library << " nid=" << std::hex << import.nid
                      << " stub=" << import.stub_address << std::dec << '\n';
    } catch (const std::exception& error) {
        std::cerr << "[game-prepare] FAIL " << error.what() << '\n';
        return 1;
    }
}
