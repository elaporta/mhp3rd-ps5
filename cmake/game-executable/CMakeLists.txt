# SPDX-License-Identifier: GPL-3.0-or-later
# Host-only preparation tool; no game input is compiled into this executable.
cmake_minimum_required(VERSION 3.22)
project(MHP3rdExecutablePreparation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ROOT}/cmake/RequirePrivateBuild.cmake")
set(UP "${ROOT}/upstream/Yakumo")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${ROOT}/tools/check_dependency.py" yakumo
    COMMAND_ERROR_IS_FATAL ANY)
set_source_files_properties("${UP}/profiles/mhp3rd/third_party/tiny_aes/aes.c" PROPERTIES LANGUAGE CXX)
add_executable(prepare_game_executable "${ROOT}/tools/prepare_game_executable.cpp"
    "${UP}/profiles/mhp3rd/host/install/executable_preparation.cpp"
    "${UP}/profiles/mhp3rd/third_party/tiny_aes/aes.c"
    "${UP}/src/elf32.cpp" "${UP}/src/sha256.cpp" "${UP}/src/guest_memory.cpp"
    "${UP}/src/runtime.cpp" "${UP}/src/decoder.cpp" "${UP}/src/interpreter.cpp"
    "${UP}/src/nid_registry.cpp" "${UP}/src/deflate.cpp")
target_link_libraries(prepare_game_executable PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(prepare_game_executable PRIVATE "${UP}/include"
    "${UP}/profiles/mhp3rd/host" "${UP}/profiles/mhp3rd/third_party")
