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
  core/invalidation.cpp
  block/image_device.cpp
  block/windows_raw_device.cpp
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
  write/alloc.cpp
  write/bmap.cpp
  write/ialloc.cpp
  write/dir_write.cpp
  write/namespace.cpp
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

target_include_directories(xfs_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
  target_compile_options(xfs_core PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(XFSWIN_WARNINGS_AS_ERRORS)
    target_compile_options(xfs_core PRIVATE /WX)
  endif()
  target_compile_definitions(xfs_core PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# xfswin - command line tool: list / info / check / ls / cat / mount / format
# ---------------------------------------------------------------------------
add_executable(xfswin
  cli/main.cpp
  cli/commands.cpp
)

target_link_libraries(xfswin PRIVATE xfs_core)

target_include_directories(xfswin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
  target_compile_options(xfswin PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(xfswin PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
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
  )
  target_include_directories(xfs_core PRIVATE ${XFSWIN_WINFSP_INCLUDE_DIR})
  target_include_directories(xfswin PRIVATE ${XFSWIN_WINFSP_INCLUDE_DIR})
  target_link_libraries(xfs_core PRIVATE ${XFSWIN_WINFSP_LIBRARY})
  target_link_libraries(xfswin PRIVATE ${XFSWIN_WINFSP_LIBRARY})
  # SDDL security descriptors
  target_link_libraries(xfs_core PRIVATE advapi32)
  target_link_libraries(xfswin PRIVATE advapi32)
  # Delay-load the WinFsp runtime so all other commands work without it.
  if(MSVC)
    target_link_options(xfswin PRIVATE "/DELAYLOAD:winfsp-x64.dll")
    target_link_libraries(xfswin PRIVATE delayimp)
  endif()
  # Make a locally extracted SDK runtime available to the build output (the
  # driver still has to be installed system-wide by the WinFsp installer).
  file(GLOB _winfsp_dlls LIST_DIRECTORIES false
       "D:/SDK/WinFsp/winfsp-*/DYNAMIC/SxS/DYNAMIC/bin/winfsp-x64.dll")
  list(LENGTH _winfsp_dlls _winfsp_dll_count)
  if(_winfsp_dll_count GREATER 0)
    list(GET _winfsp_dlls 0 _winfsp_dll)
    add_custom_command(TARGET xfswin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_winfsp_dll}" "$<TARGET_FILE_DIR:xfswin>")
  endif()
  target_compile_definitions(xfs_core PRIVATE XFSWIN_HAS_WINFSP=1)
  target_compile_definitions(xfswin PRIVATE XFSWIN_HAS_WINFSP=1)
  message(STATUS "xfswin: WinFsp mount support enabled")
else()
  message(STATUS "xfswin: WinFsp mount support disabled")
endif()
