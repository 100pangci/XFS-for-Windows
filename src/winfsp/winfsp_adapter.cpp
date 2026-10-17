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
#include <mutex>
#include <unordered_map>
#include <vector>

#include "xfs/log.h"
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

struct AdapterState {
  std::unique_ptr<XfsFilesystem> fs;
  PSECURITY_DESCRIPTOR security_descriptor = nullptr;
  ULONG security_descriptor_size = 0;
  std::u16string label;
  uint16_t sector_size = 512;
  uint16_t sectors_per_allocation_unit = 8;
  // File contexts are identified by a monotonically increasing id (never
  // reused). WinFsp may call Close twice for one open: once when a failed
  // Overwrite releases the context and again when the handle is torn down.
  // Close is therefore idempotent and the map (not the raw pointer) owns the
  // inode data.
  std::mutex context_mutex;
  std::unordered_map<uint64_t, XfsInode> contexts;
  uint64_t next_context_id = 1;
};

uint64_t ContextId(PVOID context) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
}

// Copies the inode for a live file context. Returns false for stale contexts.
bool LookupContext(AdapterState& state, PVOID context, XfsInode* out) {
  if (!context) return false;
  std::lock_guard<std::mutex> lock(state.context_mutex);
  auto it = state.contexts.find(ContextId(context));
  if (it == state.contexts.end()) return false;
  *out = it->second;
  return true;
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

uint64_t ToFileTime(const XfsTimestamp& ts) {
  constexpr int64_t kEpochOffset = 11644473600LL;  // 1601 -> 1970 seconds
  int64_t sec = ts.sec + kEpochOffset;
  if (sec < 0) sec = 0;
  return static_cast<uint64_t>(sec) * 10000000ULL + ts.nsec / 100;
}

uint32_t FileAttributesFor(const XfsInode& inode) {
  uint32_t attributes = FILE_ATTRIBUTE_READONLY;
  if (inode.IsDir()) attributes |= FILE_ATTRIBUTE_DIRECTORY;
  return attributes;
}

void FillFileInfo(AdapterState& state, const XfsInode& inode,
                  FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  info->FileAttributes = FileAttributesFor(inode);
  info->AllocationSize =
      inode.nblocks * static_cast<uint64_t>(state.fs->geometry().block_size);
  info->FileSize = inode.IsDir() ? 0 : inode.size;
  info->CreationTime = ToFileTime(inode.crtime);
  info->LastAccessTime = ToFileTime(inode.atime);
  info->LastWriteTime = ToFileTime(inode.mtime);
  info->ChangeTime = ToFileTime(inode.ctime);
  info->IndexNumber = inode.ino;
  info->HardLinks = 0;
  info->EaSize = 0;
  info->ReparseTag = 0;
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
  return state.fs->ReadFile(inode.ino, offset, buffer, length);
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
  auto inode = ResolvePath(*state.fs, file_name);
  if (!inode.ok()) {
    XFS_WARN("GetSecurityByName('" << WideToUtf8(file_name)
                                    << "') failed: "
                                    << inode.error().Format());
    return NtFromError(inode.error().code);
  }
  uint32_t attributes = FileAttributesFor(inode.value());
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
  auto inode = ResolvePath(*state.fs, file_name);
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
  XfsInode node = std::move(inode.value());
  FillFileInfo(state, node, file_info);
  uint64_t id;
  {
    std::lock_guard<std::mutex> lock(state.context_mutex);
    id = state.next_context_id++;
    state.contexts.emplace(id, std::move(node));
  }
  XFS_WARN("open id=" << id << " '" << WideToUtf8(file_name) << "'");
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
  // Idempotent: WinFsp closes a context on a failed Overwrite and then closes
  // it again when the handle is torn down.
  std::lock_guard<std::mutex> lock(state.context_mutex);
  XFS_WARN("close id=" << ContextId(file_context));
  state.contexts.erase(ContextId(file_context));
}

NTSTATUS Read(FSP_FILE_SYSTEM* file_system, PVOID file_context,
              PVOID buffer, UINT64 offset, ULONG length,
              PULONG bytes_transferred) {
  AdapterState& state = *State(file_system);
  XfsInode node;
  if (!LookupContext(state, file_context, &node)) {
    XFS_WARN("Read: stale context id=" << ContextId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  if (node.IsDir()) return STATUS_INVALID_DEVICE_REQUEST;
  auto read = ReadInode(state, node, offset, buffer, length);
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
  XfsInode node;
  if (!LookupContext(state, file_context, &node)) {
    XFS_WARN("GetFileInfo: stale context id=" << ContextId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  FillFileInfo(state, node, file_info);
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

bool AddOneDirectoryEntry(FSP_FILE_SYSTEM* file_system, const XfsInode& inode,
                          const std::u16string& name, PVOID buffer,
                          ULONG length, PULONG bytes_transferred) {
  AdapterState& state = *State(file_system);
  if (name.size() >= 256) return TRUE;  // skip unrepresentable long names
  std::vector<uint8_t> entry(sizeof(FSP_FSCTL_DIR_INFO) +
                             name.size() * sizeof(char16_t));
  std::memset(entry.data(), 0, entry.size());
  auto* info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(entry.data());
  info->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) +
                                   name.size() * sizeof(char16_t));
  FillFileInfo(state, inode, &info->FileInfo);
  std::memcpy(info->FileNameBuf, name.data(), name.size() * sizeof(char16_t));
  return FspFileSystemAddDirInfo(info, buffer, length, bytes_transferred) !=
         FALSE;
}

NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* file_system, PVOID file_context,
                       PWSTR /*pattern*/, PWSTR marker, PVOID buffer,
                       ULONG length, PULONG bytes_transferred) {
  AdapterState& state = *State(file_system);
  XfsInode context;
  if (!LookupContext(state, file_context, &context)) {
    XFS_WARN("ReadDirectory: stale context id=" << ContextId(file_context));
    return STATUS_INVALID_HANDLE;
  }
  if (!context.IsDir()) return STATUS_NOT_A_DIRECTORY;
  *bytes_transferred = 0;

  // Fixed ordering: ".", "..", then children sorted by Windows name. The
  // marker is the last name returned by the previous call.
  std::u16string marker_name = marker ? ToU16(std::wstring(marker))
                                      : std::u16string();
  bool marker_seen = marker == nullptr;

  auto entries = state.fs->ListDirectory(context.ino);
  if (!entries.ok()) return NtFromError(entries.error().code);

  struct Child {
    std::u16string name;
    uint64_t ino;
  };
  std::vector<Child> children;
  children.reserve(entries.value().size());
  for (const auto& entry : entries.value()) {
    if (entry.name == "." || entry.name == "..") continue;
    WindowsName win = XfsNameToWindows(entry.name);
    if (win.too_long) {
      XFS_WARN("directory entry '" << entry.name
                                   << "' is too long for Windows; skipped");
      continue;
    }
    children.push_back({win.name, entry.ino});
  }
  std::sort(children.begin(), children.end(),
            [](const Child& a, const Child& b) { return a.name < b.name; });

  auto resolve_info = [&](uint64_t ino, XfsInode* out) -> bool {
    auto inode = state.fs->OpenInode(ino);
    if (!inode.ok()) {
      XFS_WARN("cannot open inode " << ino << ": "
                                    << inode.error().Format());
      return false;
    }
    *out = std::move(inode.value());
    return true;
  };

  // Dot entries (only for non-root directories; the FSD handles root ".").
  if (context.ino != state.fs->RootInode()) {
    for (bool parent_entry : {false, true}) {
      std::u16string wname = parent_entry ? u".." : u".";
      if (!marker_seen) {
        if (wname == marker_name) marker_seen = true;
        continue;
      }
      XfsInode target = context;
      if (parent_entry) {
        auto parent = state.fs->LookupName(context.ino, "..");
        if (!parent.ok() || !resolve_info(parent.value().ino, &target)) {
          continue;
        }
      }
      if (!AddOneDirectoryEntry(file_system, target, wname, buffer, length,
                                bytes_transferred)) {
        return STATUS_SUCCESS;
      }
    }
  }

  for (const auto& child : children) {
    if (!marker_seen) {
      if (child.name == marker_name) marker_seen = true;
      continue;
    }
    XfsInode inode;
    if (!resolve_info(child.ino, &inode)) continue;
    if (!AddOneDirectoryEntry(file_system, inode, child.name, buffer, length,
                              bytes_transferred)) {
      return STATUS_SUCCESS;
    }
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

std::wstring StopEventName(const std::wstring& drive) {
  wchar_t letter = drive.empty() ? L'X' : drive[0];
  if (letter >= L'a' && letter <= L'z') letter = letter - L'a' + L'A';
  std::wstring name = L"Local\\xfswin.mount.";
  name.push_back(letter);
  return name;
}

bool IsDriveLetterLike(const std::wstring& drive) {
  if (drive.size() != 2 && !(drive.size() == 3 && drive[2] == L'\\')) {
    return false;
  }
  wchar_t c = drive[0];
  return ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) &&
         drive[1] == L':';
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
  if (!IsDriveLetterLike(config.mount_point)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "mount point must be a drive letter like X:");
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
  params.FlushAndPurgeOnCleanup = 1;
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
  // Install WinFsp's built-in operation guard and serialize every operation.
  // The core is not thread-safe (shared block device state), so the FSD must
  // hold its exclusive lock around all callbacks; setting only the strategy
  // is not enough (EnterOperation is NULL by default and skips locking).
  FspFileSystemSetOperationGuard(winfsp, FspFileSystemOpEnter,
                                 FspFileSystemOpLeave);
  FspFileSystemSetOperationGuardStrategy(
      winfsp, FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_COARSE);

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

Status RequestUnmount(const std::wstring& drive) {
  if (!IsValidDriveLetter(drive)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "expected a drive letter (e.g. X:)");
  }
  std::wstring event_name = StopEventName(drive);
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
  if (!event) {
    return XFS_ERR(XfsError::kNotMounted,
                   "no running xfswin mount found for this drive letter");
  }
  SetEvent(event);
  CloseHandle(event);
  return OkStatus();
}

}  // namespace xfs::winfsp

#endif  // XFSWIN_HAS_WINFSP
