# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
cmake_minimum_required(VERSION 3.24)
project(XFS-for-Windows VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(XFSWIN_BUILD_WINFSP "Build the WinFsp adapter (mount support)" ON)
option(XFSWIN_BUILD_TESTS "Build unit and integration tests" ON)
option(XFSWIN_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(XFSWIN_BUILD_FUZZERS "Build the libFuzzer parser harnesses (Clang only)" OFF)

# The canonical CI builds turn warnings into errors. MSVC applies /WX to
# xfs_core in src/CMakeLists.txt (the product library); GCC/Clang get the same
# policy for every target here so the Linux build gates hold the whole tree to
# the same standard.
if(XFSWIN_WARNINGS_AS_ERRORS AND NOT MSVC)
  add_compile_options(-Wall -Wextra -Werror)
endif()

# The fuzz harnesses must instrument the core library, not just the entry
# point. Apply the sanitizer flags to every target in this configuration; the
# fuzz executables add the libFuzzer main at link time.
if(XFSWIN_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "XFSWIN_BUILD_FUZZERS requires Clang (libFuzzer); configure with "
        "-DCMAKE_CXX_COMPILER=clang++ and a separate build directory.")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined)
  add_compile_options(-fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Locate the WinFsp SDK (headers + import library).
#
# Search order:
#   1. -DXFSWIN_WINFSP_ROOT=<dir>
#   2. $ENV{WINFSP_ROOT}
#   3. D:/SDK/WinFsp/winfsp-<version>/DYNAMIC   (per D:\SDK naming convention)
#   4. C:/Program Files (x86)/WinFsp            (normal WinFsp install)
# ---------------------------------------------------------------------------
if(XFSWIN_BUILD_WINFSP)
  set(_winfsp_hints
      "${XFSWIN_WINFSP_ROOT}"
      "$ENV{WINFSP_ROOT}"
      "C:/Program Files (x86)/WinFsp"
      "C:/Program Files/WinFsp")
  if(EXISTS "D:/SDK/WinFsp")
    file(GLOB _winfsp_sdk_dirs LIST_DIRECTORIES true "D:/SDK/WinFsp/winfsp-*")
    foreach(d ${_winfsp_sdk_dirs})
      if(EXISTS "${d}/DYNAMIC/inc/winfsp/winfsp.h")
        list(APPEND _winfsp_hints "${d}/DYNAMIC")
      endif()
    endforeach()
  endif()

  find_path(XFSWIN_WINFSP_INCLUDE_DIR
      NAMES winfsp/winfsp.h
      HINTS ${_winfsp_hints}
      PATH_SUFFIXES inc)
  find_library(XFSWIN_WINFSP_LIBRARY
      NAMES winfsp-x64
      HINTS ${_winfsp_hints}
      PATH_SUFFIXES lib)

  if(XFSWIN_WINFSP_INCLUDE_DIR AND XFSWIN_WINFSP_LIBRARY)
    message(STATUS "WinFsp SDK found: ${XFSWIN_WINFSP_INCLUDE_DIR}")
  else()
    message(WARNING
        "WinFsp SDK not found - building without mount support. "
        "Set -DXFSWIN_WINFSP_ROOT=<sdk dir containing inc/ and lib/>, "
        "or set the WINFSP_ROOT environment variable.")
    set(XFSWIN_BUILD_WINFSP OFF)
  endif()
endif()

add_subdirectory(src)

if(XFSWIN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(XFSWIN_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()
