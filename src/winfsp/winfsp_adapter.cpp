// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "winfsp/winfsp_adapter.h"

#ifdef XFSWIN_HAS_WINFSP

// The WinFsp 2.1 SDK header uses PNTSTATUS, a WDK type that the user-mode
// Windows SDK does not define. Include the Windows headers with WIN32_NO_STATUS
// so ntstatus.h can provide the STATUS_* constants, provide PNTSTATUS, and
// only then include the WinFsp header (whose own includes become no-ops).
#define WIN32_NO_STATUS
#include <memory>
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>
typedef NTSTATUS* PNTSTATUS;

#include <winfsp/winfsp.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xfs/concurrency_hooks.h"
#include "xfs/log.h"
#include "xfs/perf_stats.h"
#include "xfs/ondisk.h"
#include "xfs/extent.h"
#include "xfs/win_name_map.h"

namespace xfs::winfsp {
namespace {

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
}

std::u16string ToU16(const std::wstring& w) {
  return std::u16string(w.begin(), w.end());
}

struct DirChild {
  std::u16string name;
  uint64_t ino;
};

struct AdapterState {
  std::unique_ptr<XfsFilesystem> fs;
  PSECURITY_DESCRIPTOR security_descriptor = nullptr;
  ULONG security_descriptor_size = 0;
  std::u16string label;
  uint16_t sector_size = 512;
  uint16_t sectors_per_allocation_unit = 8;
  // Read-write volume: the Write/Flush callbacks are live and the file
  // attributes must not claim that everything is read-only.
  bool read_write = false;
  // Latched once the block device reports a terminal failure (removed, media
  // changed, invalid handle).  Every later device-touching callback fails
  // fast instead of retrying, and the mount host is asked to unmount.
  std::atomic<bool> device_failed{false};

  // WinFsp's default FileContext model is a "file node": opening the same file
  // must return the same node value for as long as it is open anywhere, and
  // Close may be called once per Open (including for a failed Overwrite).
  // Nodes are keyed by inode number and reference counted; the last Close
  // destroys the node.
  struct FileNode {
    // Shared, immutable inode record: callbacks hold this instead of copying
    // the whole inode (with its fork vectors) on every operation.
    std::shared_ptr<const XfsInode> inode;
    // Data-fork extent list, parsed once at open for regular files so reads
    // never re-walk the BMBT.
    std::shared_ptr<const std::vector<XfsExtent>> extents;
    // Extent-map generation the cached extent list belongs to (PERF-1). A
    // data-only overwrite bumps the data cache generation but not this one, so
    // Write does not reparse the BMBT after every callback.
    uint64_t extent_generation = 0;
    uint32_t refcount = 0;
    // Where the name lives. Rename refreshes it while the node stays open.
    // Cleanup needs it for delete-on-close.
    uint64_t parent_ino = 0;
    std::string xfs_name;
    // Delete-on-close: Cleanup removed the namespace entry and put the inode
    // on the AGI unlinked list; the last Close frees the inode
    // (FreeUnlinkedInode).
    bool delete_pending = false;
  };
  xfs::conc::Mutex node_mutex;
  std::unordered_map<uint64_t, uint64_t> node_by_inode;
  std::unordered_map<uint64_t, std::shared_ptr<FileNode>> nodes;
  uint64_t next_node_id = 1;

  // Resolved WinFsp path -> inode number. Explorer queries security and then
  // opens the same file, and revisits files across refreshes; this turns the
  // repeated path walk into one hash lookup. Bounded LRU rather than the old
  // "clear the whole map at 32768 entries" cache cliff.
  struct PathCacheEntry {
    uint64_t ino = 0;
    std::list<std::wstring>::iterator lru;
  };
  std::unordered_map<std::wstring, PathCacheEntry> path_inode_cache;
  std::list<std::wstring> path_cache_lru;  // front = most recently used

  // Sorted (Windows name, inode) children per directory. On a read-only mount
  // directory contents never change, so this is built once per directory and
  // reused by every ReadDirectory page (WinFsp re-enters with a marker for
  // each page) and by repeated Explorer enumerations. Every namespace
  // mutation (create, mkdir, unlink, rmdir) drops the affected directories
  // through DropNamespaceCaches, and the core caches through
  // XfsFilesystem::InvalidateCaches.
  std::unordered_map<uint64_t, std::shared_ptr<const std::vector<DirChild>>>
      dir_children;
  std::list<uint64_t> dir_children_lru;  // front = most recently used
  size_t dir_children_entries = 0;
};

// A snapshot of the immutable parts of a live file node. All fields are copied
// while node_mutex is held, so a concurrent RefreshNode/Rename/Cleanup cannot
// race with a Read, Write or GetFileInfo callback. The shared_ptrs keep the
// old inode/extent image alive until the callback is done with it.
struct NodeSnapshot {
  std::shared_ptr<const XfsInode> inode;
  std::shared_ptr<const std::vector<XfsExtent>> extents;
  uint64_t extent_generation = 0;
};

uint64_t NodeId(PVOID context) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
}

// Returns the current immutable inode/extent image of a live file node.
std::optional<NodeSnapshot> LookupNode(AdapterState& state, PVOID context) {
  if (!context) return std::nullopt;
  std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
  auto it = state.nodes.find(NodeId(context));
  if (it == state.nodes.end()) return std::nullopt;
  return NodeSnapshot{it->second->inode, it->second->extents,
                      it->second->extent_generation};
}

// Returns the mutable node record for the few callbacks that own a mutation
// (RefreshNode, Cleanup and Rename). Callers must not read its unguarded
// fields without holding node_mutex.
std::shared_ptr<AdapterState::FileNode> LookupNodeState(AdapterState& state,
                                                        PVOID context) {
  if (!context) return nullptr;
  std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
  auto it = state.nodes.find(NodeId(context));
  if (it == state.nodes.end()) return nullptr;
  return it->second;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AdapterState* State(FSP_FILE_SYSTEM* fs) {
  return static_cast<AdapterState*>(fs->UserContext);
}

void CountCallback(std::atomic<uint64_t>& counter) {
  PerfStats& perf = PerfStats::Get();
  if (perf.enabled()) counter.fetch_add(1, std::memory_order_relaxed);
}

// A process mounts at most one filesystem (the stop event below already
// assumes this), so the active mount state is reachable from the error
// translation helpers that do not receive FSP_FILE_SYSTEM*.
std::atomic<AdapterState*> g_mount_state{nullptr};
HANDLE g_stop_event = nullptr;

// The raw-device backend latches a terminal failure (device removed, media
// changed, invalid handle) into DeviceState::kInvalid.  A mount must not keep
// serving a dead device: latch the failure once, make every later
// device-touching callback fail, and signal the mount host so it runs the
// unmount path instead of leaving a dead drive letter attached.  The unmount
// itself will report the failed durable close, which is the correct outcome.
void NoteDeviceFailure() {
  AdapterState* state = g_mount_state.load(std::memory_order_acquire);
  if (!state || !state->fs) return;
  if (state->fs->device().State() != DeviceState::kInvalid) return;
  if (state->device_failed.exchange(true, std::memory_order_relaxed)) return;
  XFS_ERROR("block device failed terminally ("
            << ToString(state->fs->device().LastErrorKind())
            << "); stopping the mount");
  if (g_stop_event) SetEvent(g_stop_event);
}

// Publishes the active mount to NoteDeviceFailure() for exactly as long as
// callbacks can run against it.
class MountStateScope {
 public:
  explicit MountStateScope(AdapterState* state) {
    g_mount_state.store(state, std::memory_order_release);
  }
  ~MountStateScope() {
    g_mount_state.store(nullptr, std::memory_order_release);
  }
  MountStateScope(const MountStateScope&) = delete;
  MountStateScope& operator=(const MountStateScope&) = delete;
};

NTSTATUS NtFromError(XfsError code) {
  // Device I/O errors are the only ones that can mean the device went away.
  if (code == XfsError::kIoError || code == XfsError::kShortRead) {
    NoteDeviceFailure();
  }
  switch (code) {
    case XfsError::kOk: return STATUS_SUCCESS;
    case XfsError::kNotFound: return STATUS_OBJECT_NAME_NOT_FOUND;
    case XfsError::kNotDirectory: return STATUS_NOT_A_DIRECTORY;
    case XfsError::kNotRegularFile: return STATUS_INVALID_DEVICE_REQUEST;
    case XfsError::kNotSymlink: return STATUS_INVALID_DEVICE_REQUEST;
    case XfsError::kNameTooLong: return STATUS_OBJECT_NAME_INVALID;
    case XfsError::kAlreadyExists: return STATUS_OBJECT_NAME_COLLISION;
    case XfsError::kDirectoryNotEmpty: return STATUS_DIRECTORY_NOT_EMPTY;
    case XfsError::kTooManyLinks: return STATUS_TOO_MANY_LINKS;
    case XfsError::kWouldCycle: return STATUS_ACCESS_DENIED;
    case XfsError::kInvalidArgument: return STATUS_INVALID_PARAMETER;
    case XfsError::kPermissionDenied: return STATUS_ACCESS_DENIED;
    case XfsError::kReadOnlyVolume: return STATUS_MEDIA_WRITE_PROTECTED;
    case XfsError::kIoError:
    case XfsError::kShortRead: return STATUS_IO_DEVICE_ERROR;
    case XfsError::kUnsupportedFeature:
    case XfsError::kUnsupportedGeometry: return STATUS_NOT_SUPPORTED;
    // RW-0 write limitations: the target range needs an allocation or a
    // conversion transaction that this build does not implement yet.
    case XfsError::kWriteExtendsFile:
    case XfsError::kWriteNeedsAllocation:
    case XfsError::kWriteNeedsConversion: return STATUS_NOT_SUPPORTED;
    case XfsError::kNoSpace: return STATUS_DISK_FULL;
    case XfsError::kDirtyLog:
    case XfsError::kNeedsRepair: return STATUS_VOLUME_DIRTY;
    case XfsError::kBadCrc:
    case XfsError::kBadMagic:
    case XfsError::kBadUuid:
    case XfsError::kBadOwner:
    case XfsError::kBadBlockNo:
    case XfsError::kCorruptSuperblock:
    case XfsError::kCorruptAgHeader:
    case XfsError::kCorruptBtree:
    case XfsError::kCorruptInode:
    case XfsError::kCorruptDirectory:
    case XfsError::kCorruptExtent:
    case XfsError::kOutOfRange: return STATUS_FILE_CORRUPT_ERROR;
    default: return STATUS_INTERNAL_ERROR;
  }
}

NTSTATUS NtFromStatus(const Status& status) {
  return NtFromError(status.error().code);
}

std::wstring JoinName(const std::wstring& parent, const std::wstring& child) {
  if (parent.empty() || parent == L"\\") return L"\\" + child;
  if (parent.back() == L'\\') return parent + child;
  return parent + L"\\" + child;
}

// Splits a WinFsp path (backslash separated) into components.
std::vector<std::wstring> SplitPath(PWSTR file_name) {
  std::vector<std::wstring> parts;
  std::wstring current;
  for (const wchar_t* p = file_name; *p; ++p) {
    if (*p == L'\\') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(*p);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

Result<XfsInode> ResolvePath(XfsFilesystem& fs, PWSTR file_name) {
  auto parts = SplitPath(file_name);
  uint64_t ino = fs.RootInode();
  auto current = fs.OpenInode(ino);
  if (!current.ok()) return current.error();
  for (const auto& part : parts) {
    if (part == L".") continue;
    if (!current.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory,
                     "path component is not a directory")
          .Ino(current.value().ino);
    }
    auto xfs_name = WindowsNameToXfs(ToU16(part));
    if (!xfs_name) {
      return XFS_ERR(XfsError::kNotFound, "name is not representable")
          .Ino(current.value().ino);
    }
    auto entry = fs.LookupName(current.value().ino, *xfs_name);
    if (!entry.ok()) return entry.error();
    auto next = fs.OpenInode(entry.value().ino);
    if (!next.ok()) return next.error();
    current = std::move(next);
  }
  return current;
}

// Path resolution front-end for the WinFsp callbacks: memoizes full paths so
// repeated opens of the same file skip the component walk.
Result<XfsInode> ResolvePathCached(AdapterState& state, PWSTR file_name) {
  const std::wstring key = file_name ? file_name : L"";
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  uint64_t cached_ino = 0;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto it = state.path_inode_cache.find(key);
    if (it != state.path_inode_cache.end()) {
      cached_ino = it->second.ino;
      state.path_cache_lru.splice(state.path_cache_lru.begin(),
                                  state.path_cache_lru, it->second.lru);
      it->second.lru = state.path_cache_lru.begin();
    }
  }
  if (cached_ino != 0) {
    auto inode = state.fs->OpenInode(cached_ino);
    if (inode.ok()) {
      if (perf_on) {
        perf.path_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      return inode;
    }
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto it = state.path_inode_cache.find(key);
    if (it != state.path_inode_cache.end()) {
      state.path_cache_lru.erase(it->second.lru);
      state.path_inode_cache.erase(it);
    }
  }
  if (perf_on) perf.path_cache_misses.fetch_add(1, std::memory_order_relaxed);
  auto inode = ResolvePath(*state.fs, file_name);
  if (inode.ok()) {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    constexpr size_t kMaxPathCacheEntries = 32768;
    auto existing = state.path_inode_cache.find(key);
    if (existing != state.path_inode_cache.end()) {
      state.path_cache_lru.erase(existing->second.lru);
      state.path_inode_cache.erase(existing);
    }
    while (state.path_inode_cache.size() >= kMaxPathCacheEntries &&
           !state.path_cache_lru.empty()) {
      const std::wstring victim = state.path_cache_lru.back();
      state.path_cache_lru.pop_back();
      state.path_inode_cache.erase(victim);
    }
    state.path_cache_lru.push_front(key);
    state.path_inode_cache.emplace(
        key, AdapterState::PathCacheEntry{inode.value().ino,
                                          state.path_cache_lru.begin()});
  }
  return inode;
}

// Splits a WinFsp path into the parent directory's inode and the on-disk
// (raw XFS) name of the last component. This is what create/mkdir/unlink/
// rmdir need: the namespace API works on (parent, name) pairs.
Result<std::pair<uint64_t, std::string>> ResolveParentAndName(
    AdapterState& state, PWSTR file_name) {
  auto parts = SplitPath(file_name);
  if (parts.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot create the root")
        .Metadata("namespace");
  }
  auto name = WindowsNameToXfs(ToU16(parts.back()));
  if (!name) {
    return XFS_ERR(XfsError::kNotFound, "name is not representable")
        .Metadata("namespace");
  }
  parts.pop_back();
  // Reuse the same path memo as Open/GetSecurity: Explorer and copy loops
  // resolve the same parent directory for every child. Building the parent
  // path once turns the component walk into one cache hit.
  std::wstring parent_path;
  for (const std::wstring& part : parts) {
    parent_path.push_back(L'\\');
    parent_path += part;
  }
  if (parent_path.empty()) parent_path = L"\\";
  auto parent = ResolvePathCached(state, parent_path.data());
  if (!parent.ok()) return parent.error();
  return std::make_pair(parent.value().ino, *name);
}
// Targeted path-memo invalidation. prefix removes every entry below a
// directory (used when a directory is created, deleted or renamed); otherwise
// only the exact path is removed. This replaces the old "clear every path on
// every namespace mutation" policy.
void DropPathCache(AdapterState& state, const std::wstring& path,
                   bool prefix = false) {
  if (path.empty()) return;
  std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
  if (!prefix) {
    auto it = state.path_inode_cache.find(path);
    if (it != state.path_inode_cache.end()) {
      state.path_cache_lru.erase(it->second.lru);
      state.path_inode_cache.erase(it);
    }
    return;
  }
  const std::wstring needle =
      (!path.empty() && path.back() == L'\\') ? path : path + L"\\";
  for (auto it = state.path_inode_cache.begin();
       it != state.path_inode_cache.end();) {
    if (it->first == path || it->first.compare(0, needle.size(), needle) == 0) {
      state.path_cache_lru.erase(it->second.lru);
      it = state.path_inode_cache.erase(it);
    } else {
      ++it;
    }
  }
}

// A namespace mutation changed the directory listing: drop the adapter's
// per-directory listings. The path memo is invalidated separately and
// precisely at each call site. parent_ino == 0 drops the whole path memo
// (last-close of an unlinked inode).
void DropNamespaceCaches(AdapterState& state, uint64_t parent_ino,
                         uint64_t second_parent_ino = 0) {
  std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
  for (uint64_t dir : {parent_ino, second_parent_ino}) {
    if (dir == 0) continue;
    auto it = state.dir_children.find(dir);
    if (it != state.dir_children.end()) {
      state.dir_children_entries -= it->second->size();
      state.dir_children.erase(it);
    }
    state.dir_children_lru.remove(dir);
  }
  if (parent_ino == 0 && second_parent_ino == 0) {
    state.path_inode_cache.clear();
    state.path_cache_lru.clear();
  }
}
// Read-only delete validation shared by SetDelete and CanDelete: can this
// inode be removed right now? Mirrors the checks XfsFilesystem's namespace
// layer makes before it would modify anything.
Status CheckDeletable(AdapterState& state, const XfsInode& inode) {
  if (inode.ino == state.fs->RootInode()) {
    return XFS_ERR(XfsError::kPermissionDenied, "cannot delete the root")
        .Ino(inode.ino)
        .Metadata("delete");
  }
  if ((inode.flags & ondisk::inode::kDiFlagImmutable) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied, "inode is immutable")
        .Ino(inode.ino)
        .Metadata("delete");
  }
  if (inode.IsDir()) {
    // xfs_dir_remove_child: nlink must be 2 and only "." and ".." may remain.
    if (inode.nlink != 2) {
      return XFS_ERR(XfsError::kDirectoryNotEmpty, "directory is not empty")
          .Ino(inode.ino)
          .Actual(inode.nlink)
          .Metadata("delete");
    }
    auto listing = state.fs->ListDirectory(inode.ino);
    if (!listing.ok()) return listing.error();
    for (const XfsDirEntry& entry : listing.value()) {
      if (entry.name != "." && entry.name != "..") {
        return XFS_ERR(XfsError::kDirectoryNotEmpty, "directory is not empty")
            .Ino(inode.ino)
            .Metadata("delete");
      }
    }
    return OkStatus();
  }
  if (!inode.IsRegular() && !inode.IsSymlink()) {
    // Only regular files, directories and symlinks may be deleted.
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "only regular files, symlinks and directories can be "
                   "deleted")
        .Ino(inode.ino)
        .Metadata("delete");
  }
  return OkStatus();
}

bool IsOverwriteDisposition(UINT32 create_options) {
  // Disposition lives in the high 8 bits of the create options (WinFsp SDK
  // fsctl.h); FILE_SUPERSEDE = 0, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5.
  switch ((create_options >> 24) & 0xFF) {
    case 0:   // FILE_SUPERSEDE
    case 4:   // FILE_OVERWRITE
    case 5:   // FILE_OVERWRITE_IF
      return true;
    default:
      return false;
  }
}

uint64_t ToFileTime(const XfsTimestamp& ts) {
  constexpr int64_t kEpochOffset = 11644473600LL;  // 1601 -> 1970 seconds
  int64_t sec = ts.sec + kEpochOffset;
  if (sec < 0) sec = 0;
  return static_cast<uint64_t>(sec) * 10000000ULL + ts.nsec / 100;
}

XfsTimestamp FromFileTime(uint64_t file_time) {
  constexpr int64_t kEpochOffset = 11644473600LL;  // 1601 -> 1970 seconds
  XfsTimestamp ts;
  ts.sec = static_cast<int64_t>(file_time / 10000000ULL) - kEpochOffset;
  ts.nsec = static_cast<uint32_t>((file_time % 10000000ULL) * 100);
  return ts;
}

uint32_t FileAttributesFor(const AdapterState& state, uint16_t mode) {
  uint32_t attributes = 0;
  if ((mode & 0170000) == 0040000) attributes |= FILE_ATTRIBUTE_DIRECTORY;
  // Read-only volume: everything is read-only (the volume really is). On a
  // writable volume the POSIX write bits decide, so a 0644 file can be
  // written while a 0444 file stays read-only. XFS immutable/append-only
  // inodes are handled (and refused) in the write path itself.
  if (!state.read_write || (mode & 0222) == 0) {
    attributes |= FILE_ATTRIBUTE_READONLY;
  }
  if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
  return attributes;
}

// Write callbacks that are not implemented yet. On a read-only volume they
// report write protection (that is what is true); on a read-write volume they
// must not claim the media is protected, so they report "not supported".
NTSTATUS NotImplementedWrite(const AdapterState& state) {
  return state.read_write ? STATUS_NOT_SUPPORTED : STATUS_MEDIA_WRITE_PROTECTED;
}

void FillFileInfoCore(AdapterState& state, uint16_t mode, uint32_t nlink,
                      uint64_t nblocks, uint64_t size,
                      const XfsTimestamp& atime, const XfsTimestamp& mtime,
                      const XfsTimestamp& ctime, const XfsTimestamp& crtime,
                      uint64_t ino, FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  info->FileAttributes = FileAttributesFor(state, mode);
  // di_nblocks counts filesystem blocks: the kernel adds bmap extent lengths
  // to i_nblocks directly and xfs_repair recomputes it the same way.
  info->AllocationSize = nblocks * state.fs->geometry().block_size;
  info->FileSize = (mode & 0170000) == 0040000 ? 0 : size;
  info->CreationTime = ToFileTime(crtime);
  info->LastAccessTime = ToFileTime(atime);
  info->LastWriteTime = ToFileTime(mtime);
  info->ChangeTime = ToFileTime(ctime);
  info->IndexNumber = ino;
  // The POSIX link count, so Explorer and fsutil see hard links correctly.
  info->HardLinks = nlink;
  info->EaSize = 0;
  info->ReparseTag = 0;
}

void FillFileInfo(AdapterState& state, const XfsInode& inode,
                  FSP_FSCTL_FILE_INFO* info) {
  FillFileInfoCore(state, inode.mode, inode.nlink, inode.nblocks, inode.size,
                   inode.atime, inode.mtime, inode.ctime, inode.crtime,
                   inode.ino, info);
}

void FillFileInfo(AdapterState& state, const XfsFileInfo& inode,
                  FSP_FSCTL_FILE_INFO* info) {
  FillFileInfoCore(state, inode.mode, inode.nlink, inode.nblocks, inode.size,
                   inode.atime, inode.mtime, inode.ctime, inode.crtime,
                   inode.ino, info);
}

// Replaces a node's cached inode and extent list after a mutation. The core
// invalidated its caches when the transaction committed; re-reading here gives
// WinFsp the new size, timestamp and extent map.
void RefreshNode(AdapterState& state, uint64_t node_id) {
  auto node = LookupNodeState(state, reinterpret_cast<PVOID>(
                                        static_cast<uintptr_t>(node_id)));
  if (!node) return;
  uint64_t ino = 0;
  std::shared_ptr<const std::vector<XfsExtent>> old_extents;
  uint64_t old_generation = 0;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    ino = node->inode->ino;
    old_extents = node->extents;
    old_generation = node->extent_generation;
  }
  // Reload outside node_mutex: a read of the inode/btree must not block every
  // other open/close/read-directory callback in the adapter.
  auto inode = state.fs->OpenInode(ino);
  if (!inode.ok()) {
    XFS_ERROR("RefreshNode: cannot reload inode: " << inode.error().Format());
    return;
  }
  auto fresh = std::make_shared<const XfsInode>(std::move(inode.value()));
  std::shared_ptr<const std::vector<XfsExtent>> fresh_extents = old_extents;
  const uint64_t generation = state.fs->ExtentGeneration(ino);
  if (fresh->IsRegular() && generation != old_generation) {
    // Parse through the core API: it takes write_barrier_ and the inode's
    // extent-map lock. A direct LoadDataForkExtents here would race with an
    // ordinary write that frees a bmbt block.
    auto loaded = state.fs->GetDataExtents(ino);
    if (!loaded.ok()) {
      XFS_ERROR("RefreshNode: cannot reload extents: "
                << loaded.error().Format());
      return;
    }
    auto extents =
        std::make_shared<std::vector<XfsExtent>>(std::move(loaded.value()));
    fresh_extents = std::shared_ptr<const std::vector<XfsExtent>>(
        std::move(extents));
  }
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto it = state.nodes.find(node_id);
    if (it == state.nodes.end() || it->second != node) return;
    node->inode = std::move(fresh);
    node->extents = std::move(fresh_extents);
    node->extent_generation = generation;
  }
}
// Windows sends SetAllocationSize/AllocationSize hints for the copy path. Honor
// them with bounded transactions so a multi-gigabyte hint does not create one
// enormous log record. Preallocation is an optimization: ENOSPC or an IO error
// stops the hint but does not fail the create/write, which can still allocate
// on demand. The reserved eighth keeps normal writes from being starved.
Status PreallocateHint(XfsFilesystem& fs, uint64_t ino, uint64_t size) {
  if (size == 0) return OkStatus();
  constexpr uint64_t kChunkBytes = 256ull << 20;
  const uint64_t block_size = fs.geometry().block_size;
  const uint64_t free_blocks = fs.superblock().fdblocks;
  uint64_t limit = size;
  if (free_blocks > 0 && block_size > 0 &&
      free_blocks <= (UINT64_MAX / block_size)) {
    const uint64_t free_bytes = free_blocks * block_size;
    const uint64_t usable = free_bytes - free_bytes / 8;
    if (usable < limit) limit = usable;
  }
  for (uint64_t offset = 0; offset < limit; offset += kChunkBytes) {
    const uint64_t length =
        std::min<uint64_t>(kChunkBytes, limit - offset);
    auto pre =
        fs.AllocateUnwritten(ino, offset, length, /*keep_size=*/true);
    if (!pre.ok()) return pre.error();
  }
  return OkStatus();
}

// Registers an open file node for `inode` (the WinFsp file-context model:
// one node per inode, reference counted) and returns its context value.
Result<PVOID> TakeNode(AdapterState& state, XfsInode inode, uint64_t parent_ino,
                       std::string xfs_name) {
  auto node = std::make_shared<const XfsInode>(std::move(inode));
  // Parse the data fork once per open: reads then never re-walk the extent
  // list or the BMBT.
  std::shared_ptr<const std::vector<XfsExtent>> extents;
  if (node->IsRegular() && node->format != ondisk::inode::kFormatLocal) {
    // The core API takes write_barrier_ shared and the inode's extent-map
    // lock shared, so the BMBT parse cannot race with a writer freeing a
    // bmbt block.
    auto loaded = state.fs->GetDataExtents(node->ino);
    if (loaded.ok()) {
      auto parsed =
          std::make_shared<std::vector<XfsExtent>>(std::move(loaded.value()));
      extents = std::shared_ptr<const std::vector<XfsExtent>>(
          std::move(parsed));
    }
  }
  uint64_t id;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto index = state.node_by_inode.find(node->ino);
    if (index != state.node_by_inode.end()) {
      // Same file already open: reuse its node and take another reference.
      id = index->second;
      ++state.nodes[id]->refcount;
    } else {
      id = state.next_node_id++;
      auto entry = std::make_shared<AdapterState::FileNode>();
      entry->inode = node;
      entry->extents = std::move(extents);
      // node_mutex is the outermost project lock (docs/LOCKING.md); this
      // leaf cache lookup is the only core call made while holding it.
      entry->extent_generation = state.fs->ExtentGeneration(node->ino);
      entry->refcount = 1;
      entry->parent_ino = parent_ino;
      entry->xfs_name = std::move(xfs_name);
      state.nodes.emplace(id, std::move(entry));
      state.node_by_inode.emplace(node->ino, id);
    }
  }
  return reinterpret_cast<PVOID>(static_cast<uintptr_t>(id));
}

void FillSecurity(AdapterState& state, PSECURITY_DESCRIPTOR buffer,
                  SIZE_T* size, PUINT32 attributes, uint32_t attrs) {
  if (attributes) *attributes = attrs;
  if (size) {
    if (*size < state.security_descriptor_size) {
      *size = state.security_descriptor_size;
      // Caller sees STATUS_BUFFER_OVERFLOW and retries.
    } else {
      *size = state.security_descriptor_size;
      if (buffer) {
        std::memcpy(buffer, state.security_descriptor,
                    state.security_descriptor_size);
      }
    }
  }
}

bool NeedsSecurityOverflow(AdapterState& state, SIZE_T* size) {
  if (!size) return false;
  if (*size < state.security_descriptor_size) {
    *size = state.security_descriptor_size;
    return true;
  }
  return false;
}


// Reads data for a regular file or a symlink (whose target is exposed as the
// file content in this first version).
Result<size_t> ReadInode(AdapterState& state, const XfsInode& inode,
                         const std::vector<XfsExtent>* extents,
                         uint64_t offset, void* buffer, size_t length) {
  if (inode.IsSymlink()) {
    auto target = state.fs->ReadSymlink(inode.ino);
    if (!target.ok()) return target.error();
    if (offset >= target.value().size()) return 0;
    size_t to_copy = static_cast<size_t>(
        std::min<uint64_t>(length, target.value().size() - offset));
    std::memcpy(buffer, target.value().data() + offset, to_copy);
    return to_copy;
  }
  return state.fs->ReadFile(inode, extents, offset, buffer, length);
}

// ---------------------------------------------------------------------------
// WinFsp callbacks
// ---------------------------------------------------------------------------

NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* file_system,
                       FSP_FSCTL_VOLUME_INFO* volume_info) {
  AdapterState& state = *State(file_system);
  const XfsGeometry& geo = state.fs->geometry();
  const XfsSuperblock& sb = state.fs->superblock();
  volume_info->TotalSize = geo.dblocks * geo.block_size;
  volume_info->FreeSize = sb.fdblocks * geo.block_size;
  size_t label_bytes =
      std::min<size_t>(state.label.size() * sizeof(char16_t),
                       sizeof(volume_info->VolumeLabel));
  volume_info->VolumeLabelLength = static_cast<UINT16>(label_bytes);
  std::memcpy(volume_info->VolumeLabel, state.label.data(), label_bytes);
  return STATUS_SUCCESS;
}

NTSTATUS SetVolumeLabel(FSP_FILE_SYSTEM* file_system, PWSTR, FSP_FSCTL_VOLUME_INFO*) {
  return NotImplementedWrite(*State(file_system));
}

NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                           PUINT32 file_attributes,
                           PSECURITY_DESCRIPTOR security_descriptor,
                           SIZE_T* security_descriptor_size) {
  AdapterState& state = *State(file_system);
  auto inode = ResolvePathCached(state, file_name);
  if (!inode.ok()) {
    // Expected for speculative lookups (e.g. Windows probing \autorun.inf).
    XFS_DEBUG("GetSecurityByName('" << WideToUtf8(file_name)
                                     << "') failed: "
                                     << inode.error().Format());
    return NtFromError(inode.error().code);
  }
  uint32_t attributes = FileAttributesFor(state, inode.value().mode);
  if (NeedsSecurityOverflow(state, security_descriptor_size)) {
    if (file_attributes) *file_attributes = attributes;
    return STATUS_BUFFER_OVERFLOW;
  }
  FillSecurity(state, security_descriptor, security_descriptor_size,
               file_attributes, attributes);
  return STATUS_SUCCESS;
}

NTSTATUS Create(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
                UINT32 create_options, UINT32 /*granted_access*/,
                UINT32 /*file_attributes*/, PSECURITY_DESCRIPTOR /*security*/,
                UINT64 allocation_size, PVOID* file_context,
                FSP_FSCTL_FILE_INFO* file_info) {
  CountCallback(PerfStats::Get().fs_create_calls);
  AdapterState& state = *State(file_system);
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  // The FSD only calls Create when the target must be created: FILE_CREATE
  // always, FILE_OPEN_IF / FILE_OVERWRITE_IF after Open reported
  // OBJECT_NAME_NOT_FOUND. Existence is therefore an error here.
  auto target = ResolveParentAndName(state, file_name);
  if (!target.ok()) return NtFromError(target.error().code);
  const uint64_t parent_ino = target.value().first;
  const std::string& name = target.value().second;
  const bool directory = (create_options & FILE_DIRECTORY_FILE) != 0;

  uint64_t ino = 0;
  if (directory) {
    Status status = state.fs->Mkdir(parent_ino, name, 0755, 0, 0);
    if (!status.ok()) return NtFromStatus(status);
    auto entry = state.fs->LookupName(parent_ino, name);
    if (!entry.ok()) return NtFromError(entry.error().code);
    ino = entry.value().ino;
  } else {
    auto created = state.fs->CreateRegularFile(parent_ino, name, 0644, 0, 0);
    if (!created.ok()) return NtFromError(created.error().code);
    ino = created.value();
    // AllocationSize is a real hint from the Windows copy path. Preallocate
    // unwritten extents without moving EOF; a later write converts only the
    // blocks it actually writes. Failure to preallocate is not fatal (the
    // write path can still allocate on demand), so log and continue.
    if (allocation_size > 0) {
      Status pre = PreallocateHint(*state.fs, ino, allocation_size);
      if (!pre.ok()) {
        XFS_WARN("Create: preallocation of " << allocation_size
                                             << " bytes failed: "
                                             << pre.error().Format());
      }
    }
  }
  auto inode = state.fs->OpenInode(ino);
  if (!inode.ok()) return NtFromError(inode.error().code);
  auto context = TakeNode(state, std::move(inode.value()), parent_ino, name);
  if (!context.ok()) return NtFromError(context.error().code);
  *file_context = context.value();
  auto info = state.fs->GetFileInfo(ino);
  if (!info.ok()) return NtFromError(info.error().code);
  FillFileInfo(state, info.value(), file_info);
  DropNamespaceCaches(state, parent_ino);
  DropPathCache(state, file_name ? std::wstring(file_name) : std::wstring(),
                directory);
  return STATUS_SUCCESS;
}

NTSTATUS Open(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
              UINT32 create_options, UINT32 /*granted_access*/,
              PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) {
  CountCallback(PerfStats::Get().fs_open_calls);
  AdapterState& state = *State(file_system);
  if (SplitPath(file_name).empty()) {
    // The root directory itself: no parent, never deletable.
    if ((create_options & FILE_NON_DIRECTORY_FILE) != 0) {
      return STATUS_FILE_IS_A_DIRECTORY;
    }
    auto inode = state.fs->OpenInode(state.fs->RootInode());
    if (!inode.ok()) return NtFromError(inode.error().code);
    auto context = TakeNode(state, std::move(inode.value()),
                            state.fs->RootInode(), std::string());
    if (!context.ok()) return NtFromError(context.error().code);
    *file_context = context.value();
    auto info = state.fs->GetFileInfo(state.fs->RootInode());
    if (!info.ok()) return NtFromError(info.error().code);
    FillFileInfo(state, info.value(), file_info);
    return STATUS_SUCCESS;
  }
  auto target = ResolveParentAndName(state, file_name);
  if (!target.ok()) {
    XFS_DEBUG("Open('" << WideToUtf8(file_name)
                       << "') failed: " << target.error().Format());
    return NtFromError(target.error().code);
  }
  const uint64_t parent_ino = target.value().first;
  const std::string& name = target.value().second;
  auto entry = state.fs->LookupName(parent_ino, name);
  if (!entry.ok()) return NtFromError(entry.error().code);
  auto inode = state.fs->OpenInode(entry.value().ino);
  if (!inode.ok()) return NtFromError(inode.error().code);
  if ((create_options & FILE_DIRECTORY_FILE) && !inode.value().IsDir()) {
    return STATUS_NOT_A_DIRECTORY;
  }
  if ((create_options & FILE_NON_DIRECTORY_FILE) && inode.value().IsDir()) {
    return STATUS_FILE_IS_A_DIRECTORY;
  }
  // FILE_OVERWRITE / FILE_OVERWRITE_IF / FILE_SUPERSEDE on an existing file:
  // the FSD reports FILE_OVERWRITTEN based on this Open, so the truncation
  // (SetFileSize(0), one metadata transaction) happens here - exactly what
  // Windows then observes as the new end-of-file.
  if (IsOverwriteDisposition(create_options) && !inode.value().IsDir()) {
    if (!inode.value().IsRegular()) return STATUS_NOT_SUPPORTED;
    auto info = state.fs->SetFileSize(inode.value().ino, 0);
    if (!info.ok()) {
      XFS_DEBUG("Open(overwrite, ino=" << inode.value().ino
                                       << ") refused: "
                                       << info.error().Format());
      return NtFromError(info.error().code);
    }
    auto fresh = state.fs->OpenInode(inode.value().ino);
    if (!fresh.ok()) return NtFromError(fresh.error().code);
    inode = std::move(fresh);
  }
  auto context =
      TakeNode(state, std::move(inode.value()), parent_ino, name);
  if (!context.ok()) return NtFromError(context.error().code);
  *file_context = context.value();
  auto info = state.fs->GetFileInfo(entry.value().ino);
  if (!info.ok()) return NtFromError(info.error().code);
  FillFileInfo(state, info.value(), file_info);
  return STATUS_SUCCESS;
}

NTSTATUS Overwrite(FSP_FILE_SYSTEM* file_system, PVOID file_context, UINT32,
                   BOOLEAN, UINT64 allocation_size,
                   FSP_FSCTL_FILE_INFO* file_info) {
  AdapterState& state = *State(file_system);
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  auto node = LookupNode(state, file_context);
  if (!node) return STATUS_INVALID_HANDLE;
  if (node->inode->IsDir()) return STATUS_INVALID_DEVICE_REQUEST;
  if (!node->inode->IsRegular()) return STATUS_NOT_SUPPORTED;
  auto info = state.fs->SetFileSize(node->inode->ino, 0);
  if (!info.ok()) return NtFromError(info.error().code);
  if (allocation_size > 0) {
    Status pre = PreallocateHint(*state.fs, node->inode->ino, allocation_size);
    if (!pre.ok()) {
      XFS_WARN("Overwrite: preallocation of " << allocation_size
                                              << " bytes failed: "
                                              << pre.error().Format());
    }
  }
  RefreshNode(state, NodeId(file_context));
  auto refreshed = LookupNode(state, file_context);
  if (refreshed) FillFileInfo(state, *refreshed->inode, file_info);
  return STATUS_SUCCESS;
}

VOID Cleanup(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
             ULONG flags) {
  CountCallback(PerfStats::Get().fs_cleanup_calls);
  AdapterState& state = *State(file_system);
  if (!state.read_write) return;
  auto node = LookupNodeState(state, file_context);
  if (!node) return;
  uint64_t ino = 0;
  bool is_dir = false;
  bool delete_pending = false;
  uint64_t parent_ino = 0;
  std::string name;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    ino = node->inode->ino;
    is_dir = node->inode->IsDir();
    delete_pending = node->delete_pending;
    parent_ino = node->parent_ino;
    name = node->xfs_name;
  }
  if (0 == (flags & FspCleanupDelete)) {
    // Normal close: publish the deferred mtime/ctime. The timestamp is
    // already visible through the cached inode since the write; this is the
    // transaction that makes it durable at the ordinary close boundary.
    Status published = state.fs->CommitDeferredMetadata(ino);
    if (!published.ok()) {
      XFS_ERROR("Cleanup: deferred timestamp commit failed: "
                << published.error().Format());
    }
    return;
  }
  if (delete_pending) return;
  // The FSD sends FileName with a delete request. The namespace removal is
  // one transaction; the inode stays on the AGI unlinked list until the last
  // Close, so open handles keep working (xfs_remove + xfs_inactive).
  if (file_name != nullptr && *file_name != L'\0') {
    auto resolved = ResolveParentAndName(state, file_name);
    if (resolved.ok()) {
      parent_ino = resolved.value().first;
      name = resolved.value().second;
    }
  }
  Status status;
  if (is_dir) {
    status = state.fs->RemoveEmptyDirectory(parent_ino, name,
                                            /*defer_inode_free=*/false);
  } else {
    status = state.fs->Unlink(parent_ino, name, /*defer_inode_free=*/true);
  }
  if (!status.ok()) {
    // CanDelete / SetDelete validated this beforehand; a failure here cannot
    // be reported through the WinFsp interface.
    XFS_ERROR("Cleanup(delete, ino=" << ino
                                     << ") failed: " << status.error().Format());
    return;
  }
  // A file only needs the deferred free when this was its last link; with
  // other hard links the inode stays fully reachable and nothing is yours to
  // release here.
  bool deferred = false;
  if (!is_dir) {
    auto fresh = state.fs->OpenInode(ino);
    deferred = fresh.ok() && fresh.value().nlink == 0;
  }
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto it = state.nodes.find(NodeId(file_context));
    if (it != state.nodes.end() && it->second == node) {
      node->delete_pending = deferred;
    }
  }
  DropNamespaceCaches(state, parent_ino);
  DropPathCache(state, file_name ? std::wstring(file_name) : std::wstring(),
                is_dir);
  XFS_DEBUG("deleted " << name << " in " << parent_ino
                       << (is_dir ? " (directory)" : "") << "; inode " << ino
                       << (deferred ? " freed at last close" : " freed"));
}

VOID Close(FSP_FILE_SYSTEM* file_system, PVOID file_context) {
  CountCallback(PerfStats::Get().fs_close_calls);
  AdapterState& state = *State(file_system);
  if (!file_context) return;
  // One reference per Open. The node is destroyed when the last one goes away
  // (idempotent for repeated Close of the same node). A delete-on-close node
  // frees its inode here: xfs_ifree for the zero-link inode that Cleanup left
  // on the AGI unlinked list.
  uint64_t free_ino = 0;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto it = state.nodes.find(NodeId(file_context));
    if (it == state.nodes.end()) return;
    if (0 == --it->second->refcount) {
      if (it->second->delete_pending) free_ino = it->second->inode->ino;
      state.node_by_inode.erase(it->second->inode->ino);
      state.nodes.erase(it);
    }
  }
  if (free_ino != 0) {
    Status status = state.fs->FreeUnlinkedInode(free_ino);
    if (!status.ok()) {
      XFS_ERROR("Close: cannot free unlinked inode " << free_ino << ": "
                                                     << status.error().Format());
    }
    DropNamespaceCaches(state, 0);
  }
}

NTSTATUS Read(FSP_FILE_SYSTEM* file_system, PVOID file_context,
              PVOID buffer, UINT64 offset, ULONG length,
              PULONG bytes_transferred) {
  CountCallback(PerfStats::Get().fs_read_calls);
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  if (!node) {
    XFS_DEBUG("Read: stale file node " << NodeId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  if (node->inode->IsDir()) return STATUS_INVALID_DEVICE_REQUEST;
  auto read =
      ReadInode(state, *node->inode, node->extents.get(), offset, buffer, length);
  if (!read.ok()) return NtFromError(read.error().code);
  *bytes_transferred = static_cast<ULONG>(read.value());
  return STATUS_SUCCESS;
}

NTSTATUS Write(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer,
               UINT64 offset, ULONG length, BOOLEAN write_to_end,
               BOOLEAN /*constrained_io*/, PULONG bytes_transferred,
               FSP_FSCTL_FILE_INFO* file_info) {
  CountCallback(PerfStats::Get().fs_write_calls);
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  if (!node) {
    XFS_DEBUG("Write: stale file node " << NodeId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  if (bytes_transferred) *bytes_transferred = 0;
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  if (node->inode->IsDir()) return STATUS_INVALID_DEVICE_REQUEST;
  if (node->inode->IsSymlink()) {
    // v1 exposes the link target as file content; that is a read-only view.
    return STATUS_NOT_SUPPORTED;
  }
  if (length == 0) {
    FillFileInfo(state, *node->inode, file_info);
    return STATUS_SUCCESS;
  }

  // The core validates the range and may allocate, convert unwritten extents and
  // move EOF; everything below goes through one transaction per phase (see
  // XfsFilesystem::WriteFileData). Write-to-end (append handles) is resolved
  // inside the core under its mutation lock, so concurrent appends cannot lose
  // bytes.
  auto written = state.fs->WriteFileData(node->inode->ino, offset, buffer,
                                         length, write_to_end != FALSE,
                                         /*defer_timestamps=*/true);
  if (!written.ok()) {
    XFS_DEBUG("Write(ino=" << node->inode->ino << ", offset=" << offset
                           << ", length=" << length
                           << ") refused: " << written.error().Format());
    return NtFromError(written.error().code);
  }
  if (bytes_transferred) {
    *bytes_transferred = static_cast<ULONG>(written.value());
  }
  RefreshNode(state, NodeId(file_context));
  auto refreshed = LookupNode(state, file_context);
  if (refreshed) FillFileInfo(state, *refreshed->inode, file_info);
  return STATUS_SUCCESS;
}

NTSTATUS Flush(FSP_FILE_SYSTEM* file_system, PVOID, FSP_FSCTL_FILE_INFO*) {
  CountCallback(PerfStats::Get().fs_flush_calls);
  AdapterState& state = *State(file_system);
  // Flush means "make what we have written durable": commit every deferred
  // timestamp, wait for in-flight writes, then issue the device barrier. A
  // failure must be reported, never swallowed.
  Status status = state.fs->Flush();
  if (!status.ok()) {
    XFS_ERROR("flush failed: " << status.error().Format());
    // A flush failure is how a removed device usually surfaces first.
    NoteDeviceFailure();
    return STATUS_IO_DEVICE_ERROR;
  }
  return STATUS_SUCCESS;
}

NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                     FSP_FSCTL_FILE_INFO* file_info) {
  CountCallback(PerfStats::Get().fs_get_file_info_calls);
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  if (!node) {
    XFS_DEBUG("GetFileInfo: stale file node " << NodeId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  FillFileInfo(state, *node->inode, file_info);
  return STATUS_SUCCESS;
}

NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                      UINT32 file_attributes, UINT64 creation_time,
                      UINT64 last_access_time, UINT64 last_write_time,
                      UINT64 change_time, FSP_FSCTL_FILE_INFO* file_info) {
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  if (!node) return STATUS_INVALID_HANDLE;
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;

  auto current = state.fs->GetFileInfo(node->inode->ino);
  if (!current.ok()) return NtFromError(current.error().code);

  if (file_attributes != INVALID_FILE_ATTRIBUTES) {
    // POSIX mode is the authority on this volume. Windows' read-only bit maps
    // to the write bits; clearing it adds owner write, setting it removes all
    // write bits. Other DOS attributes (hidden/system/archive) are not stored
    // on XFS and are accepted as no-ops.
    const bool want_readonly =
        (file_attributes & FILE_ATTRIBUTE_READONLY) != 0;
    const bool is_readonly = (current.value().mode & 0222) == 0;
    if (want_readonly != is_readonly) {
      uint16_t mode = current.value().mode;
      if (want_readonly) {
        mode = static_cast<uint16_t>(mode & ~0222);
      } else {
        mode = static_cast<uint16_t>(mode | 0200);
      }
      auto changed_mode = state.fs->SetFileMode(node->inode->ino, mode);
      if (!changed_mode.ok()) return NtFromError(changed_mode.error().code);
      current = changed_mode;
    }
  }

  XfsTimestamp atime = current.value().atime;
  XfsTimestamp mtime = current.value().mtime;
  XfsTimestamp ctime = current.value().ctime;
  bool changed = false;
  if (creation_time != 0) {
    // Windows creation time maps to the XFS crtime. Updating it would add a
    // fourth timestamp field to the transaction helper; keep the original
    // value rather than silently moving ctime.
    XFS_DEBUG("SetBasicInfo: creation time change ignored for ino "
              << current.value().ino);
  }
  if (last_access_time != 0) {
    atime = FromFileTime(last_access_time);
    changed = true;
  }
  if (last_write_time != 0) {
    mtime = FromFileTime(last_write_time);
    changed = true;
  }
  if (change_time != 0) {
    ctime = FromFileTime(change_time);
    changed = true;
  }
  if (changed) {
    auto updated = state.fs->SetFileTimes(node->inode->ino, atime, mtime,
                                          ctime);
    if (!updated.ok()) return NtFromError(updated.error().code);
  }
  RefreshNode(state, NodeId(file_context));
  auto refreshed = LookupNode(state, file_context);
  if (refreshed) FillFileInfo(state, *refreshed->inode, file_info);
  return STATUS_SUCCESS;
}

NTSTATUS SetFileSize(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                     UINT64 new_size, BOOLEAN set_allocation_size,
                     FSP_FSCTL_FILE_INFO* file_info) {
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  if (!node) return STATUS_INVALID_HANDLE;
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  if (node->inode->IsDir()) return STATUS_INVALID_DEVICE_REQUEST;
  if (set_allocation_size) {
    // Windows maintains allocation size separately from EOF. The WinFsp
    // contract says an allocation size below EOF truncates the file; a larger
    // one extends allocation without moving EOF (unwritten extents).
    if (new_size < node->inode->size) {
      auto truncated = state.fs->SetFileSize(node->inode->ino, new_size);
      if (!truncated.ok()) return NtFromError(truncated.error().code);
    } else {
      const uint64_t block_size = state.fs->geometry().block_size;
      const uint64_t allocated =
          block_size == 0 ? 0 : node->inode->nblocks * block_size;
      if (new_size > allocated) {
        Status pre = PreallocateHint(*state.fs, node->inode->ino, new_size);
        if (!pre.ok()) {
          XFS_DEBUG("SetAllocationSize(ino=" << node->inode->ino
                                             << ", size=" << new_size
                                             << ") incomplete: "
                                             << pre.error().Format());
        }
      }
    }
    RefreshNode(state, NodeId(file_context));
    auto updated = LookupNode(state, file_context);
    if (updated) FillFileInfo(state, *updated->inode, file_info);
    return STATUS_SUCCESS;
  }
  auto info = state.fs->SetFileSize(node->inode->ino, new_size);
  if (!info.ok()) {
    XFS_DEBUG("SetFileSize(ino=" << node->inode->ino << ", size=" << new_size
                                 << ") refused: " << info.error().Format());
    return NtFromError(info.error().code);
  }
  RefreshNode(state, NodeId(file_context));
  auto refreshed = LookupNode(state, file_context);
  if (refreshed) FillFileInfo(state, *refreshed->inode, file_info);
  return STATUS_SUCCESS;
}

NTSTATUS SetDelete(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR,
                   BOOLEAN delete_file) {
  AdapterState& state = *State(file_system);
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  auto node = LookupNode(state, file_context);
  if (!node) return STATUS_INVALID_HANDLE;
  if (!delete_file) return STATUS_SUCCESS;  // clearing the disposition
  // The actual deletion happens in Cleanup; this is the read-only test that
  // Windows performs first (like xfs_remove's checks, before any mutation).
  return NtFromStatus(CheckDeletable(state, *node->inode));
}

NTSTATUS CanDelete(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR) {
  // Used only when SetDelete is not implemented; kept consistent with it.
  AdapterState& state = *State(file_system);
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  auto node = LookupNode(state, file_context);
  if (!node) return STATUS_INVALID_HANDLE;
  return NtFromStatus(CheckDeletable(state, *node->inode));
}

NTSTATUS Rename(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
                PWSTR new_file_name, BOOLEAN replace_if_exists) {
  CountCallback(PerfStats::Get().fs_rename_calls);
  AdapterState& state = *State(file_system);
  if (!state.read_write) return STATUS_MEDIA_WRITE_PROTECTED;
  // The FSD hands over both names; the core owns the whole atomic operation
  // (never DirRemove + DirInsert from here).
  auto src = ResolveParentAndName(state, file_name);
  if (!src.ok()) return NtFromError(src.error().code);
  auto dst = ResolveParentAndName(state, new_file_name);
  if (!dst.ok()) return NtFromError(dst.error().code);

  // The replaced target may still have open handles: keep its inode alive
  // (AGI unlinked list) and let the last Close free it.
  auto outcome = state.fs->Rename(src.value().first, src.value().second,
                                  dst.value().first, dst.value().second,
                                  replace_if_exists != FALSE,
                                  /*defer_replaced_free=*/true);
  if (!outcome.ok()) {
    XFS_DEBUG("Rename('" << WideToUtf8(file_name) << "' -> '"
                         << WideToUtf8(new_file_name)
                         << "') failed: " << outcome.error().Format());
    return NtFromError(outcome.error().code);
  }

  uint64_t free_ino = 0;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    // An open handle on the moved inode keeps following it under the new
    // name; a pending delete-on-close on it now refers to the new name.
    if (file_context != nullptr) {
      auto it = state.nodes.find(NodeId(file_context));
      if (it != state.nodes.end() &&
          it->second->inode->ino == outcome.value().moved_ino) {
        it->second->parent_ino = dst.value().first;
        it->second->xfs_name = dst.value().second;
      }
    }
    if (outcome.value().replaced_zero_link) {
      auto it = state.node_by_inode.find(outcome.value().replaced_ino);
      if (it == state.node_by_inode.end()) {
        free_ino = outcome.value().replaced_ino;
      } else {
        // The old target stays valid through its handles; its namespace entry
        // is gone, so Cleanup must not unlink anything and the last Close
        // frees the inode (exactly the open-but-unlinked state).
        state.nodes[it->second]->delete_pending = true;
      }
    }
  }
  if (free_ino != 0) {
    Status freed = state.fs->FreeUnlinkedInode(free_ino);
    if (!freed.ok()) {
      XFS_ERROR("Rename: cannot free the replaced inode " << free_ino << ": "
                                                          << freed.error().Format());
    }
  }
  DropNamespaceCaches(state, src.value().first, dst.value().first);
  auto moved_inode = state.fs->OpenInode(outcome.value().moved_ino);
  const bool moved_directory =
      moved_inode.ok() && moved_inode.value().IsDir();
  DropPathCache(state, file_name ? std::wstring(file_name) : std::wstring(),
                moved_directory);
  DropPathCache(state, new_file_name ? std::wstring(new_file_name)
                                     : std::wstring(),
                moved_directory);
  XFS_DEBUG("renamed '" << WideToUtf8(file_name) << "' -> '"
                        << WideToUtf8(new_file_name) << "' (moved ino "
                        << outcome.value().moved_ino << ", replaced ino "
                        << outcome.value().replaced_ino << ")");
  return STATUS_SUCCESS;
}

NTSTATUS GetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                     PSECURITY_DESCRIPTOR security_descriptor,
                     SIZE_T* security_descriptor_size) {
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  const uint16_t mode = node ? node->inode->mode : static_cast<uint16_t>(0100644);
  if (NeedsSecurityOverflow(state, security_descriptor_size)) {
    return STATUS_BUFFER_OVERFLOW;
  }
  FillSecurity(state, security_descriptor, security_descriptor_size, nullptr,
               FileAttributesFor(state, mode));
  return STATUS_SUCCESS;
}

NTSTATUS SetSecurity(FSP_FILE_SYSTEM* file_system, PVOID, SECURITY_INFORMATION,
                     PSECURITY_DESCRIPTOR) {
  // POSIX modes are not translated into Windows ACLs; changing a security
  // descriptor would require a metadata transaction.
  return NotImplementedWrite(*State(file_system));
}

bool AddOneDirectoryEntry(FSP_FILE_SYSTEM* file_system,
                          const std::u16string& name,
                          const FSP_FSCTL_FILE_INFO& file_info, PVOID buffer,
                          ULONG length, PULONG bytes_transferred) {
  if (name.size() >= 256) return TRUE;  // skip unrepresentable long names
  std::vector<uint8_t> entry(sizeof(FSP_FSCTL_DIR_INFO) +
                             name.size() * sizeof(char16_t));
  std::memset(entry.data(), 0, entry.size());
  auto* info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(entry.data());
  info->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) +
                                   name.size() * sizeof(char16_t));
  info->FileInfo = file_info;
  std::memcpy(info->FileNameBuf, name.data(), name.size() * sizeof(char16_t));
  return FspFileSystemAddDirInfo(info, buffer, length, bytes_transferred) !=
         FALSE;
}

NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                       PWSTR /*pattern*/, PWSTR marker, PVOID buffer,
                       ULONG length, PULONG bytes_transferred) {
  CountCallback(PerfStats::Get().fs_read_directory_calls);
  AdapterState& state = *State(file_system);
  auto context = LookupNode(state, file_context);
  if (!context) {
    XFS_DEBUG("ReadDirectory: stale file node " << NodeId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  if (!context->inode->IsDir()) return STATUS_NOT_A_DIRECTORY;
  *bytes_transferred = 0;

  // Sorted Windows-name children, built once per directory and reused for
  // every page (the FSD calls again with a marker per page) and for repeated
  // Explorer enumerations.
  std::shared_ptr<const std::vector<DirChild>> children;
  {
    std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
    auto it = state.dir_children.find(context->inode->ino);
    if (it != state.dir_children.end()) {
      children = it->second;
      state.dir_children_lru.remove(context->inode->ino);
      state.dir_children_lru.push_front(context->inode->ino);
    }
  }
  if (!children) {
    auto entries = state.fs->ListDirectory(context->inode->ino);
    if (!entries.ok()) return NtFromError(entries.error().code);
    auto built = std::make_shared<std::vector<DirChild>>();
    built->reserve(entries.value().size());
    for (const auto& entry : entries.value()) {
      if (entry.name == "." || entry.name == "..") continue;
      WindowsName win = XfsNameToWindows(entry.name);
      if (win.too_long) {
        XFS_WARN("directory entry '" << entry.name
                                     << "' is too long for Windows; skipped");
        continue;
      }
      built->push_back({std::move(win.name), entry.ino});
    }
    std::sort(built->begin(), built->end(),
              [](const DirChild& a, const DirChild& b) {
                return a.name < b.name;
              });
    {
      std::lock_guard<xfs::conc::Mutex> lock(state.node_mutex);
      constexpr size_t kMaxCachedDirs = 64;
      constexpr size_t kMaxCachedEntries = 262144;
      if (built->size() <= kMaxCachedEntries) {
        while (!state.dir_children.empty() &&
               (state.dir_children.size() >= kMaxCachedDirs ||
                state.dir_children_entries + built->size() >
                    kMaxCachedEntries)) {
          const uint64_t victim = state.dir_children_lru.back();
          state.dir_children_lru.pop_back();
          auto victim_it = state.dir_children.find(victim);
          if (victim_it != state.dir_children.end()) {
            state.dir_children_entries -= victim_it->second->size();
            state.dir_children.erase(victim_it);
          }
        }
        auto existing = state.dir_children.find(context->inode->ino);
        if (existing != state.dir_children.end()) {
          state.dir_children_entries -= existing->second->size();
          state.dir_children.erase(existing);
          state.dir_children_lru.remove(context->inode->ino);
        }
        state.dir_children_entries += built->size();
        state.dir_children.emplace(context->inode->ino, built);
        state.dir_children_lru.push_front(context->inode->ino);
      }
    }
    children = std::move(built);
  }

  // Fixed ordering: ".", "..", then children sorted by Windows name. The
  // marker is the last name returned by the previous call.
  std::u16string marker_name = marker ? ToU16(std::wstring(marker))
                                      : std::u16string();
  bool marker_seen = marker == nullptr;
  NTSTATUS stop_status = STATUS_SUCCESS;

  // Returns false when this page is done (full, or the device failed); the
  // caller then returns stop_status.  A single unreadable inode is skipped
  // with a warning, but a terminally failed device must not be reported as a
  // successful partial listing.
  auto add_entry = [&](uint64_t ino, const std::u16string& name) -> bool {
    auto file_info = state.fs->GetFileInfo(ino);
    if (!file_info.ok()) {
      NoteDeviceFailure();
      if (state.device_failed.load(std::memory_order_relaxed)) {
        XFS_ERROR("cannot enumerate inode " << ino
                                            << " after device failure: "
                                            << file_info.error().Format());
        stop_status = NtFromError(file_info.error().code);
        return false;
      }
      XFS_WARN("cannot open inode " << ino << ": "
                                    << file_info.error().Format());
      return true;  // skip this entry, keep enumerating
    }
    FSP_FSCTL_FILE_INFO out;
    FillFileInfo(state, file_info.value(), &out);
    if (!AddOneDirectoryEntry(file_system, name, out, buffer, length,
                              bytes_transferred)) {
      stop_status = STATUS_SUCCESS;  // page full; the FSD asks again
      return false;
    }
    return true;
  };

  // Dot entries (only for non-root directories; the FSD handles root ".").
  if (context->inode->ino != state.fs->RootInode()) {
    uint64_t parent_ino = 0;
    bool have_parent = false;
    for (bool parent_entry : {false, true}) {
      const std::u16string wname = parent_entry ? u".." : u".";
      if (!marker_seen) {
        if (wname == marker_name) marker_seen = true;
        continue;
      }
      if (parent_entry) {
        if (!have_parent) {
          auto parent = state.fs->LookupName(context->inode->ino, "..");
          if (!parent.ok()) {
            NoteDeviceFailure();
            if (state.device_failed.load(std::memory_order_relaxed)) {
              return NtFromError(parent.error().code);
            }
            continue;
          }
          parent_ino = parent.value().ino;
          have_parent = true;
        }
        if (!add_entry(parent_ino, wname)) return stop_status;
      } else {
        if (!add_entry(context->inode->ino, wname)) return stop_status;
      }
    }
  }

  // Children: when the marker was a dot entry start at the beginning,
  // otherwise resume strictly after the marker name (WinFsp semantics).
  auto begin = children->begin();
  if (!marker_seen) {
    begin = std::upper_bound(
        children->begin(), children->end(), marker_name,
        [](const std::u16string& value, const DirChild& child) {
          return value < child.name;
        });
  }
  for (auto it = begin; it != children->end(); ++it) {
    if (!add_entry(it->ino, it->name)) return stop_status;
  }

  // End-of-directory marker: tells the FSD the enumeration is complete (see
  // FspFileSystemAddDirInfo with a NULL DirInfo).
  FspFileSystemAddDirInfo(nullptr, buffer, length, bytes_transferred);
  return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Mount lifetime
// ---------------------------------------------------------------------------

BOOL WINAPI ConsoleCtrlHandler(DWORD) {
  if (g_stop_event) SetEvent(g_stop_event);
  return TRUE;
}

std::wstring NormalizeMountPoint(const std::wstring& mount_point) {
  std::wstring s = mount_point;
  while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

// Named events live in Local\ and must be stable between mount and unmount but
// unique per mount point. Drive letters and directories both map to a hash.
std::wstring StopEventName(const std::wstring& mount_point) {
  const std::wstring s = NormalizeMountPoint(mount_point);
  uint64_t h = 1469598103934665603ull;  // FNV-1a 64-bit
  for (wchar_t c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  wchar_t buf[48] = {};
  swprintf_s(buf, L"Local\\xfswin.mount.%016llX",
             static_cast<unsigned long long>(h));
  return buf;
}

bool IsDriveLetterLike(const std::wstring& drive) {
  if (drive.size() != 2 && !(drive.size() == 3 && drive[2] == L'\\')) {
    return false;
  }
  wchar_t c = drive[0];
  return ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) &&
         drive[1] == L':';
}

// Absolute directory mount point such as E:\XFS (WinFsp junctions).
bool IsDirectoryMountPoint(const std::wstring& mount_point) {
  if (mount_point.size() < 4) return false;
  const wchar_t c = mount_point[0];
  const bool letter = ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'));
  return letter && mount_point[1] == L':' &&
         (mount_point[2] == L'\\' || mount_point[2] == L'/');
}

}  // namespace

bool IsValidDriveLetter(const std::wstring& drive) {
  return IsDriveLetterLike(drive);
}

int RunAdapterSelfTest(std::unique_ptr<XfsFilesystem> filesystem) {
  if (!filesystem) {
    std::fprintf(stderr, "selftest: no filesystem\n");
    return 1;
  }
  const bool read_write = filesystem->writable();
  int failures = 0;
  auto check = [&](bool cond, const char* what) {
    if (!cond) {
      std::fprintf(stderr, "SELFTEST FAIL: %s\n", what);
      ++failures;
    }
    return cond;
  };

  AdapterState state;
  state.fs = std::move(filesystem);
  state.read_write = read_write;
  const XfsGeometry& geo = state.fs->geometry();
  const XfsSuperblock& sb = state.fs->superblock();
  state.sector_size = 512;
  state.sectors_per_allocation_unit =
      static_cast<uint16_t>(std::max<uint32_t>(1, geo.block_size / 512));
  state.label = Utf8ToUtf16(sb.label.data());
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:BAG:BAD:(A;;FR;;;WD)", SDDL_REVISION_1,
          &state.security_descriptor, &state.security_descriptor_size)) {
    std::fprintf(stderr, "selftest: security descriptor failed\n");
    return 1;
  }

  FSP_FILE_SYSTEM fake{};
  fake.UserContext = &state;
  // Plain opens carry the packed FILE_OPEN disposition (the FSD
  // packs the disposition into the high 8 bits; 0 would mean
  // FILE_SUPERSEDE and truncate the file).
  const UINT32 kFileOpen = 1u << 24;

  const wchar_t* dirs[] = {L"\\", L"\\folder", L"\\fragdir"};
  const wchar_t* files[] = {L"\\hello.txt", L"\\large.bin", L"\\sparse.img",
                            L"\\folder\\link", L"\\hello.bin", L"\\big.bin"};
  std::vector<uint8_t> dir_buffer(64 * 1024);
  std::vector<uint8_t> read_buffer(128 * 1024);
  std::vector<uint8_t> sd_buffer(1024);
  std::wstring marker;
  // The self-test drives callbacks; it does not verify image contents (the
  // mount script does that with the manifest). Names that do not exist in a
  // given image are skipped, but at least one directory and one file must
  // have been exercised.
  size_t opened_dirs = 0;
  size_t opened_files = 0;

  auto enumerate = [&](PVOID ctx, const char* what) {
    size_t total = 0;
    marker.clear();
    bool first = true;
    for (int guard = 0; guard < 10000; ++guard) {
      ULONG transferred = 0;
      NTSTATUS st = ReadDirectory(
          &fake, ctx, nullptr,
          first ? nullptr : const_cast<PWSTR>(marker.c_str()), dir_buffer.data(),
          static_cast<ULONG>(dir_buffer.size()), &transferred);
      if (!check(st == STATUS_SUCCESS, "ReadDirectory status")) break;
      if (transferred == 0) break;
      size_t pos = 0;
      std::wstring last;
      bool end_marker = false;
      while (pos + 2 <= transferred) {
        uint16_t rec_size =
            *reinterpret_cast<const uint16_t*>(dir_buffer.data() + pos);
        if (rec_size == 0) {
          end_marker = true;
          break;
        }
        if (rec_size < sizeof(FSP_FSCTL_DIR_INFO) ||
            pos + rec_size > transferred) {
          check(false, "directory record size out of range");
          break;
        }
        const auto* di =
            reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(dir_buffer.data() +
                                                        pos);
        size_t chars =
            (rec_size - sizeof(FSP_FSCTL_DIR_INFO)) / sizeof(char16_t);
        last.assign(di->FileNameBuf, di->FileNameBuf + chars);
        pos += (rec_size + 7u) & ~size_t{7u};
        ++total;
      }
      if (end_marker) break;
      if (last.empty()) {
        check(false, "no marker progress");
        break;
      }
      marker = last;
      first = false;
    }
    std::fprintf(stderr, "selftest: %s -> %zu entries\n", what, total);
  };

  const int kIterations = 20;
  for (int iter = 0; iter < kIterations; ++iter) {
    FSP_FSCTL_VOLUME_INFO vi{};
    check(GetVolumeInfo(&fake, &vi) == STATUS_SUCCESS, "GetVolumeInfo");
    check(vi.TotalSize == geo.dblocks * geo.block_size, "volume size");

    for (const wchar_t* dir : dirs) {
      UINT32 attrs = 0;
      NTSTATUS sec = GetSecurityByName(&fake, const_cast<PWSTR>(dir), &attrs,
                                       nullptr, nullptr);
      if (sec == STATUS_OBJECT_NAME_NOT_FOUND) continue;
      check(sec == STATUS_SUCCESS, "GetSecurityByName attrs");
      SIZE_T sd_size = sd_buffer.size();
      check(GetSecurityByName(&fake, const_cast<PWSTR>(dir), &attrs,
                              sd_buffer.data(), &sd_size) == STATUS_SUCCESS,
            "GetSecurityByName sd");

      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      if (!check(Open(&fake, const_cast<PWSTR>(dir), FILE_DIRECTORY_FILE | kFileOpen, 0,
                      &ctx, &fi) == STATUS_SUCCESS,
                 "Open directory")) {
        continue;
      }
      ++opened_dirs;
      check((fi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            "directory attribute");
      enumerate(ctx, "dir");
      check(GetFileInfo(&fake, ctx, &fi) == STATUS_SUCCESS, "GetFileInfo dir");
      Close(&fake, ctx);
    }

    for (const wchar_t* file : files) {
      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      NTSTATUS opened = Open(&fake, const_cast<PWSTR>(file), kFileOpen, 0, &ctx, &fi);
      if (opened == STATUS_OBJECT_NAME_NOT_FOUND) continue;
      if (!check(opened == STATUS_SUCCESS, "Open file")) continue;
      ++opened_files;
      ULONG transferred = 0;
      check(Read(&fake, ctx, read_buffer.data(), 0,
                 static_cast<ULONG>(read_buffer.size()),
                 &transferred) == STATUS_SUCCESS,
            "Read file");
      check(GetFileInfo(&fake, ctx, &fi) == STATUS_SUCCESS, "GetFileInfo file");
      Close(&fake, ctx);
    }

    if (iter == 0) {
      check(opened_dirs >= 1, "at least one directory was opened");
      check(opened_files >= 1, "at least one file was read");
      std::fprintf(stderr, "selftest: exercised %zu directories, %zu files\n",
                   opened_dirs, opened_files);
    }

    if (iter < 3) {
      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      NTSTATUS opened = Open(&fake, const_cast<PWSTR>(L"\\bigdir"),
                             FILE_DIRECTORY_FILE | kFileOpen, 0, &ctx, &fi);
      if (opened == STATUS_OBJECT_NAME_NOT_FOUND) {
        // Not every image has the large directory; skip it.
      } else if (check(opened == STATUS_SUCCESS, "Open bigdir")) {
        enumerate(ctx, "bigdir");
        Close(&fake, ctx);
      }
    }

    check(HeapValidate(GetProcessHeap(), 0, nullptr), "heap valid");
    if (failures) break;
  }

  // Read-write self-test: overwrite bytes inside an existing file through the
  // real callbacks and verify them (this is the driver-free coverage of the
  // Write/Flush path).
  if (read_write && !failures) {
    const wchar_t* candidates[] = {L"\\hello.bin", L"\\hello.txt",
                                   L"\\folder\\test.bin", L"\\large.bin",
                                   L"\\big.bin"};
    for (const wchar_t* candidate : candidates) {
      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      if (Open(&fake, const_cast<PWSTR>(candidate), kFileOpen, 0, &ctx, &fi) !=
          STATUS_SUCCESS) {
        continue;
      }
      if (fi.FileSize < 8192 || fi.FileAttributes & FILE_ATTRIBUTE_READONLY) {
        Close(&fake, ctx);
        continue;
      }
      std::vector<uint8_t> before(static_cast<size_t>(fi.FileSize));
      ULONG transferred = 0;
      if (Read(&fake, ctx, before.data(), 0,
               static_cast<ULONG>(before.size()), &transferred) !=
              STATUS_SUCCESS ||
          transferred != before.size()) {
        check(false, "rw selftest: cannot read the whole file");
        Close(&fake, ctx);
        break;
      }
      // Overwrite the middle of the first block, inside an allocated extent.
      const UINT64 write_offset = 1024;
      std::vector<uint8_t> overlay(512);
      for (size_t i = 0; i < overlay.size(); ++i) {
        overlay[i] = static_cast<uint8_t>(0x30 + (i & 0x3F));
      }
      ULONG written = 0;
      NTSTATUS status =
          Write(&fake, ctx, overlay.data(), write_offset,
                static_cast<ULONG>(overlay.size()), FALSE, FALSE, &written,
                &fi);
      if (!check(status == STATUS_SUCCESS, "rw selftest: Write") ||
          !check(written == overlay.size(), "rw selftest: Write length")) {
        Close(&fake, ctx);
        break;
      }
      check(Flush(&fake, ctx, &fi) == STATUS_SUCCESS, "rw selftest: Flush");

      // RW-2: writing past EOF grows the file and allocates blocks.
      ULONG grown = 0;
      std::vector<uint8_t> tail(64, 0x5A);
      NTSTATUS beyond =
          Write(&fake, ctx, tail.data(), before.size(), 64, FALSE, FALSE,
                &grown, &fi);
      check(beyond == STATUS_SUCCESS, "rw selftest: write past EOF");
      check(grown == 64, "rw selftest: grown write length");
      check(fi.FileSize == before.size() + 64, "rw selftest: grown size");

      // ... and SetFileSize shrinks it again (freeing the new blocks).
      check(SetFileSize(&fake, ctx, before.size(), FALSE, &fi) == STATUS_SUCCESS,
            "rw selftest: SetFileSize shrink");
      check(fi.FileSize == before.size(), "rw selftest: shrunk size");
      // Growing only moves the EOF: the new range reads as zeros.
      check(SetFileSize(&fake, ctx, before.size() + 8192, FALSE, &fi) ==
                STATUS_SUCCESS,
            "rw selftest: SetFileSize grow");
      check(fi.FileSize == before.size() + 8192, "rw selftest: grown size 2");
      check(SetFileSize(&fake, ctx, before.size(), FALSE, &fi) == STATUS_SUCCESS,
            "rw selftest: SetFileSize shrink 2");

      // Re-open (fresh node: re-reads inode and extents) and compare.
      Close(&fake, ctx);
      ctx = nullptr;
      if (!check(Open(&fake, const_cast<PWSTR>(candidate), kFileOpen, 0, &ctx, &fi) ==
                     STATUS_SUCCESS,
                 "rw selftest: re-open")) {
        break;
      }
      std::vector<uint8_t> after(static_cast<size_t>(fi.FileSize));
      transferred = 0;
      if (!check(Read(&fake, ctx, after.data(), 0,
                      static_cast<ULONG>(after.size()), &transferred) ==
                     STATUS_SUCCESS,
                 "rw selftest: re-read")) {
        Close(&fake, ctx);
        break;
      }
      bool mismatch = after.size() != before.size();
      for (size_t i = 0; !mismatch && i < after.size(); ++i) {
        const uint8_t expected =
            (i >= write_offset && i < write_offset + overlay.size())
                ? overlay[i - write_offset]
                : before[i];
        mismatch = after[i] != expected;
      }
      check(!mismatch, "rw selftest: written bytes did not read back");
      if (!mismatch) {
        std::fprintf(stderr,
                     "selftest: read-write overlay at offset %llu in %ls "
                     "verified through the Write callback\n",
                     static_cast<unsigned long long>(write_offset), candidate);
      }
      Close(&fake, ctx);
      break;
    }
  }

  // RW-3 self-test: create, mkdir, delete-on-close and rmdir through the same
  // callbacks WinFsp drives. This is the driver-free coverage of the inode
  // allocator, the directory writer and the open-but-unlinked state.
  if (read_write && !failures) {
    auto options_for = [](UINT32 disposition, UINT32 options) {
      // Disposition in the high 8 bits, options in the low 24 (fsctl.h).
      return (disposition << 24) | options;
    };
    constexpr UINT32 kFileCreate = 2;  // FILE_CREATE
    PVOID ctx = nullptr;
    FSP_FSCTL_FILE_INFO fi{};
    NTSTATUS st = Create(&fake, const_cast<PWSTR>(L"\\rw3-created.bin"),
                         options_for(kFileCreate, 0), 0, 0, nullptr, 0, &ctx,
                         &fi);
    check(st == STATUS_SUCCESS, "rw3 selftest: Create file");
    if (st == STATUS_SUCCESS) {
      check(fi.FileSize == 0, "rw3 selftest: new file size");
      std::vector<uint8_t> payload(4096, 0xA5);
      ULONG written = 0;
      check(Write(&fake, ctx, payload.data(), 0,
                  static_cast<ULONG>(payload.size()), FALSE, FALSE, &written,
                  &fi) == STATUS_SUCCESS &&
                written == payload.size(),
            "rw3 selftest: write new file");
      Close(&fake, ctx);
      ctx = nullptr;
      check(Open(&fake, const_cast<PWSTR>(L"\\rw3-created.bin"), kFileOpen, 0, &ctx,
                 &fi) == STATUS_SUCCESS,
            "rw3 selftest: re-open created file");
      if (ctx) {
        std::vector<uint8_t> readback(payload.size(), 0);
        ULONG got = 0;
        check(Read(&fake, ctx, readback.data(), 0,
                   static_cast<ULONG>(readback.size()), &got) == STATUS_SUCCESS &&
                  got == readback.size() && readback == payload,
              "rw3 selftest: created-file contents");
        Close(&fake, ctx);
      }
      // A duplicate create collides (the FSD tries Open first in practice).
      PVOID dup = nullptr;
      check(Create(&fake, const_cast<PWSTR>(L"\\rw3-created.bin"),
                   options_for(kFileCreate, 0), 0, 0, nullptr, 0, &dup,
                   &fi) == STATUS_OBJECT_NAME_COLLISION,
            "rw3 selftest: duplicate create collides");

      // mkdir, enumerate it while empty, then create a child and enumerate
      // again: the parent listing cache must have been dropped.
      ctx = nullptr;
      st = Create(&fake, const_cast<PWSTR>(L"\\rw3-dir"),
                  options_for(kFileCreate, FILE_DIRECTORY_FILE), 0, 0, nullptr,
                  0, &ctx, &fi);
      check(st == STATUS_SUCCESS, "rw3 selftest: mkdir");
      if (ctx) {
        check((fi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
              "rw3 selftest: mkdir attribute");
        enumerate(ctx, "rw3 empty dir");
        check(SetDelete(&fake, ctx, const_cast<PWSTR>(L"\\rw3-dir"), TRUE) ==
                  STATUS_SUCCESS,
              "rw3 selftest: empty dir deletable");
        Close(&fake, ctx);
      }
      PVOID child = nullptr;
      st = Create(&fake, const_cast<PWSTR>(L"\\rw3-dir\\child.bin"),
                  options_for(kFileCreate, 0), 0, 0, nullptr, 0, &child, &fi);
      check(st == STATUS_SUCCESS, "rw3 selftest: create child");
      if (child) {
        std::vector<uint8_t> child_data(1024, 0x5C);
        ULONG written = 0;
        check(Write(&fake, child, child_data.data(), 0,
                    static_cast<ULONG>(child_data.size()), FALSE, FALSE,
                    &written, &fi) == STATUS_SUCCESS,
              "rw3 selftest: write child");
        check(GetFileInfo(&fake, child, &fi) == STATUS_SUCCESS &&
                  fi.FileSize == child_data.size(),
              "rw3 selftest: child size");
        Close(&fake, child);
      }
      // The directory is no longer empty: the delete disposition is refused
      // without modifying anything.
      ctx = nullptr;
      if (Open(&fake, const_cast<PWSTR>(L"\\rw3-dir"), FILE_DIRECTORY_FILE | kFileOpen, 0,
               &ctx, &fi) == STATUS_SUCCESS) {
        enumerate(ctx, "rw3 dir with child");
        check(SetDelete(&fake, ctx, const_cast<PWSTR>(L"\\rw3-dir"), TRUE) ==
                  STATUS_DIRECTORY_NOT_EMPTY,
              "rw3 selftest: non-empty rmdir refused");
        Close(&fake, ctx);
      }

      // Delete-on-close: Cleanup removes the namespace entry (the inode goes
      // on the AGI unlinked list), the handle keeps working until Close, and
      // Close frees the inode.
      child = nullptr;
      if (Open(&fake, const_cast<PWSTR>(L"\\rw3-dir\\child.bin"), kFileOpen, 0, &child,
               &fi) == STATUS_SUCCESS) {
        check(SetDelete(&fake, child,
                        const_cast<PWSTR>(L"\\rw3-dir\\child.bin"),
                        TRUE) == STATUS_SUCCESS,
              "rw3 selftest: SetDelete child");
        Cleanup(&fake, child, const_cast<PWSTR>(L"\\rw3-dir\\child.bin"),
                FspCleanupDelete);
        std::vector<uint8_t> readback(1024, 0);
        ULONG got = 0;
        check(Read(&fake, child, readback.data(), 0,
                   static_cast<ULONG>(readback.size()), &got) == STATUS_SUCCESS,
              "rw3 selftest: read open-but-unlinked");
        check(GetFileInfo(&fake, child, &fi) == STATUS_SUCCESS,
              "rw3 selftest: info open-but-unlinked");
        Close(&fake, child);  // frees the unlinked inode
      }
      PVOID gone = nullptr;
      check(Open(&fake, const_cast<PWSTR>(L"\\rw3-dir\\child.bin"), kFileOpen, 0,
                 &gone, &fi) == STATUS_OBJECT_NAME_NOT_FOUND,
            "rw3 selftest: deleted child name gone");

      // Now the directory is empty and can be removed; its name disappears.
      ctx = nullptr;
      if (Open(&fake, const_cast<PWSTR>(L"\\rw3-dir"), FILE_DIRECTORY_FILE | kFileOpen, 0,
               &ctx, &fi) == STATUS_SUCCESS) {
        check(SetDelete(&fake, ctx, const_cast<PWSTR>(L"\\rw3-dir"), TRUE) ==
                  STATUS_SUCCESS,
              "rw3 selftest: emptied dir deletable");
        Cleanup(&fake, ctx, const_cast<PWSTR>(L"\\rw3-dir"), FspCleanupDelete);
        Close(&fake, ctx);
      }
      check(Open(&fake, const_cast<PWSTR>(L"\\rw3-dir"),
                 FILE_DIRECTORY_FILE | kFileOpen,
                 0, &gone, &fi) == STATUS_OBJECT_NAME_NOT_FOUND,
            "rw3 selftest: removed dir name gone");

      // Remove the first file the same way, then create the name again: names
      // and inodes are reusable.
      ctx = nullptr;
      if (Open(&fake, const_cast<PWSTR>(L"\\rw3-created.bin"), kFileOpen, 0, &ctx,
               &fi) == STATUS_SUCCESS) {
        check(SetDelete(&fake, ctx, const_cast<PWSTR>(L"\\rw3-created.bin"),
                        TRUE) == STATUS_SUCCESS,
              "rw3 selftest: SetDelete file");
        Cleanup(&fake, ctx, const_cast<PWSTR>(L"\\rw3-created.bin"),
                FspCleanupDelete);
        Close(&fake, ctx);
      }
      ctx = nullptr;
      check(Create(&fake, const_cast<PWSTR>(L"\\rw3-created.bin"),
                   options_for(kFileCreate, 0), 0, 0, nullptr, 0, &ctx,
                   &fi) == STATUS_SUCCESS,
            "rw3 selftest: recreate the same name");
      if (ctx) Close(&fake, ctx);
      std::fprintf(stderr,
                   "selftest: RW-3 namespace mutation verified through the "
                   "Create/SetDelete/Cleanup callbacks\n");
    } else {
      // A failed create leaves nothing behind; report and continue.
      if (ctx) Close(&fake, ctx);
    }
  }

  // RW-4 self-test: atomic rename, hard links and symlinks, including the
  // replace-an-open-target case. Rename goes through the real callback; hard
  // links and symlinks have no WinFsp file-system callback, so they drive the
  // core API the CLI uses (Windows sees the result through the namespace).
  if (read_write && !failures) {
    auto options_for = [](UINT32 disposition, UINT32 options) {
      return (disposition << 24) | options;
    };
    constexpr UINT32 kFileCreate = 2;  // FILE_CREATE
    PVOID ctx = nullptr;
    FSP_FSCTL_FILE_INFO fi{};
    std::vector<uint8_t> payload_a(4096);
    std::vector<uint8_t> payload_w(1024, 0x77);
    std::vector<uint8_t> payload_v(2048, 0x56);
    for (size_t i = 0; i < payload_a.size(); ++i) {
      payload_a[i] = static_cast<uint8_t>(i * 7 + 3);
    }

    // 1. Create /rw4-a.bin, then rename it to /rw4-b.bin.
    NTSTATUS st = Create(&fake, const_cast<PWSTR>(L"\\rw4-a.bin"),
                         options_for(kFileCreate, 0), 0, 0, nullptr, 0, &ctx,
                         &fi);
    check(st == STATUS_SUCCESS, "rw4 selftest: create rw4-a.bin");
    if (st == STATUS_SUCCESS) {
      ULONG written = 0;
      check(Write(&fake, ctx, payload_a.data(), 0,
                  static_cast<ULONG>(payload_a.size()), FALSE, FALSE, &written,
                  &fi) == STATUS_SUCCESS &&
                written == payload_a.size(),
            "rw4 selftest: write rw4-a.bin");
      Close(&fake, ctx);
      ctx = nullptr;
      check(Rename(&fake, nullptr, const_cast<PWSTR>(L"\\rw4-a.bin"),
                   const_cast<PWSTR>(L"\\rw4-b.bin"), TRUE) == STATUS_SUCCESS,
            "rw4 selftest: rename a -> b");
      check(Open(&fake, const_cast<PWSTR>(L"\\rw4-a.bin"), kFileOpen, 0, &ctx,
                 &fi) == STATUS_OBJECT_NAME_NOT_FOUND,
            "rw4 selftest: old name is gone");
      ctx = nullptr;
      if (check(Open(&fake, const_cast<PWSTR>(L"\\rw4-b.bin"), kFileOpen, 0,
                     &ctx, &fi) == STATUS_SUCCESS,
                "rw4 selftest: open renamed file")) {
        std::vector<uint8_t> readback(payload_a.size(), 0);
        ULONG got = 0;
        check(Read(&fake, ctx, readback.data(), 0,
                   static_cast<ULONG>(readback.size()), &got) == STATUS_SUCCESS &&
                  got == readback.size() && readback == payload_a,
              "rw4 selftest: renamed file keeps content and inode");
        const uint64_t moved_ino = fi.IndexNumber;

        // 2. Hard link through the core (WinFsp has no link callback): both
        // names must resolve to the same inode and share data.
        Status linked =
            state.fs->Link(moved_ino, state.fs->RootInode(), "rw4-hard.bin");
        check(linked.ok(), "rw4 selftest: hard link");
        if (linked.ok()) {
          PVOID link_ctx = nullptr;
          FSP_FSCTL_FILE_INFO link_fi{};
          if (check(Open(&fake, const_cast<PWSTR>(L"\\rw4-hard.bin"), kFileOpen,
                         0, &link_ctx, &link_fi) == STATUS_SUCCESS,
                    "rw4 selftest: open the hard link")) {
            check(link_fi.IndexNumber == moved_ino,
                  "rw4 selftest: hard link shares the inode");
            check(link_fi.HardLinks >= 2, "rw4 selftest: nlink >= 2");
            std::vector<uint8_t> overlay(64, 0x42);
            ULONG wrote = 0;
            check(Write(&fake, link_ctx, overlay.data(), 0,
                        static_cast<ULONG>(overlay.size()), FALSE, FALSE, &wrote,
                        &link_fi) == STATUS_SUCCESS,
                  "rw4 selftest: write through the hard link");
            std::vector<uint8_t> via_other(64, 0);
            ULONG got_other = 0;
            check(Read(&fake, ctx, via_other.data(), 0,
                       static_cast<ULONG>(via_other.size()), &got_other) ==
                      STATUS_SUCCESS &&
                      via_other == overlay,
                  "rw4 selftest: the other name sees the write");
            Close(&fake, link_ctx);
          }
        }
      }
      Close(&fake, ctx);
      ctx = nullptr;
    }

    // 3. Replace an open target: the old handle keeps the old inode, the name
    // points at the moved inode, and the last Close releases the old one.
    auto make_file = [&](const wchar_t* name,
                         const std::vector<uint8_t>& data) -> bool {
      PVOID c = nullptr;
      FSP_FSCTL_FILE_INFO info{};
      if (Create(&fake, const_cast<PWSTR>(name), options_for(kFileCreate, 0), 0,
                 0, nullptr, 0, &c, &info) != STATUS_SUCCESS) {
        return false;
      }
      ULONG wrote = 0;
      bool ok = Write(&fake, c, const_cast<uint8_t*>(data.data()), 0,
                      static_cast<ULONG>(data.size()), FALSE, FALSE, &wrote,
                      &info) == STATUS_SUCCESS &&
                wrote == data.size();
      Close(&fake, c);
      return ok;
    };
    check(make_file(L"\\rw4-victim.bin", payload_v),
          "rw4 selftest: create the replace target");
    check(make_file(L"\\rw4-winner.bin", payload_w),
          "rw4 selftest: create the replace source");
    PVOID victim_ctx = nullptr;
    FSP_FSCTL_FILE_INFO victim_fi{};
    if (check(Open(&fake, const_cast<PWSTR>(L"\\rw4-victim.bin"), kFileOpen, 0,
                   &victim_ctx, &victim_fi) == STATUS_SUCCESS,
              "rw4 selftest: open the replace target")) {
      const uint64_t old_ino = victim_fi.IndexNumber;
      check(Rename(&fake, nullptr, const_cast<PWSTR>(L"\\rw4-winner.bin"),
                   const_cast<PWSTR>(L"\\rw4-victim.bin"),
                   TRUE) == STATUS_SUCCESS,
            "rw4 selftest: replace rename");
      // The old handle still reads the old inode's data.
      std::vector<uint8_t> old_read(payload_v.size(), 0);
      ULONG got = 0;
      check(Read(&fake, victim_ctx, old_read.data(), 0,
                 static_cast<ULONG>(old_read.size()), &got) == STATUS_SUCCESS &&
                got == old_read.size() && old_read == payload_v,
            "rw4 selftest: the replaced inode stays alive for its handle");
      // The name now serves the moved inode.
      PVOID new_ctx = nullptr;
      FSP_FSCTL_FILE_INFO new_fi{};
      if (check(Open(&fake, const_cast<PWSTR>(L"\\rw4-victim.bin"), kFileOpen,
                     0, &new_ctx, &new_fi) == STATUS_SUCCESS,
                "rw4 selftest: open the replaced name")) {
        check(new_fi.IndexNumber != old_ino,
              "rw4 selftest: the name points at the moved inode");
        std::vector<uint8_t> new_read(payload_w.size(), 0);
        ULONG got_new = 0;
        check(Read(&fake, new_ctx, new_read.data(), 0,
                   static_cast<ULONG>(new_read.size()), &got_new) ==
                      STATUS_SUCCESS &&
                  new_read == payload_w,
              "rw4 selftest: the replaced name serves the new contents");
        Close(&fake, new_ctx);
      }
      // Closing the old handle frees the unlinked inode.
      Close(&fake, victim_ctx);
    }

    // 4. Symlink: create through the core, read through the adapter (the
    // documented policy: the target is exposed as the file content), then
    // delete it through SetDelete/Cleanup/Close.
    {
      auto link = state.fs->CreateSymlink(state.fs->RootInode(), "rw4-link",
                                          "../rw4-b.bin", 0777, 0, 0);
      check(link.ok(), "rw4 selftest: create symlink");
      PVOID link_ctx = nullptr;
      FSP_FSCTL_FILE_INFO link_fi{};
      if (check(Open(&fake, const_cast<PWSTR>(L"\\rw4-link"), kFileOpen, 0,
                     &link_ctx, &link_fi) == STATUS_SUCCESS,
                "rw4 selftest: open the symlink")) {
        std::vector<char> target(64, 0);
        ULONG got = 0;
        check(Read(&fake, link_ctx, target.data(), 0,
                   static_cast<ULONG>(target.size()), &got) == STATUS_SUCCESS,
              "rw4 selftest: read the symlink target");
        check(got == 12 && std::memcmp(target.data(), "../rw4-b.bin", 12) == 0,
              "rw4 selftest: symlink target is byte-exact");
        check(SetDelete(&fake, link_ctx, const_cast<PWSTR>(L"\\rw4-link"),
                        TRUE) == STATUS_SUCCESS,
              "rw4 selftest: symlink deletable");
        Cleanup(&fake, link_ctx, const_cast<PWSTR>(L"\\rw4-link"),
                FspCleanupDelete);
        Close(&fake, link_ctx);
        PVOID gone = nullptr;
        check(Open(&fake, const_cast<PWSTR>(L"\\rw4-link"), kFileOpen, 0, &gone,
                   &link_fi) == STATUS_OBJECT_NAME_NOT_FOUND,
              "rw4 selftest: symlink name is gone");
      }
    }

    // 5. Delete the two remaining hard-linked names; the inode stays valid
    // until the last one is gone.
    {
      PVOID hard = nullptr;
      FSP_FSCTL_FILE_INFO hard_fi{};
      if (check(Open(&fake, const_cast<PWSTR>(L"\\rw4-hard.bin"), kFileOpen, 0,
                     &hard, &hard_fi) == STATUS_SUCCESS,
                "rw4 selftest: reopen the hard link")) {
        check(SetDelete(&fake, hard, const_cast<PWSTR>(L"\\rw4-hard.bin"),
                        TRUE) == STATUS_SUCCESS,
              "rw4 selftest: unlink one hard link");
        Cleanup(&fake, hard, const_cast<PWSTR>(L"\\rw4-hard.bin"),
                FspCleanupDelete);
        Close(&fake, hard);
        PVOID other = nullptr;
        check(Open(&fake, const_cast<PWSTR>(L"\\rw4-b.bin"), kFileOpen, 0,
                   &other, &hard_fi) == STATUS_SUCCESS,
              "rw4 selftest: the other name still resolves");
        if (other) Close(&fake, other);
      }
    }

    std::fprintf(stderr,
                 "selftest: RW-4 rename/hardlink/symlink verified through the "
                 "callbacks\n");
  }

  LocalFree(state.security_descriptor);
  std::fprintf(stderr, "selftest: %s (%d failures)\n",
               failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}

Status RunMountedFilesystem(std::unique_ptr<XfsFilesystem> filesystem,
                            const MountConfig& config) {
  if (!filesystem) {
    return XFS_ERR(XfsError::kInvalidArgument, "no filesystem to mount");
  }
  const bool directory_mount = IsDirectoryMountPoint(config.mount_point);
  if (!directory_mount && !IsDriveLetterLike(config.mount_point)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "mount point must be a drive letter (X:) or an absolute "
                   "directory (e.g. E:\\XFS)");
  }

  if (directory_mount) {
    // WinFsp creates the directory mount point itself (with DELETE_ON_CLOSE),
    // so it must not exist yet. Accept an existing *empty* directory by
    // removing it; refuse anything else. Directory mount points live on NTFS
    // and are visible across sessions, unlike drive letters created from an
    // elevated process.
    std::error_code ec;
    if (std::filesystem::exists(config.mount_point, ec)) {
      if (ec || !std::filesystem::is_empty(config.mount_point, ec) || ec) {
        return XFS_ERR(XfsError::kAlreadyExists,
                       "mount directory already exists and is not empty: " +
                           WideToUtf8(config.mount_point));
      }
      std::filesystem::remove(config.mount_point, ec);
      if (ec) {
        return XFS_ERR(XfsError::kIoError,
                       "cannot remove empty mount directory " +
                           WideToUtf8(config.mount_point));
      }
    }
  } else {
    // Refuse to mount over an existing drive letter (network mapping, subst,
    // another volume, ...). WinFsp would otherwise report success while the
    // letter still resolves to the existing device.
    const DWORD drives = GetLogicalDrives();
    const int letter = towupper(config.mount_point[0]) - L'A';
    if (letter >= 0 && letter < 26 && (drives & (1u << letter)) != 0) {
      return XFS_ERR(XfsError::kAlreadyExists,
                     "drive letter " + WideToUtf8(config.mount_point) +
                         " is already in use; choose a free letter")
          .Actual(static_cast<uint32_t>(letter));
    }
  }

  if (config.perf_stats) PerfStats::Get().SetEnabled(true);
  auto state = std::make_unique<AdapterState>();
  state->fs = std::move(filesystem);
  // Callbacks may run from the moment the dispatcher starts; publish the
  // mount now so a terminal device failure is noticed on any callback path.
  MountStateScope mount_state_scope(state.get());
  state->read_write = config.read_write;
  const XfsGeometry& geo = state->fs->geometry();
  const XfsSuperblock& sb = state->fs->superblock();
  if (state->read_write && !state->fs->writable()) {
    return XFS_ERR(XfsError::kInternal,
                   "read-write mount requested for a read-only filesystem");
  }

  state->sector_size = 512;
  state->sectors_per_allocation_unit =
      static_cast<uint16_t>(std::max<uint32_t>(1, geo.block_size / 512));
  state->label = Utf8ToUtf16(sb.label.data());

  // Read-only: read access for everyone (POSIX ACLs are preserved internally
  // but not translated into Windows ACLs in this version). Read-write: full
  // access for everyone, so applications can actually write; the POSIX mode
  // bits still decide per file (a 0444 file is reported read-only).
  const wchar_t* sddl =
      state->read_write ? L"O:BAG:BAD:(A;;FA;;;WD)" : L"O:BAG:BAD:(A;;FR;;;WD)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl, SDDL_REVISION_1, &state->security_descriptor,
          &state->security_descriptor_size)) {
    return XFS_ERR(XfsError::kInternal,
                   "failed to build the volume security descriptor");
  }

  FSP_FSCTL_VOLUME_PARAMS params{};
  params.Version = sizeof(params);
  params.SectorSize = state->sector_size;
  params.SectorsPerAllocationUnit = state->sectors_per_allocation_unit;
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  params.VolumeCreationTime =
      (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  params.VolumeSerialNumber =
      static_cast<uint32_t>(sb.uuid[0] | (sb.uuid[1] << 8) |
                            (sb.uuid[2] << 16) | (sb.uuid[3] << 24));
  // Caching policy. Read-only data never changes, so file info can be cached
  // for a second. On a writable volume file info must not go stale in
  // Windows' cache after our own writes, so the default stays 0. The timeout
  // is overridable for A/B measurement (docs/PERFORMANCE.md); it is not a
  // durability knob.
  uint32_t file_info_timeout = state->read_write ? 0 : 1000;
  if (const char* env = std::getenv("XFSWIN_FILE_INFO_TIMEOUT_MS")) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(env, &end, 10);
    if (end != env && parsed <= 60000) {
      file_info_timeout = static_cast<uint32_t>(parsed);
    }
  }
  params.FileInfoTimeout = file_info_timeout;
  params.CaseSensitiveSearch = 1;
  params.CasePreservedNames = 1;
  params.UnicodeOnDisk = 1;
  params.ReadOnlyVolume = state->read_write ? 0 : 1;
  params.PersistentAcls = 0;
  params.PostCleanupWhenModifiedOnly = 1;
  // The adapter supports open-but-unlinked files and open-handle rename
  // (Cleanup removes the namespace entry and the last Close frees the inode),
  // so let the FSD use POSIX-style delete/rename. Without this flag Windows
  // applications that use FileDispositionInformationEx (PowerShell 7
  // Remove-Item, some installers) get STATUS_NOT_SUPPORTED before the
  // adapter's SetDelete/Cleanup path is reached.
  params.SupportsPosixUnlinkRename = 1;
  // FlushAndPurgeOnCleanup forces a flush (and a device durability barrier)
  // on every close. That is not required for coherence on this volume: a
  // writable image is opened with FILE_SHARE_READ only, so no external writer
  // can modify it, and Windows' cache is kept coherent through the FSD for
  // ordinary cached I/O. Keep it off by default so open/write/close loops do
  // not pay one FlushFileBuffers per file. Explicit FlushFileBuffers still
  // reaches the Flush callback and the durability contract. Set
  // XFSWIN_FLUSH_PURGE_ON_CLEANUP=1 to restore the old behavior.
  bool flush_and_purge = false;
  if (const char* env = std::getenv("XFSWIN_FLUSH_PURGE_ON_CLEANUP")) {
    flush_and_purge = std::strcmp(env, "1") == 0;
  }
  params.FlushAndPurgeOnCleanup = flush_and_purge ? 1 : 0;
  wcscpy_s(params.FileSystemName,
           sizeof(params.FileSystemName) / sizeof(WCHAR), L"XFS");

  FSP_FILE_SYSTEM_INTERFACE interface{};
  interface.GetVolumeInfo = GetVolumeInfo;
  interface.SetVolumeLabel = SetVolumeLabel;
  interface.GetSecurityByName = GetSecurityByName;
  interface.Create = Create;
  interface.Open = Open;
  interface.Overwrite = Overwrite;
  interface.Cleanup = Cleanup;
  interface.Close = Close;
  interface.Read = Read;
  interface.Write = Write;
  interface.Flush = Flush;
  interface.GetFileInfo = GetFileInfo;
  interface.SetBasicInfo = SetBasicInfo;
  interface.SetFileSize = SetFileSize;
  interface.CanDelete = CanDelete;
  interface.SetDelete = SetDelete;
  interface.Rename = Rename;
  interface.GetSecurity = GetSecurity;
  interface.SetSecurity = SetSecurity;
  interface.ReadDirectory = ReadDirectory;

  FSP_FILE_SYSTEM* winfsp = nullptr;
  // The device path must be the WinFsp control device (FSP_FSCTL_DISK_DEVICE_NAME
  // is a narrow literal; the L"" prefix makes the concatenation wide). The FSD
  // derives a unique volume device from the mount point.
  static const wchar_t kDiskDeviceName[] = L"" FSP_FSCTL_DISK_DEVICE_NAME;
  NTSTATUS status = FspFileSystemCreate(
      const_cast<PWSTR>(kDiskDeviceName), &params, &interface, &winfsp);
  if (status != STATUS_SUCCESS) {
    LocalFree(state->security_descriptor);
    return XFS_ERR(XfsError::kInternal,
                   "FspFileSystemCreate failed (NTSTATUS 0x" +
                       std::to_string(static_cast<uint32_t>(status)) + ")");
  }
  winfsp->UserContext = state.get();
  // WinFsp's fine-grained guard: namespace operations take an exclusive lock,
  // while reads/writes/getinfo for different files run in parallel. The core
  // uses a write barrier plus per-inode stripes, and the image device now
  // allows concurrent positioned I/O, so this actually reaches the storage.
  FspFileSystemSetOperationGuard(winfsp, FspFileSystemOpEnter,
                                 FspFileSystemOpLeave);
  FspFileSystemSetOperationGuardStrategy(
      winfsp, FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_FINE);

  status = FspFileSystemSetMountPoint(winfsp,
                                      const_cast<PWSTR>(config.mount_point.c_str()));
  if (status == STATUS_SUCCESS) {
    status = FspFileSystemStartDispatcher(winfsp, 0);
  }
  if (status != STATUS_SUCCESS) {
    FspFileSystemRemoveMountPoint(winfsp);
    FspFileSystemDelete(winfsp);
    LocalFree(state->security_descriptor);
    return XFS_ERR(XfsError::kInternal,
                   "failed to mount XFS at " +
                       WideToUtf8(config.mount_point) +
                       " (NTSTATUS 0x" +
                       std::to_string(static_cast<uint32_t>(status)) +
                       "); is WinFsp installed and is the drive letter free?");
  }

  std::wstring event_name = StopEventName(config.mount_point);
  HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
  g_stop_event = stop_event;
  // A device failure during mount setup latched the flag before the event
  // existed; honor it now instead of waiting forever.
  if (state->device_failed.load(std::memory_order_relaxed)) {
    SetEvent(stop_event);
  }
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  XFS_INFO("mounted XFS " << (config.read_write ? "read-write" : "read-only")
                          << " at " << WideToUtf8(config.mount_point)
                          << " (uuid " << FormatUuid(sb.uuid) << ")");
  if (config.read_write) {
    XFS_WARN("read-write mount: experimental writer; create, overwrite, "
             "extend, truncate, delete, rename, hard links, symlinks, xattrs "
             "and reflinks are enabled (real disks require "
             "--device-write-qualification)");
  }
  if (stop_event) {
    WaitForSingleObject(stop_event, INFINITE);
  } else {
    XFS_WARN("failed to create stop event; use Ctrl+C to unmount");
    for (;;) Sleep(1000);
  }

  SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
  g_stop_event = nullptr;
  FspFileSystemStopDispatcher(winfsp);
  // Clean unmount: close the journal with an unmount record and push
  // everything this process wrote to stable storage before the mount point
  // disappears. A failure is reported as a failed unmount: the filesystem may
  // still be structurally consistent, but the caller must not be told that the
  // durable close succeeded when it did not.
  Status unmount_status = OkStatus();
  if (config.read_write) {
    // Publish deferred timestamps before the unmount record: a clean log must
    // not be followed by new transactions.
    Status published = state->fs->CommitAllDeferredMetadata();
    if (!published.ok()) {
      XFS_ERROR("unmount deferred metadata commit failed: "
                << published.error().Format());
      unmount_status = published;
    }
    Status closed = state->fs->WriteCleanUnmountRecord();
    if (!closed.ok()) {
      XFS_ERROR("unmount journal close failed: " << closed.error().Format());
      if (unmount_status.ok()) unmount_status = closed;
    }
    Status flushed = state->fs->Flush();
    if (!flushed.ok()) {
      XFS_ERROR("unmount flush failed: " << flushed.error().Format());
      if (unmount_status.ok()) unmount_status = flushed;
    }
  }
  FspFileSystemRemoveMountPoint(winfsp);
  FspFileSystemDelete(winfsp);
  if (stop_event) CloseHandle(stop_event);
  LocalFree(state->security_descriptor);
  if (PerfStats::Get().enabled()) {
    std::fputs(PerfStats::Get().Text().c_str(), stderr);
  }
  XFS_INFO("unmounted");
  return unmount_status;
}

Status RequestUnmount(const std::wstring& mount_point) {
  if (mount_point.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "expected a drive letter (X:) or a mount directory");
  }
  std::wstring event_name = StopEventName(mount_point);
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
  if (!event) {
    return XFS_ERR(XfsError::kNotMounted,
                   "no running xfswin mount found for this mount point");
  }
  SetEvent(event);
  CloseHandle(event);
  return OkStatus();
}

}  // namespace xfs::winfsp

#endif  // XFSWIN_HAS_WINFSP
