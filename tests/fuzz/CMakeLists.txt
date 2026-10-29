# libFuzzer parser harnesses. They depend only on xfs_core (no WinFsp, no CLI)
# and are built in a separate Clang configuration:
#   cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
#     -DXFSWIN_BUILD_FUZZERS=ON -DXFSWIN_BUILD_TESTS=OFF
set(XFSWIN_FUZZ_TARGETS
  fuzz_superblock
  fuzz_ag
  fuzz_inode
  fuzz_btree
  fuzz_directory
  fuzz_xattr
  fuzz_log_item)

foreach(target IN LISTS XFSWIN_FUZZ_TARGETS)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE xfs_core)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
