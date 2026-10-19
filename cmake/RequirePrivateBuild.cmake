# SPDX-License-Identifier: GPL-3.0-or-later
get_filename_component(_install_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(REAL_PATH "${_install_root}/out/data" _private_data)
file(REAL_PATH "${CMAKE_BINARY_DIR}" _private_build)
string(FIND "${_private_build}/" "${_private_data}/" _private_prefix)
if(NOT _private_prefix EQUAL 0 OR _private_build STREQUAL _private_data)
    message(FATAL_ERROR "Configure component builds in a child directory of out/data")
endif()
