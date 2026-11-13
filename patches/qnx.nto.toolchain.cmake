# qnx.nto.toolchain.cmake — CMake toolchain file for QNX Neutrino 8.0 (x86_64)
#
# SPDX-License-Identifier: MIT
#
# Copyright (c) 2025 xtang2010        (original file, llama.cpp-qnx)
# Copyright (c) 2026 Tradoooo        (rewritten and corrected for Cadence)
#
# This file is a derivative work of:
#     https://github.com/xtang2010/llama.cpp-qnx/blob/master/qnx.nto.toolchain.cmake
# which is distributed under the MIT License:
#
#     MIT License
#
#     Copyright (c) 2025 xtang2010
#
#     Permission is hereby granted, free of charge, to any person obtaining a copy
#     of this software and associated documentation files (the "Software"), to deal
#     in the Software without restriction, including without limitation the rights
#     to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#     copies of the Software, and to permit persons to whom the Software is
#     furnished to do so, subject to the following conditions:
#
#     The above copyright notice and this permission notice shall be included in
#     all copies or substantial portions of the Software.
#
#     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#     IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#     FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#     THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#     LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#     FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
#     DEALINGS IN THE SOFTWARE.
#
# Changes made for Cadence:
#   * CPU variant is configurable (QNX_CPUVARDIR), default x86_64.
#   * Fixed the upstream typo ${QNX+TARGET} in CMAKE_LIBRARY_PATH.
#   * The C++ driver is q++ (not qcc) so the C++ standard library is linked.
#   * -Vgcc_nto<arch> is selected explicitly so the compiler variant matches the
#     target triple; GCC_VER can override it (e.g. GCC_VER=12.2.0).
#   * CMAKE_FIND_ROOT_PATH_MODE_PROGRAM stays NEVER so host tools (git, python)
#     remain reachable from the build.

if("$ENV{QNX_HOST}" STREQUAL "")
    message(FATAL_ERROR "QNX_HOST is not set. Source the QNX SDP environment first:\n"
                        "    . /path/to/qnxsdp-env.sh")
endif()
if("$ENV{QNX_TARGET}" STREQUAL "")
    message(FATAL_ERROR "QNX_TARGET is not set. Source the QNX SDP environment first:\n"
                        "    . /path/to/qnxsdp-env.sh")
endif()

set(QNX_HOST "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")
set(HOST_EXECUTABLE_SUFFIX "")

# Target variant directory. Override with -DQNX_CPUVARDIR=aarch64le for ARM.
set(CPUVARDIR "$ENV{QNX_CPUVARDIR}")
if(CPUVARDIR STREQUAL "")
    set(CPUVARDIR "x86_64")
endif()
set(CPU "${CPUVARDIR}")
set(CMAKE_SYSTEM_PROCESSOR "${CPUVARDIR}")

message(STATUS "QNX toolchain: QNX_HOST=${QNX_HOST}")
message(STATUS "QNX toolchain: QNX_TARGET=${QNX_TARGET}")
message(STATUS "QNX toolchain: CPU variant=${CPUVARDIR}")

set(QNX TRUE)
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_C_COMPILER   "${QNX_HOST}/usr/bin/qcc")
set(CMAKE_CXX_COMPILER "${QNX_HOST}/usr/bin/q++")
set(CMAKE_AR      "${QNX_HOST}/usr/bin/nto${CPU}-ar"      CACHE PATH "archiver")
set(CMAKE_RANLIB  "${QNX_HOST}/usr/bin/nto${CPU}-ranlib"  CACHE PATH "ranlib")

# Compiler variant: "-Vgcc_nto<arch>". GCC_VER (e.g. 12.2.0) pins a release.
if("${GCC_VER}" STREQUAL "")
    set(GCC_VERSION "" CACHE STRING "gcc_ver")
else()
    set(GCC_VERSION "${GCC_VER}," CACHE STRING "gcc_ver")
endif()
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -V${GCC_VERSION}gcc_nto${CMAKE_SYSTEM_PROCESSOR} ${EXTRA_CMAKE_C_FLAGS}"   CACHE STRING "c_flags")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -V${GCC_VERSION}gcc_nto${CMAKE_SYSTEM_PROCESSOR} ${EXTRA_CMAKE_CXX_FLAGS}" CACHE STRING "cxx_flags")

set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${EXTRA_CMAKE_LINKER_FLAGS}"   CACHE STRING "exe_linker_flags")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${EXTRA_CMAKE_LINKER_FLAGS}" CACHE STRING "so_linker_flags")

list(APPEND CMAKE_FIND_ROOT_PATH "${QNX_TARGET}" "${QNX_TARGET}/${CPUVARDIR}")
list(APPEND CMAKE_LIBRARY_PATH "${QNX_TARGET}/${CPUVARDIR}/lib" "${QNX_TARGET}/${CPUVARDIR}/usr/lib")

set(CMAKE_SKIP_RPATH TRUE CACHE BOOL "Do not add runtime paths to shared libraries.")
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# q++ injects "-lang-c++ " in the compile/link rules; CMake parses it as a
# language flag and misplaces it. Strip it from both rules.
string(REPLACE "-lang-c++ " " " CMAKE_CXX_COMPILE_OBJECT   "${CMAKE_CXX_COMPILE_OBJECT}")
string(REPLACE "-lang-c++ " " " CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
set(CMAKE_CXX_COMPILE_OBJECT   "${CMAKE_CXX_COMPILE_OBJECT}"   CACHE INTERNAL "" FORCE)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}" CACHE INTERNAL "" FORCE)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED TRUE)
