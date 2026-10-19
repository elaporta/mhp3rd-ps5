# SPDX-License-Identifier: GPL-3.0-or-later
# Separate static/PIC codec closure. The original desktop build stays unchanged.
include(ExternalProject)
set(_codec_source "${YAKUMO_ROOT}/out/data/dependencies/ffmpeg-7.1.5")
if(NOT EXISTS "${_codec_source}/configure")
    message(FATAL_ERROR "Run tools/prepare_game_codecs.py to obtain the locked public source")
endif()
set(_codec_prefix "${CMAKE_CURRENT_BINARY_DIR}/codecs/install")
set(YAKUMO_REUSE_CODECS "" CACHE PATH "Verified same-platform pinned codec install to reuse")
if(YAKUMO_REUSE_CODECS)
    set(_codec_prefix "${YAKUMO_REUSE_CODECS}")
    foreach(component lib/libavcodec.a lib/libavutil.a include/libavcodec/avcodec.h)
        if(NOT EXISTS "${_codec_prefix}/${component}")
            message(FATAL_ERROR "Incomplete pinned codec reuse directory")
        endif()
    endforeach()
endif()
file(MAKE_DIRECTORY "${_codec_prefix}/include")
find_program(_codec_make NAMES gmake make REQUIRED)
set(_codec_flags
    --enable-static --disable-shared --enable-pic --disable-programs --disable-doc
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale
    --disable-swresample --disable-network --disable-autodetect --disable-everything
    --enable-decoder=h264,atrac3,atrac3p --disable-asm --disable-debug
    --disable-pthreads --disable-w32threads --disable-os2threads --disable-symver
    --ar=llvm-ar-18 --ranlib=llvm-ranlib-18 --nm=llvm-nm-18)
if(CMAKE_CROSSCOMPILING)
    list(APPEND _codec_flags --enable-cross-compile --target-os=freebsd --arch=x86_64
        "--cc=${CMAKE_CXX_COMPILER}" "--ld=${CMAKE_CXX_COMPILER}"
        "--extra-ldflags=-L$ENV{PS5_PAYLOAD_SDK}/target/lib"
        "--extra-libs=-lSceLibcInternal -lkernel")
else()
    list(APPEND _codec_flags --cc=clang-18 --ld=clang-18)
endif()
if(NOT YAKUMO_REUSE_CODECS)
ExternalProject_Add(game_ffmpeg
    SOURCE_DIR "${_codec_source}"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/codecs/build"
    INSTALL_DIR "${_codec_prefix}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND bash "${_codec_source}/configure" "--prefix=<INSTALL_DIR>" ${_codec_flags}
    BUILD_COMMAND "${_codec_make}" -j2
    INSTALL_COMMAND "${_codec_make}" install
    BUILD_BYPRODUCTS "${_codec_prefix}/lib/libavcodec.a" "${_codec_prefix}/lib/libavutil.a"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)
endif()
add_library(game_avcodec STATIC IMPORTED GLOBAL)
add_library(game_avutil STATIC IMPORTED GLOBAL)
set_target_properties(game_avcodec PROPERTIES IMPORTED_LOCATION "${_codec_prefix}/lib/libavcodec.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_codec_prefix}/include")
set_target_properties(game_avutil PROPERTIES IMPORTED_LOCATION "${_codec_prefix}/lib/libavutil.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_codec_prefix}/include")
if(NOT YAKUMO_REUSE_CODECS)
    add_dependencies(game_avcodec game_ffmpeg)
    add_dependencies(game_avutil game_ffmpeg)
endif()
target_compile_definitions(first_frame_profile PUBLIC MHP3RD_HAS_FFMPEG=1)
target_link_libraries(first_frame_profile PUBLIC game_avcodec game_avutil)
if(NOT CMAKE_CROSSCOMPILING)
    target_link_libraries(first_frame_profile PUBLIC m)
endif()
