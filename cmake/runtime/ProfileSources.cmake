# Original pinned portable profile closure; no game, SDL or FFmpeg target.
set(YAKUMO_PROFILE_ROOT "${YAKUMO_ROOT}/upstream/Yakumo/profiles/mhp3rd")
set(YAKUMO_PROFILE_SOURCES)
foreach(source hle/hle_common gpu/ge_state gpu/texture_decode perf/frame_stats
    save_data/param_sfo save_data/aes128 save_data/savedata_crypto kernel/iso_image platform/utf8_path)
    list(APPEND YAKUMO_PROFILE_SOURCES "${YAKUMO_PROFILE_ROOT}/host/${source}.cpp")
endforeach()
set(YAKUMO_TINY_AES "${YAKUMO_PROFILE_ROOT}/third_party/tiny_aes/aes.c")
# Compile the unchanged, C++ compatible vendored implementation with the fixed
# cross C++ driver rather than accidentally selecting the host C compiler.
set_source_files_properties("${YAKUMO_TINY_AES}" PROPERTIES LANGUAGE CXX)
list(APPEND YAKUMO_PROFILE_SOURCES "${YAKUMO_TINY_AES}")
