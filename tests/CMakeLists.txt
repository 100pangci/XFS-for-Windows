# ---------------------------------------------------------------------------
# xfswin tests: a small self-contained harness (no external dependencies).
# ---------------------------------------------------------------------------
add_executable(xfs_tests
  test_main.cpp
  support/fixture_image.cpp
  support/fixture_device.cpp
  unit/unit_tests.cpp
  unit/write_tests.cpp
  unit/journal_tests.cpp
  unit/rw2_tests.cpp
  unit/rw3_tests.cpp
  unit/rw4_tests.cpp
  unit/rw6_tests.cpp
  unit/rw6b_tests.cpp
  unit/rw6c_tests.cpp
  unit/rw6d_tests.cpp
  unit/rw6e_tests.cpp
  unit/perf_tests.cpp
  unit/conc_tests.cpp
  unit/recovery_tests.cpp
  integration/formatter_tests.cpp
  integration/corruption_tests.cpp
)

find_package(Threads REQUIRED)

target_link_libraries(xfs_tests PRIVATE xfs_core Threads::Threads)
target_include_directories(xfs_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(MSVC)
  target_compile_options(xfs_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(xfs_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# PERF-1 core benchmark runner (no WinFsp driver required).
# ---------------------------------------------------------------------------
add_executable(xfswin_perf
  perf/perf_main.cpp
)
target_link_libraries(xfswin_perf PRIVATE xfs_core Threads::Threads)
target_include_directories(xfswin_perf PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
  target_compile_options(xfswin_perf PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(xfswin_perf PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(xfswin_perf_ab
  perf/ab_benchmark.cpp
)
target_link_libraries(xfswin_perf_ab PRIVATE xfs_core Threads::Threads)
target_compile_definitions(xfswin_perf_ab PRIVATE XFSWIN_HAVE_DEFERRED_TIMESTAMPS=1)
target_include_directories(xfswin_perf_ab PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
  target_compile_options(xfswin_perf_ab PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(xfswin_perf_ab PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_test(NAME xfs_tests COMMAND xfs_tests)
set_tests_properties(xfs_tests PROPERTIES TIMEOUT 600)

# ---------------------------------------------------------------------------
# FUZZ-2: model-based stateful mutation fuzzing.  These are bounded smoke
# tests; the CI jobs run the same binary over more seeds and operations.
# ---------------------------------------------------------------------------
add_executable(stateful_fuzz
  stateful/stateful_fuzz.cpp
  stateful/model.cpp
  stateful/generator.cpp
  stateful/verifier.cpp
  stateful/runner.cpp
)
target_link_libraries(stateful_fuzz PRIVATE xfs_core Threads::Threads)
target_include_directories(stateful_fuzz PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
  target_compile_options(stateful_fuzz PRIVATE
    /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(stateful_fuzz PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_test(NAME stateful_fuzz_smoke
  COMMAND stateful_fuzz
    --seed 0x73a8175d0c41a9e3
    --steps 120
    --image "${CMAKE_CURRENT_BINARY_DIR}/stateful-smoke.img"
    --trace "${CMAKE_CURRENT_BINARY_DIR}/stateful-smoke.trace.txt"
    --manifest "${CMAKE_CURRENT_BINARY_DIR}/stateful-smoke.manifest.json"
    --profile modern --workload mixed
    --check-every 20 --consistency-every 40 --quiet)
set_tests_properties(stateful_fuzz_smoke PROPERTIES TIMEOUT 900)

add_test(NAME stateful_fuzz_enospc
  COMMAND stateful_fuzz
    --seed 0xabcdefabcdefabcd
    --steps 400
    --image "${CMAKE_CURRENT_BINARY_DIR}/stateful-enospc.img"
    --trace "${CMAKE_CURRENT_BINARY_DIR}/stateful-enospc.trace.txt"
    --manifest "${CMAKE_CURRENT_BINARY_DIR}/stateful-enospc.manifest.json"
    --profile modern --workload enospc --image-size-mb 300
    --require-enospc --check-every 100 --consistency-every 0 --quiet)
set_tests_properties(stateful_fuzz_enospc PROPERTIES TIMEOUT 900)

add_test(NAME stateful_fuzz_xattr_regression
  COMMAND stateful_fuzz
    --seed 0x4444444444444444
    --steps 400
    --image "${CMAKE_CURRENT_BINARY_DIR}/stateful-xattr.img"
    --trace "${CMAKE_CURRENT_BINARY_DIR}/stateful-xattr.trace.txt"
    --manifest "${CMAKE_CURRENT_BINARY_DIR}/stateful-xattr.manifest.json"
    --profile modern --workload xattr
    --check-every 100 --consistency-every 0 --quiet)
set_tests_properties(stateful_fuzz_xattr_regression PROPERTIES TIMEOUT 600)
