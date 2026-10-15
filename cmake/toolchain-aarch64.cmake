# ============================================================
# toolchain-aarch64.cmake — edge-sdk's aarch64-linux-gnu cross toolchain
#
# Usage (recommended via build.sh --cross, or directly):
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -B build -S .
#   cmake --build build -j
#   file build/dock_quickstart      # should show ELF ... ARM aarch64
#
# Dependencies: target OpenSSL/mosquitto .so files are bundled self-contained in third_party/aarch64-linux-gnu,
#       no target sysroot needed (CMakeLists auto-selects this triplet in cross mode).
#
# Defaults to the Ubuntu standard package gcc-aarch64-linux-gnu (sudo apt install gcc-aarch64-linux-gnu).
# Customize: cmake -DEDGE_SDK_AARCH64_COMPILER=/path/to/gcc \
#              -DEDGE_SDK_AARCH64_SYSROOT=/sysroot ...
# ============================================================

set(CMAKE_SYSTEM_NAME       Linux)
set(CMAKE_SYSTEM_PROCESSOR  aarch64)

# Compiler: user can override via -DEDGE_SDK_AARCH64_COMPILER
if(NOT DEFINED EDGE_SDK_AARCH64_COMPILER)
    # Prefer common versions (11 / 10 / 9); if none, the bare name
    foreach(_v IN ITEMS 11 10 9 "")
        if(_v)
            find_program(_cc NAMES aarch64-linux-gnu-gcc-${_v} NO_CACHE)
        else()
            find_program(_cc NAMES aarch64-linux-gnu-gcc NO_CACHE)
        endif()
        if(_cc)
            set(EDGE_SDK_AARCH64_COMPILER ${_cc})
            break()
        endif()
    endforeach()
endif()
if(NOT EDGE_SDK_AARCH64_COMPILER)
    message(FATAL_ERROR
        "aarch64-linux-gnu-gcc not found. Please install:\n"
        "  Ubuntu/Debian: sudo apt install gcc-aarch64-linux-gnu\n"
        "or specify explicitly: cmake -DEDGE_SDK_AARCH64_COMPILER=/path/to/cross-gcc")
endif()

set(CMAKE_C_COMPILER   ${EDGE_SDK_AARCH64_COMPILER})

# C++ compiler (optional; this SDK is C, kept to be compatible with occasional C++ dependencies)
get_filename_component(_cc_dir  ${EDGE_SDK_AARCH64_COMPILER} DIRECTORY)
get_filename_component(_cc_name ${EDGE_SDK_AARCH64_COMPILER} NAME)
string(REPLACE "gcc" "g++" _cxx_name ${_cc_name})
if(EXISTS ${_cc_dir}/${_cxx_name})
    set(CMAKE_CXX_COMPILER ${_cc_dir}/${_cxx_name})
endif()

# Sysroot (optional)
if(DEFINED EDGE_SDK_AARCH64_SYSROOT)
    set(CMAKE_SYSROOT ${EDGE_SDK_AARCH64_SYSROOT})
    set(CMAKE_FIND_ROOT_PATH ${EDGE_SDK_AARCH64_SYSROOT})
endif()

# find_xxx behavior: find libs/headers in the target sysroot, find programs (cmake/python/perl etc.) on the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
