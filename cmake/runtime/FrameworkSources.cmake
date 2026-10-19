# The pinned framework's runtime closure; no profile, loader, analyzer or game.
set(YAKUMO_FRAMEWORK_SOURCES)
foreach(source guest_memory decoder interpreter nid_registry runtime)
    list(APPEND YAKUMO_FRAMEWORK_SOURCES "${YAKUMO_ROOT}/upstream/Yakumo/src/${source}.cpp")
endforeach()
