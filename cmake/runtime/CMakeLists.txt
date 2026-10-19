cmake_minimum_required(VERSION 3.20)
project(YakumoRuntimeSample LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../RequirePrivateBuild.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(YAKUMO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${CMAKE_CURRENT_SOURCE_DIR}/FrameworkSources.cmake")
add_library(yakumo_runtime_sample STATIC ${YAKUMO_FRAMEWORK_SOURCES}
    "${YAKUMO_ROOT}/src/runtime_probe.cpp" "${YAKUMO_ROOT}/src/runtime_services.cpp"
    "${YAKUMO_ROOT}/src/runtime_watchdog.cpp")
target_include_directories(yakumo_runtime_sample PRIVATE "${YAKUMO_ROOT}/src"
    "${YAKUMO_ROOT}/upstream/Yakumo/include")
target_compile_definitions(yakumo_runtime_sample PRIVATE YAKUMO_NATIVE=1)
target_compile_options(yakumo_runtime_sample PRIVATE -O2 -g -fexceptions -frtti
    -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic)
option(YAKUMO_WITH_OVERLAYS "Include public synthetic overlay corpora" OFF)
if(YAKUMO_WITH_OVERLAYS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/OverlaySources.cmake")
    target_sources(yakumo_runtime_sample PRIVATE "${YAKUMO_ROOT}/src/overlay_probe.cpp")
    target_include_directories(yakumo_runtime_sample PRIVATE "${YAKUMO_OVERLAY_FIXTURES}"
        "${YAKUMO_ROOT}/upstream/Yakumo/profiles/mhp3rd/host")
    target_compile_definitions(yakumo_runtime_sample PRIVATE YAKUMO_WITH_OVERLAYS=1)
    foreach(module a b)
        add_library(synthetic_${module} OBJECT ${YAKUMO_OVERLAY_${module}_SOURCES})
        target_include_directories(synthetic_${module} PRIVATE "${YAKUMO_ROOT}/upstream/Yakumo/include"
            "${YAKUMO_ROOT}/upstream/Yakumo/profiles/mhp3rd/host")
        target_compile_definitions(synthetic_${module} PRIVATE mhp3rd_overlay_info=synthetic_${module}_overlay_info
            mhp3rd_register_overlay=synthetic_${module}_register_overlay)
        target_compile_options(synthetic_${module} PRIVATE -O2 -g -fexceptions -frtti -ffunction-sections -fdata-sections)
        target_sources(yakumo_runtime_sample PRIVATE $<TARGET_OBJECTS:synthetic_${module}>)
    endforeach()
endif()
option(YAKUMO_WITH_INTEGRATION "Include combined public synthetic session" OFF)
if(YAKUMO_WITH_INTEGRATION)
    include("${CMAKE_CURRENT_SOURCE_DIR}/IntegrationSources.cmake")
    target_sources(yakumo_runtime_sample PRIVATE "${YAKUMO_ROOT}/src/integration.cpp"
        "${YAKUMO_ROOT}/src/integration_native.cpp" "${YAKUMO_ROOT}/src/integration_devices.cpp" ${YAKUMO_PROFILE_PORTABLE_SOURCES} ${YAKUMO_INTEGRATION_GENERATED_SOURCES})
    target_include_directories(yakumo_runtime_sample PRIVATE "${YAKUMO_INTEGRATION_FIXTURES}"
        "${YAKUMO_ROOT}/upstream/Yakumo/profiles/mhp3rd/host")
    target_compile_definitions(yakumo_runtime_sample PRIVATE YAKUMO_WITH_INTEGRATION=1
        YAKUMO_INTEGRATION_MODULE_COUNT=${YAKUMO_INTEGRATION_MODULE_COUNT})
endif()

option(YAKUMO_WITH_PROFILE "Include original portable profile probe" OFF)
if(YAKUMO_WITH_PROFILE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/ProfileSources.cmake")
    target_sources(yakumo_runtime_sample PRIVATE "${YAKUMO_ROOT}/src/profile_probe.cpp" "${YAKUMO_ROOT}/src/profile_native.cpp" ${YAKUMO_PROFILE_SOURCES})
    target_include_directories(yakumo_runtime_sample PRIVATE "${YAKUMO_PROFILE_ROOT}/third_party")
    target_compile_definitions(yakumo_runtime_sample PRIVATE YAKUMO_WITH_PROFILE=1)
endif()
