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
option(XFSWIN_CONCURRENCY_HOOKS
       "Compile the deterministic concurrency scheduler hooks (CONC-1 tests)"
       OFF)
option(XFSWIN_TEST_HOOKS
       "Compile the one-shot failure-injection hooks used by the Windows UX integration tests"
       OFF)
option(XFSWIN_RELEASE_BUILD
       "Require a production Windows build with native WinFsp support and no test hooks"
       OFF)
option(XFSWIN_STATIC_RUNTIME
       "Link the MSVC runtime statically (no separate VC runtime deployment)"
       OFF)

if(MSVC AND XFSWIN_STATIC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(XFSWIN_RELEASE_BUILD)
  if(NOT WIN32 OR NOT MSVC OR NOT XFSWIN_STATIC_RUNTIME)
    message(FATAL_ERROR "Release packages require Windows, MSVC and XFSWIN_STATIC_RUNTIME=ON")
  endif()
  if(XFSWIN_TEST_HOOKS OR XFSWIN_CONCURRENCY_HOOKS OR XFSWIN_BUILD_FUZZERS)
    message(FATAL_ERROR "Release packages cannot contain test, scheduler or fuzz hooks")
  endif()
  if(NOT XFSWIN_BUILD_WINFSP)
    message(FATAL_ERROR "Release packages require WinFsp mount support")
  endif()
  if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "Production configuration" FORCE)
  elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Release packages require CMAKE_BUILD_TYPE=Release")
  endif()
  install(CODE [[
    if(NOT CMAKE_INSTALL_CONFIG_NAME STREQUAL "Release")
      message(FATAL_ERROR "Production payloads can only be installed from Release configuration")
    endif()
  ]])
endif()

# project(VERSION) above is the single version source for the CLI, PE resource,
# package names and installer. ARM64 denotes native ARM64, never ARM64EC/x64.
set(XFSWIN_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
  if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$" OR
     CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64" OR
     CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64" OR
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(XFSWIN_BUILD_ARCH "arm64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(XFSWIN_BUILD_ARCH "x64")
  else()
    set(XFSWIN_BUILD_ARCH "x86")
  endif()
  if(XFSWIN_RELEASE_BUILD)
    string(TOUPPER
      "${CMAKE_GENERATOR_PLATFORM};${CMAKE_VS_PLATFORM_NAME};${CMAKE_CXX_COMPILER_ARCHITECTURE_ID};${CMAKE_SYSTEM_PROCESSOR}"
      _xfswin_target_arch_signals)
    if(NOT XFSWIN_BUILD_ARCH MATCHES "^(x64|arm64)$" OR
       _xfswin_target_arch_signals MATCHES "ARM64EC|ARM64X")
      message(FATAL_ERROR "Release packages support only native x64 and ARM64")
    endif()
  endif()
  enable_language(RC)
endif()
find_package(Git QUIET)
set(XFSWIN_GIT_SHA "unknown")
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE XFSWIN_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _xfswin_git_result)
  if(NOT _xfswin_git_result EQUAL 0)
    set(XFSWIN_GIT_SHA "unknown")
  endif()
endif()
set(XFSWIN_SOURCE_TREE_DIRTY "true")
if(GIT_FOUND AND NOT XFSWIN_GIT_SHA STREQUAL "unknown")
  execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=all
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _xfswin_git_status OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _xfswin_status_result)
  if(_xfswin_status_result EQUAL 0 AND _xfswin_git_status STREQUAL "")
    set(XFSWIN_SOURCE_TREE_DIRTY "false")
  endif()
endif()
set(XFSWIN_PRODUCTION_BUILD "false")
set(XFSWIN_BUILD_CONFIGURATION "development")
if(CMAKE_BUILD_TYPE)
  set(XFSWIN_BUILD_CONFIGURATION "${CMAKE_BUILD_TYPE}")
endif()
if(XFSWIN_RELEASE_BUILD)
  if(XFSWIN_GIT_SHA STREQUAL "unknown" OR XFSWIN_SOURCE_TREE_DIRTY)
    message(FATAL_ERROR "Release packages require a clean committed source tree and a known Git SHA")
  endif()
  set(XFSWIN_PRODUCTION_BUILD "true")
  set(XFSWIN_BUILD_CONFIGURATION "Release")
  # Also check every build, not just configure: source may change after this
  # header/metadata was generated, or HEAD may move without touching CMake.
  add_custom_target(xfswin_release_source_check
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DEXPECTED_SHA=${XFSWIN_GIT_SHA}"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyReleaseSource.cmake"
    VERBATIM)
endif()
# CMake honors SOURCE_DATE_EPOCH here. This records provenance; it does not
# assert that MSVC/installer outputs are byte-for-byte reproducible.
string(TIMESTAMP XFSWIN_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(cmake/xfswin_version.h.in generated/xfswin_version.h @ONLY)
configure_file(cmake/version.json.in generated/version.json @ONLY)
if(WIN32)
  configure_file(cmake/xfswin.rc.in generated/xfswin.rc @ONLY)
endif()

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
  if(XFSWIN_BUILD_ARCH STREQUAL "arm64")
    set(XFSWIN_WINFSP_LIBRARY_NAME "winfsp-a64")
  elseif(XFSWIN_BUILD_ARCH STREQUAL "x64")
    set(XFSWIN_WINFSP_LIBRARY_NAME "winfsp-x64")
  else()
    set(XFSWIN_WINFSP_LIBRARY_NAME "winfsp-x86")
  endif()
  find_library(XFSWIN_WINFSP_LIBRARY
      NAMES "${XFSWIN_WINFSP_LIBRARY_NAME}"
      HINTS ${_winfsp_hints}
      PATH_SUFFIXES lib)

  if(XFSWIN_WINFSP_INCLUDE_DIR AND XFSWIN_WINFSP_LIBRARY)
    message(STATUS "WinFsp SDK found: ${XFSWIN_WINFSP_INCLUDE_DIR}")
  else()
    if(XFSWIN_RELEASE_BUILD)
      message(FATAL_ERROR
          "Release packages require the matching native WinFsp SDK; mount support cannot be omitted")
    endif()
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
