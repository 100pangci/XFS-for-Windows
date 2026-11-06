# ---------------------------------------------------------------------------
# xfs_core - platform independent XFS parser / formatter core.
#             No WinFsp, no Windows UI dependencies (Win32 only via the
#             block device backends).
# ---------------------------------------------------------------------------
add_library(xfs_core STATIC
  core/error.cpp
  core/log.cpp
  core/log_checker.cpp
  core/crash.cpp
  core/perf_stats.cpp
  core/endian.cpp
  core/crc32c.cpp
  core/verifier.cpp
  core/superblock.cpp
  core/ag.cpp
  core/btree.cpp
  core/rmap.cpp
  core/rmap_editor.cpp
  core/rmap_intent.cpp
  core/bmap_intent.cpp
  core/xattr_intent.cpp
  core/refcount.cpp
  core/refcount_editor.cpp
  core/refcount_intent.cpp
  core/inode.cpp
  core/xattr.cpp
  core/parent.cpp
  core/extent.cpp
  core/directory.cpp
  core/symlink.cpp
  core/filesystem.cpp
  core/mount.cpp
  core/cache.cpp
  core/check.cpp
  core/invalidation.cpp
  core/concurrency_hooks.cpp
  block/image_device.cpp
  block/device_policy.cpp
  block/partition_device.cpp
  block/drive_letter.cpp
  block/drive_letter_takeover.cpp
  block/windows_volume.cpp
  block/device_discovery.cpp
  block/windows_device_discovery.cpp
  block/device_selection.cpp
  block/windows_raw_device.cpp
  automount/automount_config.cpp
  automount/automount_engine.cpp
  mkfs/geometry.cpp
  mkfs/ag_builder.cpp
  mkfs/inode_builder.cpp
  mkfs/superblock_builder.cpp
  mkfs/journal_builder.cpp
  mkfs/formatter.cpp
  write/policy.cpp
  write/range.cpp
  write/file_data.cpp
  write/log_record.cpp
  write/transaction.cpp
  write/inode_write.cpp
  write/alloc/alloc_tree.cpp
  write/alloc/alloc_ag.cpp
  write/alloc/alloc_ownership.cpp
  write/alloc/alloc_core.cpp
  write/alloc/alloc.cpp
  write/bmap.cpp
  write/ialloc.cpp
  write/dir_write.cpp
  write/namespace/namespace_common.cpp
  write/namespace/namespace_create.cpp
  write/namespace/namespace_remove.cpp
  write/namespace/namespace_rename.cpp
  write/namespace/namespace_link.cpp
  write/namespace/namespace_symlink.cpp
  write/file_write.cpp
  write/xattr_write.cpp
  write/parent_write.cpp
  write/reflink.cpp
  write/flush.cpp
  recovery/log_scan.cpp
  recovery/replay.cpp
  write/recovery_host.cpp
  win_name_map.cpp
)

if(XFSWIN_RELEASE_BUILD)
  add_dependencies(xfs_core xfswin_release_source_check)
endif()

target_include_directories(xfs_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
          "${PROJECT_BINARY_DIR}/generated")

if(WIN32)
  target_sources(xfs_core PRIVATE deployment/deployment.cpp)
  # The deployment and service helpers are available even in WinFsp-OFF builds.
  target_link_libraries(xfs_core PRIVATE advapi32)
endif()

# CONC-1 deterministic scheduling hooks. Off by default: the production
# wrappers then reduce to direct std::mutex/std::shared_mutex calls and every
# XFS_CONC_SYNC compiles away. The dedicated CI/test configuration enables
# this for the whole xfs_tests target.
if(XFSWIN_CONCURRENCY_HOOKS)
  target_compile_definitions(xfs_core PUBLIC XFSWIN_CONCURRENCY_HOOKS=1)
endif()

if(MSVC)
  target_compile_options(xfs_core PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(XFSWIN_WARNINGS_AS_ERRORS)
    target_compile_options(xfs_core PRIVATE /WX)
  endif()
  target_compile_definitions(xfs_core PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# xfswin - command line tool: list / info / check / ls / cat / mount / format.
#
# The CLI and the WinFsp adapter are Windows-only product surfaces. The core,
# the formatter, the writer, the recovery engine and the tests are portable and
# are built on Linux for the GCC/Clang, sanitizer and fuzz jobs; the Linux
# build intentionally does not synthesize a second CLI.
# ---------------------------------------------------------------------------
if(WIN32)
  add_executable(xfswin
    cli/main.cpp
    cli/commands.cpp
    "${PROJECT_BINARY_DIR}/generated/xfswin.rc"
  )

  target_link_libraries(xfswin PRIVATE xfs_core)

  target_include_directories(xfswin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_BINARY_DIR}/generated")

  # Stage an explicit runtime payload. Never package the build directory or
  # copy WinFsp's shared runtime/driver into the product installation.
  install(TARGETS xfswin RUNTIME DESTINATION .)
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
                "${PROJECT_SOURCE_DIR}/README.md"
                "${PROJECT_BINARY_DIR}/generated/version.json"
          DESTINATION .)
  install(FILES
    "${PROJECT_SOURCE_DIR}/docs/INSTALL.md"
    "${PROJECT_SOURCE_DIR}/INSTALLER.md"
    "${PROJECT_SOURCE_DIR}/RELEASE.md"
    "${PROJECT_SOURCE_DIR}/docs/RW_STABILITY.md"
    "${PROJECT_SOURCE_DIR}/docs/UX.md"
    "${PROJECT_SOURCE_DIR}/docs/WINDOWS_BLOCK_DEVICE.md"
    DESTINATION docs)
  install(FILES
    "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES/WinFsp-NOTICE.md"
    "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES/WinFsp-LICENSE.txt"
    "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES/README.md"
    DESTINATION THIRD-PARTY-NOTICES)

  if(MSVC)
    target_compile_options(xfswin PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    target_compile_definitions(xfswin PRIVATE
      _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  if(XFSWIN_TEST_HOOKS)
    # UX-1 integration tests: one-shot sentinel files under
    # %ProgramData%\xfswin\test-hooks inject a failure at a precise point.  The
    # service runs as LocalSystem and does not inherit environment variables,
    # so a sentinel file is the transport.  Off in product builds.
    target_compile_definitions(xfswin PRIVATE XFSWIN_TEST_HOOKS=1)
  endif()
endif()

# ---------------------------------------------------------------------------
# WinFsp adapter
# ---------------------------------------------------------------------------
if(XFSWIN_BUILD_WINFSP)
  target_sources(xfs_core PRIVATE
    winfsp/winfsp_adapter.cpp
  )
  target_sources(xfswin PRIVATE
    winfsp/mount_host.cpp
    automount/windows_automount_service.cpp
  )
  target_include_directories(xfs_core SYSTEM PRIVATE ${XFSWIN_WINFSP_INCLUDE_DIR})
  target_include_directories(xfswin SYSTEM PRIVATE ${XFSWIN_WINFSP_INCLUDE_DIR})
  target_link_libraries(xfs_core PRIVATE ${XFSWIN_WINFSP_LIBRARY})
  target_link_libraries(xfswin PRIVATE ${XFSWIN_WINFSP_LIBRARY})
  # Configuration Manager device arrival/removal notifications
  target_link_libraries(xfswin PRIVATE cfgmgr32)
  # Delay-load the WinFsp runtime so all other commands work without it.
  if(MSVC)
    target_link_options(xfswin PRIVATE "/DELAYLOAD:${XFSWIN_WINFSP_LIBRARY_NAME}.dll")
    target_link_libraries(xfswin PRIVATE delayimp)
  endif()
  # WinFsp is a separately installed, shared runtime and signed kernel driver.
  # Its official pinned redistributable owns deployment; neither the build
  # output nor our portable/install payload receives an app-local WinFsp DLL.
  target_compile_definitions(xfs_core PRIVATE XFSWIN_HAS_WINFSP=1)
  target_compile_definitions(xfswin PRIVATE XFSWIN_HAS_WINFSP=1)
  message(STATUS "xfswin: WinFsp mount support enabled")
else()
  message(STATUS "xfswin: WinFsp mount support disabled")
endif()
