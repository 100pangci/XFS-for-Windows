// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - filesystem-level API (read-only).
//
// XfsFilesystem owns a BlockDevice and exposes the core operations the
// WinFsp adapter and the CLI are built on. It never writes to an existing
// filesystem.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/directory.h"
#include "xfs/error.h"
#include "xfs/extent.h"
#include "xfs/inode.h"
#include "xfs/log_checker.h"
#include "xfs/superblock.h"

namespace xfs {

struct MountOptions {
  bool verify_crc = true;
  bool check_log = true;
  // Allows mounting when the log is dirty/unknown or features are not fully
  // understood. Never allowed by default; read-only access only, and the
  // caller is responsible for the safety implications.
  bool force_unsafe_ro = false;
};

struct XfsFileInfo {
  uint64_t ino = 0;
  uint16_t mode = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint32_t projid = 0;
  uint32_t nlink = 0;
  uint64_t size = 0;
  uint64_t nblocks = 0;
  uint32_t nextents = 0;
  uint32_t anextents = 0;
  XfsTimestamp atime, mtime, ctime, crtime;
  uint16_t di_flags = 0;
  bool immutable = false;
  bool append_only = false;
};

enum class CheckStatus { kOk, kWarning, kCorrupt, kUnsupported };

const char* ToString(CheckStatus status);

struct CheckItem {
  std::string name;
  CheckStatus status = CheckStatus::kOk;
  std::string message;
};

struct CheckReport {
  std::vector<CheckItem> items;

  void Add(std::string name, CheckStatus status, std::string message = {});
  CheckStatus Overall() const;
  std::string Summary() const;
};

struct CheckOptions {
  bool secondary_superblocks = true;
  bool allocation_groups = true;
  bool btrees = true;
  bool root_directory = true;
  bool log = true;
  // Verify every inode referenced from the root directory (shallow tree walk).
  bool root_children = true;
};

class XfsFilesystem {
 public:
  static Result<std::unique_ptr<XfsFilesystem>> Mount(
      std::unique_ptr<BlockDevice> device, const MountOptions& options);

  // Non-copyable/non-movable (keeps references stable).
  XfsFilesystem(const XfsFilesystem&) = delete;
  XfsFilesystem& operator=(const XfsFilesystem&) = delete;

  const XfsGeometry& geometry() const { return geometry_; }
  const XfsSuperblock& superblock() const { return superblock_; }
  const MountOptions& options() const { return options_; }
  BlockDevice& device() { return *device_; }
  const BlockDevice& device() const { return *device_; }
  const std::vector<std::string>& warnings() const { return warnings_; }
  uint64_t RootInode() const { return geometry_.rootino; }

  // ---- core API ----
  Result<XfsInode> OpenInode(uint64_t ino) const;
  Result<XfsInode> LookupPath(std::string_view path) const;
  Result<std::vector<XfsDirEntry>> ListDirectory(uint64_t ino) const;
  Result<XfsDirEntry> LookupName(uint64_t dir_ino, std::string_view name) const;
  Result<size_t> ReadFile(uint64_t ino, uint64_t offset, void* buffer,
                          size_t length) const;
  Result<std::string> ReadSymlink(uint64_t ino) const;
  Result<XfsFileInfo> GetFileInfo(uint64_t ino) const;

  // ---- read-only consistency validation (xfswin check) ----
  CheckReport CheckConsistency(const CheckOptions& options) const;

 private:
  XfsFilesystem() = default;

  // Read-only caches. Directory contents and inodes cannot change while we
  // mount (the filesystem is never written by this process), so caching them
  // is safe and makes large-directory enumeration fast.
  struct DirCache {
    std::vector<XfsDirEntry> entries;
    std::unordered_map<std::string, size_t> index;
  };
  void CacheDirectory(uint64_t ino, std::vector<XfsDirEntry> entries) const;

  std::unique_ptr<BlockDevice> device_;
  XfsSuperblock superblock_;
  XfsGeometry geometry_;
  MountOptions options_;
  std::vector<std::string> warnings_;

  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<uint64_t, XfsInode> inode_cache_;
  // 16 KiB inode-chunk read-ahead (shared so lookups stay cheap).
  mutable std::unordered_map<uint64_t,
                             std::shared_ptr<const std::vector<uint8_t>>>
      inode_chunk_cache_;
  mutable std::unordered_map<uint64_t, XfsFileInfo> info_cache_;
  mutable std::unordered_map<uint64_t,
                             std::shared_ptr<const std::vector<XfsExtent>>>
      extent_cache_;
  mutable std::unordered_map<uint64_t, DirCache> dir_cache_;
  mutable size_t dir_cache_entries_ = 0;
};

}  // namespace xfs
