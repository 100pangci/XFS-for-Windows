# ---------------------------------------------------------------------------
# xfswin tests: a small self-contained harness (no external dependencies).
# ---------------------------------------------------------------------------
add_executable(xfs_tests
  test_main.cpp
  unit/unit_tests.cpp
  integration/formatter_tests.cpp
  integration/corruption_tests.cpp
)

target_link_libraries(xfs_tests PRIVATE xfs_core)
target_include_directories(xfs_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(MSVC)
  target_compile_options(xfs_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(xfs_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_test(NAME xfs_tests COMMAND xfs_tests)
set_tests_properties(xfs_tests PROPERTIES TIMEOUT 600)
