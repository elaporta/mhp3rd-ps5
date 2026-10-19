set(YAKUMO_OVERLAY_FIXTURES "${YAKUMO_ROOT}/out/overlay-fixtures")
foreach(module a b)
    set(_prefix "synthetic_${module}")
    file(GLOB _units CONFIGURE_DEPENDS "${YAKUMO_OVERLAY_FIXTURES}/${module}/generated/generated_unit_*.cpp")
    if(NOT _units)
        message(FATAL_ERROR "Generate public synthetic overlay fixtures before configuring")
    endif()
    set(YAKUMO_OVERLAY_${module}_SOURCES ${_units}
        "${YAKUMO_OVERLAY_FIXTURES}/${module}/generated/${_prefix}_registry.cpp"
        "${YAKUMO_OVERLAY_FIXTURES}/${module}/module.cpp")
endforeach()
