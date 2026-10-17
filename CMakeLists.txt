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
