# Build the baseline-compatible PERF-1 A/B benchmark against an arbitrary
# XFS-for-Windows source tree. Used by tests/perf/run_ab.ps1:
#
#   cmake -S tests/perf/standalone -B build-ab-baseline \
#     -DXFS_SOURCE=<baseline source> \
#     -DBENCH_SOURCE=<this repository>/tests/perf/ab_benchmark.cpp
#
# The benchmark source uses only pre-PERF-1 public APIs, so the same file can
# be compiled against the baseline and the PERF-1 tree.
cmake_minimum_required(VERSION 3.24)
project(xfswin_perf_ab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT XFS_SOURCE)
  message(FATAL_ERROR "XFS_SOURCE must point at an XFS-for-Windows checkout")
endif()
if(NOT BENCH_SOURCE)
  message(FATAL_ERROR "BENCH_SOURCE must point at tests/perf/ab_benchmark.cpp")
endif()

set(XFSWIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XFSWIN_BUILD_WINFSP OFF CACHE BOOL "" FORCE)
add_subdirectory("${XFS_SOURCE}" xfs-build)

find_package(Threads REQUIRED)

add_executable(ab_benchmark "${BENCH_SOURCE}")
target_link_libraries(ab_benchmark PRIVATE xfs_core Threads::Threads)
if(MSVC)
  target_compile_options(ab_benchmark PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(ab_benchmark PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
