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
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>
typedef NTSTATUS* PNTSTATUS;

#include <winfsp/winfsp.h>
#include <sddl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "xfs/log.h"
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
    uint32_t refcount = 0;
  };
  std::mutex node_mutex;
  std::unordered_map<uint64_t, uint64_t> node_by_inode;
  std::unordered_map<uint64_t, std::shared_ptr<FileNode>> nodes;
  uint64_t next_node_id = 1;

  // Resolved WinFsp path -> inode number. Explorer queries security and then
  // opens the same file, and revisits files across refreshes; this turns the
  // repeated path walk into one hash lookup.
  std::unordered_map<std::wstring, uint64_t> path_inode_cache;

  // Sorted (Windows name, inode) children per directory. Directory contents
  // are immutable for this read-only mount, so this is built once per
  // directory and reused by every ReadDirectory page (WinFsp re-enters with a
  // marker for each page) and by repeated Explorer enumerations.
  std::unordered_map<uint64_t, std::shared_ptr<const std::vector<DirChild>>>
      dir_children;
  size_t dir_children_entries = 0;
};

uint64_t NodeId(PVOID context) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
}

// Copies the inode behind a live file node. Returns false for stale nodes.
std::shared_ptr<AdapterState::FileNode> LookupNode(AdapterState& state,
                                                   PVOID context) {
  if (!context) return nullptr;
  std::lock_guard<std::mutex> lock(state.node_mutex);
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

NTSTATUS NtFromError(XfsError code) {
  switch (code) {
    case XfsError::kNotFound: return STATUS_OBJECT_NAME_NOT_FOUND;
    case XfsError::kNotDirectory: return STATUS_NOT_A_DIRECTORY;
    case XfsError::kNotRegularFile: return STATUS_INVALID_DEVICE_REQUEST;
    case XfsError::kNotSymlink: return STATUS_INVALID_DEVICE_REQUEST;
    case XfsError::kNameTooLong: return STATUS_OBJECT_NAME_INVALID;
    case XfsError::kInvalidArgument: return STATUS_INVALID_PARAMETER;
    case XfsError::kPermissionDenied: return STATUS_ACCESS_DENIED;
    case XfsError::kIoError:
    case XfsError::kShortRead: return STATUS_IO_DEVICE_ERROR;
    case XfsError::kUnsupportedFeature:
    case XfsError::kUnsupportedGeometry: return STATUS_NOT_SUPPORTED;
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
  {
    std::lock_guard<std::mutex> lock(state.node_mutex);
    auto it = state.path_inode_cache.find(key);
    if (it != state.path_inode_cache.end()) {
      auto inode = state.fs->OpenInode(it->second);
      if (inode.ok()) return inode;
    }
  }
  auto inode = ResolvePath(*state.fs, file_name);
  if (inode.ok()) {
    std::lock_guard<std::mutex> lock(state.node_mutex);
    constexpr size_t kMaxPathCacheEntries = 32768;
    if (state.path_inode_cache.size() >= kMaxPathCacheEntries) {
      state.path_inode_cache.clear();
    }
    state.path_inode_cache.emplace(key, inode.value().ino);
  }
  return inode;
}

uint64_t ToFileTime(const XfsTimestamp& ts) {
  constexpr int64_t kEpochOffset = 11644473600LL;  // 1601 -> 1970 seconds
  int64_t sec = ts.sec + kEpochOffset;
  if (sec < 0) sec = 0;
  return static_cast<uint64_t>(sec) * 10000000ULL + ts.nsec / 100;
}

uint32_t FileAttributesFor(uint16_t mode) {
  uint32_t attributes = FILE_ATTRIBUTE_READONLY;
  if ((mode & 0170000) == 0040000) attributes |= FILE_ATTRIBUTE_DIRECTORY;
  return attributes;
}

void FillFileInfoCore(AdapterState& state, uint16_t mode, uint64_t nblocks,
                      uint64_t size, const XfsTimestamp& atime,
                      const XfsTimestamp& mtime, const XfsTimestamp& ctime,
                      const XfsTimestamp& crtime, uint64_t ino,
                      FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  info->FileAttributes = FileAttributesFor(mode);
  info->AllocationSize =
      nblocks * static_cast<uint64_t>(state.fs->geometry().block_size);
  info->FileSize = (mode & 0170000) == 0040000 ? 0 : size;
  info->CreationTime = ToFileTime(crtime);
  info->LastAccessTime = ToFileTime(atime);
  info->LastWriteTime = ToFileTime(mtime);
  info->ChangeTime = ToFileTime(ctime);
  info->IndexNumber = ino;
  info->HardLinks = 0;
  info->EaSize = 0;
  info->ReparseTag = 0;
}

void FillFileInfo(AdapterState& state, const XfsInode& inode,
                  FSP_FSCTL_FILE_INFO* info) {
  FillFileInfoCore(state, inode.mode, inode.nblocks, inode.size, inode.atime,
                   inode.mtime, inode.ctime, inode.crtime, inode.ino, info);
}

void FillFileInfo(AdapterState& state, const XfsFileInfo& inode,
                  FSP_FSCTL_FILE_INFO* info) {
  FillFileInfoCore(state, inode.mode, inode.nblocks, inode.size, inode.atime,
                   inode.mtime, inode.ctime, inode.crtime, inode.ino, info);
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

NTSTATUS SetVolumeLabel(FSP_FILE_SYSTEM*, PWSTR, FSP_FSCTL_VOLUME_INFO*) {
  return STATUS_MEDIA_WRITE_PROTECTED;
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
  uint32_t attributes = FileAttributesFor(inode.value().mode);
  if (NeedsSecurityOverflow(state, security_descriptor_size)) {
    if (file_attributes) *file_attributes = attributes;
    return STATUS_BUFFER_OVERFLOW;
  }
  FillSecurity(state, security_descriptor, security_descriptor_size,
               file_attributes, attributes);
  return STATUS_SUCCESS;
}

NTSTATUS Create(FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, UINT32,
                PSECURITY_DESCRIPTOR, UINT64, PVOID*, FSP_FSCTL_FILE_INFO*) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS Open(FSP_FILE_SYSTEM* file_system, PWSTR file_name,
              UINT32 create_options, UINT32 /*granted_access*/,
              PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) {
  AdapterState& state = *State(file_system);
  auto inode = ResolvePathCached(state, file_name);
  if (!inode.ok()) {
    XFS_DEBUG("Open('" << WideToUtf8(file_name)
                       << "') failed: " << inode.error().Format());
    return NtFromError(inode.error().code);
  }
  if ((create_options & FILE_DIRECTORY_FILE) && !inode.value().IsDir()) {
    return STATUS_NOT_A_DIRECTORY;
  }
  if ((create_options & FILE_NON_DIRECTORY_FILE) && inode.value().IsDir()) {
    return STATUS_FILE_IS_A_DIRECTORY;
  }
  auto node = std::make_shared<const XfsInode>(std::move(inode.value()));
  FillFileInfo(state, *node, file_info);
  // Parse the data fork once per open: reads then never re-walk the extent
  // list or the BMBT.
  std::shared_ptr<const std::vector<XfsExtent>> extents;
  if (node->IsRegular() && node->format != ondisk::inode::kFormatLocal) {
    auto loaded = std::make_shared<std::vector<XfsExtent>>();
    if (LoadDataForkExtents(state.fs->device(), state.fs->geometry(), *node,
                            loaded.get())
            .ok()) {
      extents = std::move(loaded);
    }
  }
  uint64_t id;
  {
    std::lock_guard<std::mutex> lock(state.node_mutex);
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
      entry->refcount = 1;
      state.nodes.emplace(id, std::move(entry));
      state.node_by_inode.emplace(node->ino, id);
    }
  }
  *file_context = reinterpret_cast<PVOID>(static_cast<uintptr_t>(id));
  return STATUS_SUCCESS;
}

NTSTATUS Overwrite(FSP_FILE_SYSTEM*, PVOID, UINT32, BOOLEAN, UINT64,
                   FSP_FSCTL_FILE_INFO*) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

VOID Cleanup(FSP_FILE_SYSTEM*, PVOID, PWSTR, ULONG) {}

VOID Close(FSP_FILE_SYSTEM* file_system, PVOID file_context) {
  AdapterState& state = *State(file_system);
  if (!file_context) return;
  // One reference per Open. The node is destroyed when the last one goes away
  // (idempotent for repeated Close of the same node).
  std::lock_guard<std::mutex> lock(state.node_mutex);
  auto it = state.nodes.find(NodeId(file_context));
  if (it == state.nodes.end()) return;
  if (0 == --it->second->refcount) {
    state.node_by_inode.erase(it->second->inode->ino);
    state.nodes.erase(it);
  }
}

NTSTATUS Read(FSP_FILE_SYSTEM* file_system, PVOID file_context,
              PVOID buffer, UINT64 offset, ULONG length,
              PULONG bytes_transferred) {
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

NTSTATUS Write(FSP_FILE_SYSTEM*, PVOID, PVOID, UINT64, ULONG, BOOLEAN, BOOLEAN,
               PULONG, FSP_FSCTL_FILE_INFO*) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS Flush(FSP_FILE_SYSTEM*, PVOID, FSP_FSCTL_FILE_INFO*) {
  return STATUS_SUCCESS;
}

NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                     FSP_FSCTL_FILE_INFO* file_info) {
  AdapterState& state = *State(file_system);
  auto node = LookupNode(state, file_context);
  if (!node) {
    XFS_DEBUG("GetFileInfo: stale file node " << NodeId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  FillFileInfo(state, *node->inode, file_info);
  return STATUS_SUCCESS;
}

NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM*, PVOID, UINT32, UINT64, UINT64, UINT64,
                      UINT64, FSP_FSCTL_FILE_INFO*) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS SetFileSize(FSP_FILE_SYSTEM*, PVOID, UINT64, BOOLEAN,
                     FSP_FSCTL_FILE_INFO*) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS CanDelete(FSP_FILE_SYSTEM*, PVOID, PWSTR) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS Rename(FSP_FILE_SYSTEM*, PVOID, PWSTR, PWSTR, BOOLEAN) {
  return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS GetSecurity(FSP_FILE_SYSTEM* file_system, PVOID,
                     PSECURITY_DESCRIPTOR security_descriptor,
                     SIZE_T* security_descriptor_size) {
  AdapterState& state = *State(file_system);
  if (NeedsSecurityOverflow(state, security_descriptor_size)) {
    return STATUS_BUFFER_OVERFLOW;
  }
  FillSecurity(state, security_descriptor, security_descriptor_size, nullptr,
               FILE_ATTRIBUTE_READONLY);
  return STATUS_SUCCESS;
}

NTSTATUS SetSecurity(FSP_FILE_SYSTEM*, PVOID, SECURITY_INFORMATION,
                     PSECURITY_DESCRIPTOR) {
  return STATUS_MEDIA_WRITE_PROTECTED;
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
    std::lock_guard<std::mutex> lock(state.node_mutex);
    auto it = state.dir_children.find(context->inode->ino);
    if (it != state.dir_children.end()) children = it->second;
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
      std::lock_guard<std::mutex> lock(state.node_mutex);
      constexpr size_t kMaxCachedDirs = 64;
      constexpr size_t kMaxCachedEntries = 262144;
      if (state.dir_children.size() >= kMaxCachedDirs ||
          state.dir_children_entries + built->size() > kMaxCachedEntries) {
        state.dir_children.clear();
        state.dir_children_entries = 0;
      }
      state.dir_children_entries += built->size();
      state.dir_children.emplace(context->inode->ino, built);
    }
    children = std::move(built);
  }

  // Fixed ordering: ".", "..", then children sorted by Windows name. The
  // marker is the last name returned by the previous call.
  std::u16string marker_name = marker ? ToU16(std::wstring(marker))
                                      : std::u16string();
  bool marker_seen = marker == nullptr;

  auto add_entry = [&](uint64_t ino, const std::u16string& name) -> bool {
    auto file_info = state.fs->GetFileInfo(ino);
    if (!file_info.ok()) {
      XFS_WARN("cannot open inode " << ino << ": "
                                    << file_info.error().Format());
      return true;  // skip this entry, keep enumerating
    }
    FSP_FSCTL_FILE_INFO out;
    FillFileInfo(state, file_info.value(), &out);
    return AddOneDirectoryEntry(file_system, name, out, buffer, length,
                                bytes_transferred);
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
          if (!parent.ok()) continue;
          parent_ino = parent.value().ino;
          have_parent = true;
        }
        if (!add_entry(parent_ino, wname)) return STATUS_SUCCESS;
      } else {
        if (!add_entry(context->inode->ino, wname)) return STATUS_SUCCESS;
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
    if (!add_entry(it->ino, it->name)) return STATUS_SUCCESS;
  }

  // End-of-directory marker: tells the FSD the enumeration is complete (see
  // FspFileSystemAddDirInfo with a NULL DirInfo).
  FspFileSystemAddDirInfo(nullptr, buffer, length, bytes_transferred);
  return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Mount lifetime
// ---------------------------------------------------------------------------

HANDLE g_stop_event = nullptr;

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

  const wchar_t* dirs[] = {L"\\", L"\\folder", L"\\fragdir"};
  const wchar_t* files[] = {L"\\hello.txt", L"\\large.bin", L"\\sparse.img",
                            L"\\folder\\link"};
  std::vector<uint8_t> dir_buffer(64 * 1024);
  std::vector<uint8_t> read_buffer(128 * 1024);
  std::vector<uint8_t> sd_buffer(1024);
  std::wstring marker;

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
      check(GetSecurityByName(&fake, const_cast<PWSTR>(dir), &attrs, nullptr,
                              nullptr) == STATUS_SUCCESS,
            "GetSecurityByName attrs");
      SIZE_T sd_size = sd_buffer.size();
      check(GetSecurityByName(&fake, const_cast<PWSTR>(dir), &attrs,
                              sd_buffer.data(), &sd_size) == STATUS_SUCCESS,
            "GetSecurityByName sd");

      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      if (!check(Open(&fake, const_cast<PWSTR>(dir), FILE_DIRECTORY_FILE, 0,
                      &ctx, &fi) == STATUS_SUCCESS,
                 "Open directory")) {
        continue;
      }
      check((fi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            "directory attribute");
      enumerate(ctx, "dir");
      check(GetFileInfo(&fake, ctx, &fi) == STATUS_SUCCESS, "GetFileInfo dir");
      Close(&fake, ctx);
    }

    for (const wchar_t* file : files) {
      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      if (!check(Open(&fake, const_cast<PWSTR>(file), 0, 0, &ctx, &fi) ==
                     STATUS_SUCCESS,
                 "Open file")) {
        continue;
      }
      ULONG transferred = 0;
      check(Read(&fake, ctx, read_buffer.data(), 0,
                 static_cast<ULONG>(read_buffer.size()),
                 &transferred) == STATUS_SUCCESS,
            "Read file");
      check(GetFileInfo(&fake, ctx, &fi) == STATUS_SUCCESS, "GetFileInfo file");
      Close(&fake, ctx);
    }

    if (iter < 3) {
      PVOID ctx = nullptr;
      FSP_FSCTL_FILE_INFO fi{};
      if (check(Open(&fake, const_cast<PWSTR>(L"\\bigdir"),
                     FILE_DIRECTORY_FILE, 0, &ctx, &fi) == STATUS_SUCCESS,
                "Open bigdir")) {
        enumerate(ctx, "bigdir");
        Close(&fake, ctx);
      }
    }

    check(HeapValidate(GetProcessHeap(), 0, nullptr), "heap valid");
    if (failures) break;
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

  auto state = std::make_unique<AdapterState>();
  state->fs = std::move(filesystem);
  const XfsGeometry& geo = state->fs->geometry();
  const XfsSuperblock& sb = state->fs->superblock();

  state->sector_size = 512;
  state->sectors_per_allocation_unit =
      static_cast<uint16_t>(std::max<uint32_t>(1, geo.block_size / 512));
  state->label = Utf8ToUtf16(sb.label.data());

  // Read-only access for everyone (POSIX ACLs are preserved internally but
  // not translated into Windows ACLs in this version).
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:BAG:BAD:(A;;FR;;;WD)", SDDL_REVISION_1,
          &state->security_descriptor, &state->security_descriptor_size)) {
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
  params.FileInfoTimeout = 1000;
  params.CaseSensitiveSearch = 1;
  params.CasePreservedNames = 1;
  params.UnicodeOnDisk = 1;
  params.ReadOnlyVolume = 1;
  params.PersistentAcls = 0;
  params.PostCleanupWhenModifiedOnly = 1;
  // Do NOT set FlushAndPurgeOnCleanup: that flag purges a file's cached pages
  // on every close. Windows does not retain this volume's data in its cache
  // (re-reads go back to the raw device at device speed), but purging would
  // additionally penalize memory-mapped access. A read-only volume never has
  // dirty data to flush. When read-write support arrives this must flip back
  // to 1 together with explicit cache invalidation on writes.
  params.FlushAndPurgeOnCleanup = 0;
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
  // Install WinFsp's built-in operation guard with the fine-grained strategy:
  // namespace operations take a shared lock, while reads/writes/getinfo run
  // fully in parallel. The core caches are mutex-protected and block-device
  // I/O is serialized inside the device objects, so concurrent reads scale to
  // multiple threads instead of queueing behind one global lock.
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
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  XFS_INFO("mounted XFS read-only at " << WideToUtf8(config.mount_point)
                                       << " (uuid " << FormatUuid(sb.uuid)
                                       << ")");
  if (stop_event) {
    WaitForSingleObject(stop_event, INFINITE);
  } else {
    XFS_WARN("failed to create stop event; use Ctrl+C to unmount");
    for (;;) Sleep(1000);
  }

  SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
  g_stop_event = nullptr;
  FspFileSystemStopDispatcher(winfsp);
  FspFileSystemRemoveMountPoint(winfsp);
  FspFileSystemDelete(winfsp);
  if (stop_event) CloseHandle(stop_event);
  LocalFree(state->security_descriptor);
  XFS_INFO("unmounted");
  return OkStatus();
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
