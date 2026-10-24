# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(MHP3rdSourceApplication LANGUAGES CXX)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ROOT}/cmake/RequirePrivateBuild.cmake")
set(YAKUMO_GAME_MENU ON CACHE BOOL "" FORCE)
set(YAKUMO_GAME_PERFORMANCE ON CACHE BOOL "" FORCE)
set(YAKUMO_GAME_BUILD "mhp3rd-ps5-source-0.12.0" CACHE STRING "Identified local source build")
set(YAKUMO_TITLE_ID "PPSA99373" CACHE STRING "" FORCE)
include("${ROOT}/cmake/web-settings/CMakeLists.txt")
if(CMAKE_CROSSCOMPILING)
    if(NOT MHP3RD_AOT_ARCHIVE)
        message(FATAL_ERROR "Select the locally generated main archive")
    endif()
    set(DRIVER "${ROOT}/deps/PS5_Vulkan")
    add_library(native_adapter_objects OBJECT "${ROOT}/src/title_paths.cpp" "${ROOT}/src/probe_log.cpp"
        "${ROOT}/src/native_lifecycle.cpp" "${ROOT}/src/native_optional_paths.cpp" "${DRIVER}/tooling/native/app_crt.cpp")
    target_include_directories(native_adapter_objects PRIVATE "${ROOT}/src")
    target_compile_definitions(native_adapter_objects PRIVATE YAKUMO_NATIVE=1)
    target_compile_options(native_adapter_objects PRIVATE -O2 -g -ffunction-sections -fdata-sections -fexceptions -frtti)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/link/eboot.bin"
        COMMAND bash "${ROOT}/tools/link_application.sh" "${ROOT}" "${CMAKE_CURRENT_BINARY_DIR}"
            "${MHP3RD_SDK_ROOT}" "${MHP3RD_RADV_ROOT}" "${MHP3RD_AOT_ARCHIVE}" "${MHP3RD_NATIVE_TOOL}"
            "$<TARGET_OBJECTS:first_frame_entry>" $<TARGET_OBJECTS:native_adapter_objects>
        DEPENDS first_frame_profile first_frame_framework first_frame_files first_frame_entry native_adapter_objects game_tls_runtime
            "${MHP3RD_AOT_ARCHIVE}" "${ROOT}/tools/link_application.sh"
            "${ROOT}/tools/native_weak_symbols.py" "${ROOT}/tools/native-symbols.map"
            "${MHP3RD_NATIVE_TOOL}" "${MHP3RD_RADV_ROOT}/lib/libvulkan_radeon.ps5.a"
            "${MHP3RD_SDK_ROOT}/target/lib/libc++.a" "${MHP3RD_SDK_ROOT}/target/lib/libc++abi.a"
            "${MHP3RD_SDK_ROOT}/target/lib/libunwind.a" "${MHP3RD_SDK_ROOT}/target/lib/libps5platform.a"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(native_application ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/link/eboot.bin")
endif()
