// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "cli/cli.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

#include "xfs/block_device.h"
#include "xfs/directory.h"
#include "xfs/endian.h"
#include "xfs/error.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/image_device.h"
#include "xfs/inode.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"
#include "xfs/win_name_map.h"
#include "xfs/windows_raw_device.h"

#ifdef XFSWIN_HAS_WINFSP
#include "winfsp/mount_host.h"
#include "winfsp/winfsp_adapter.h"
#endif

namespace xfs::cli {
namespace {

constexpr const char* kVersion = "xfswin 0.1.0 (XFS v5 read-only + formatter)";

void PrintUsage() {
  std::cout <<
      "XFS for Windows - Windows-native XFS formatter and driver\n"
      "\n"
      "usage: xfswin <command> [options]\n"
      "\n"
      "commands:\n"
      "  list                          list physical disks (with safety info)\n"
      "  info <device|image>           show superblock and geometry\n"
      "  check <device|image>          read-only consistency validation\n"
      "  mkimage <file> <size>         create a sparse test image (e.g. 300M)\n"
      "  ls <device|image> <path>      list a directory\n"
      "  cat <device|image> <path>     write a file (or symlink target) to stdout\n"
      "  stat <device|image> <path>    show inode information\n"
      "  pwrite <image> <path> <off>   overwrite bytes inside an existing file\n"
      "      --rw-experimental [--text=<s> | --from=<file>]\n"
      "  format <device|image>         create a new XFS v5 filesystem\n"
      "  mount <device|image> <X:>     mount via WinFsp (read-only by default)\n"
      "      --rw-experimental         writable mount (images only, in-place\n"
      "                                overwrites of allocated data only)\n"
      "  unmount <X:>                  stop an xfswin mount\n"
      "\n"
      "global options:\n"
      "  --verbose                     informational output\n"
      "  --debug                       detailed parser traversal output\n"
      "  --force-unsafe-ro             allow mounting a dirty/unknown log\n"
      "  --yes-i-really-mean-it        required for destructive format\n"
      "  --help                        this message\n"
      "\n"
      "examples:\n"
      "  xfswin info test.img\n"
      "  xfswin check test.img\n"
      "  xfswin ls test.img /\n"
      "  xfswin cat \\\\.\\PhysicalDrive2 /etc/hostname\n"
      "  xfswin mount test.img X:\n"
      "  xfswin mount test.img X: --rw-experimental\n";
}

std::string HumanBytes(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 5) {
    value /= 1024.0;
    ++unit;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f %s", value, units[unit]);
  return buf;
}

std::string FormatTime(const XfsTimestamp& ts) {
  std::time_t t = static_cast<std::time_t>(ts.sec);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return buf;
}

std::string ModeString(uint16_t mode) {
  char buf[11];
  buf[0] = '-';
  switch (mode & kSIfmt) {
    case kSIfdir: buf[0] = 'd'; break;
    case kSIflnk: buf[0] = 'l'; break;
    case kSIfchr: buf[0] = 'c'; break;
    case kSIfblk: buf[0] = 'b'; break;
    case kSIfifo: buf[0] = 'p'; break;
    case kSIfsock: buf[0] = 's'; break;
    default: break;
  }
  const char* rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; ++i) {
    buf[1 + i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
  }
  buf[10] = '\0';
  return buf;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

bool IsRawDevicePath(std::string_view path) {
  return StartsWith(path, "\\\\") || StartsWith(path, "//.");
}

uint64_t ParseSize(const std::string& text, bool* ok) {
  *ok = false;
  if (text.empty()) return 0;
  const uint64_t kMax = std::numeric_limits<uint64_t>::max();
  size_t idx = 0;
  uint64_t value = 0;
  while (idx < text.size() && text[idx] >= '0' && text[idx] <= '9') {
    uint64_t digit = static_cast<uint64_t>(text[idx] - '0');
    if (value > (kMax - digit) / 10) return 0;
    value = value * 10 + digit;
    ++idx;
  }
  if (idx == 0) return 0;
  uint64_t multiplier = 1;
  if (idx < text.size()) {
    char suffix = text[idx++];
    switch (suffix) {
      case 'k': case 'K': multiplier = 1024ull; break;
      case 'm': case 'M': multiplier = 1024ull * 1024; break;
      case 'g': case 'G': multiplier = 1024ull * 1024 * 1024; break;
      case 't': case 'T': multiplier = 1024ull * 1024 * 1024 * 1024; break;
      default: return 0;
    }
    if (idx != text.size()) return 0;
  }
  if (value > kMax / multiplier) return 0;
  *ok = true;
  return value * multiplier;
}

// Looks for well-known signatures so a destructive format can warn.
std::string ProbeExistingSignatures(BlockDevice& dev) {
  std::vector<uint8_t> buf(4096, 0);
  size_t to_read = static_cast<size_t>(std::min<uint64_t>(buf.size(), dev.Size()));
  if (to_read < 512 || !dev.Read(0, buf.data(), to_read)) return {};
  auto be32 = [&](size_t off) { return GetBE32(buf.data(), off); };
  auto bytes_eq = [&](size_t off, const char* s, size_t n) {
    return off + n <= buf.size() &&
           std::memcmp(buf.data() + off, s, n) == 0;
  };
  if (be32(0) == 0x58465342) return "an existing XFS filesystem";
  if (bytes_eq(3, "NTFS    ", 8)) return "an existing NTFS filesystem";
  if (to_read > 1082 && LoadLE16(buf.data() + 1080) == 0xEF53) {
    return "an existing ext2/3/4 filesystem";
  }
  if (to_read >= 512 && buf[510] == 0x55 && buf[511] == 0xAA) {
    return "an MBR partition table";
  }
  if (to_read >= 520 && bytes_eq(512, "EFI PART", 8)) {
    return "a GPT partition table";
  }
  if (bytes_eq(0, "LUKS\xBA\xBE", 6)) return "an existing LUKS container";
  if (bytes_eq(3, "EXFAT   ", 8)) return "an existing exFAT filesystem";
  return {};
}

std::wstring ToWide(std::string_view s) {
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), len);
  return out;
}

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

// UTF-8 std::string -> filesystem::path (Windows paths are UTF-16).
std::filesystem::path ToPath(const std::string& utf8) {
  const auto* p = reinterpret_cast<const char8_t*>(utf8.data());
  return std::filesystem::path(std::u8string(p, utf8.size()));
}

// Opens a target (raw device or image) read-only unless `writable`.
std::unique_ptr<BlockDevice> OpenTarget(const std::string& target,
                                        bool writable, bool exclusive,
                                        std::string* error) {
  if (IsRawDevicePath(target)) {
    return OpenRawDevice(ToWide(target), writable, exclusive, error);
  }
  std::filesystem::path path = ToPath(target);
  if (!std::filesystem::exists(path)) {
    if (error) *error = "file not found: " + target;
    return nullptr;
  }
  std::string open_error;
  auto dev = ImageBlockDevice::Open(path, writable, 512, &open_error);
  if (!dev && error) {
    *error = "cannot open image: " + target +
             (open_error.empty() ? "" : " (" + open_error + ")");
  }
  return dev;
}

void PrintError(const ErrorContext& error) {
  std::cerr << "error: " << error.Format() << "\n";
}

void PrintGeometry(const XfsSuperblock& sb, const XfsGeometry& geo) {
  auto flags_on = [](bool b) { return b ? "yes" : "no"; };
  std::cout << "XFS filesystem\n";
  std::cout << "  label            : "
            << (sb.label[0] ? sb.label.data() : "(none)") << "\n";
  std::cout << "  version          : v5 (crc=1)\n";
  std::cout << "  uuid             : " << FormatUuid(sb.uuid) << "\n";
  if (sb.features.meta_uuid) {
    std::cout << "  metadata uuid    : " << FormatUuid(sb.meta_uuid) << "\n";
  }
  std::cout << "  block size       : " << geo.block_size << " bytes\n";
  std::cout << "  sector size      : " << geo.sector_size << " bytes\n";
  std::cout << "  inode size       : " << geo.inode_size << " bytes\n";
  std::cout << "  dir block size   : " << geo.dir_block_size << " bytes\n";
  std::cout << "  data blocks      : " << geo.dblocks << " ("
            << HumanBytes(geo.dblocks * geo.block_size) << ")\n";
  std::cout << "  allocation groups: " << geo.agcount << " x "
            << geo.agblocks << " blocks\n";
  std::cout << "  internal log     : start " << geo.logstart << ", "
            << geo.logblocks << " blocks ("
            << HumanBytes(static_cast<uint64_t>(geo.logblocks) * geo.block_size)
            << ")\n";
  std::cout << "  root inode       : " << geo.rootino << "\n";
  std::cout << "  inodes           : " << sb.icount << " allocated, "
            << sb.ifree << " free\n";
  std::cout << "  free blocks      : " << sb.fdblocks << "\n";
  std::cout << "  features         :"
            << " ftype=" << flags_on(sb.features.ftype)
            << " finobt=" << flags_on(sb.features.finobt)
            << " spinodes=" << flags_on(sb.features.spinodes)
            << " rmapbt=" << flags_on(sb.features.rmapbt)
            << " reflink=" << flags_on(sb.features.reflink)
            << " bigtime=" << flags_on(sb.features.bigtime)
            << " nrext64=" << flags_on(sb.features.nrext64)
            << " parent=" << flags_on(sb.features.parent) << "\n";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CommandList() {
  std::string error;
  auto disks = EnumeratePhysicalDrives(&error);
  if (!error.empty() && disks.empty()) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  std::printf("%-6s %-12s %-6s %-6s %-8s %-18s %s\n", "Drive", "Size",
              "Sector", "Phys", "System", "Model", "Partitions");
  for (const auto& disk : disks) {
    std::string parts;
    for (const auto& p : disk.partitions) {
      if (!parts.empty()) parts += ", ";
      parts += "#" + std::to_string(p.number) + " " +
               HumanBytes(p.length_bytes);
    }
    if (parts.empty()) parts = "(none)";
    std::string model = disk.model.empty() ? disk.device_path : disk.model;
    std::printf("%-6u %-12s %-6u %-6u %-8s %-18s %s%s\n", disk.disk_number,
                HumanBytes(disk.size_bytes).c_str(), disk.logical_sector_size,
                disk.physical_sector_size,
                disk.is_system_disk ? "YES" : "-", model.c_str(),
                parts.c_str(), disk.has_pagefile ? " [pagefile]" : "");
    if (disk.is_system_disk) {
      std::cout << "       \\-> system disk: formatting is refused\n";
    }
  }
  return 0;
}

int CommandInfo(const std::string& target) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto sb = ReadSuperblock(*dev, 0, true);
  if (!sb.ok()) {
    PrintError(sb.error());
    std::cerr << "(device size: " << HumanBytes(dev->Size()) << ", sector "
              << dev->SectorSize() << ")\n";
    return 1;
  }
  XfsGeometry geo = MakeGeometry(sb.value());
  PrintGeometry(sb.value(), geo);
  if (geo.FsbToByte(geo.dblocks) > dev->Size()) {
    std::cout << "  !! filesystem is larger than the device (truncated?)\n";
  }
  Status mountable = CheckMountable(sb.value());
  if (!mountable.ok()) {
    std::cout << "  mountable        : NO - " << mountable.error().Format()
              << "\n";
    return 2;
  }
  std::cout << "  mountable        : yes (read-only, v5 supported subset)\n";
  return 0;
}

int CommandCheck(const std::string& target, const Options& options) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options;
  mount_options.force_unsafe_ro = options.force_unsafe_ro;
  auto fs = XfsFilesystem::Mount(std::move(dev), mount_options);
  if (!fs.ok()) {
    std::cout << "UNSUPPORTED/CORRUPT: " << fs.error().Format() << "\n";
    return 2;
  }
  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  for (const auto& item : report.items) {
    const char* tag = nullptr;
    switch (item.status) {
      case CheckStatus::kOk:
        if (!options.verbose) continue;
        tag = "OK";
        break;
      case CheckStatus::kWarning: tag = "WARNING"; break;
      case CheckStatus::kCorrupt: tag = "CORRUPT"; break;
      case CheckStatus::kUnsupported: tag = "UNSUPPORTED"; break;
    }
    std::cout << tag << ": " << item.name;
    if (!item.message.empty()) std::cout << " - " << item.message;
    std::cout << "\n";
  }
  std::cout << report.Summary() << "\n";
  return report.Overall() == CheckStatus::kCorrupt ||
                 report.Overall() == CheckStatus::kUnsupported
             ? 1
             : 0;
}

int CommandMkImage(const std::string& path_string, const std::string& size) {
  bool ok = false;
  uint64_t bytes = ParseSize(size, &ok);
  if (!ok || bytes < (64ull << 20)) {
    std::cerr << "error: invalid image size; use e.g. 300M or 2G "
                 "(minimum 64 MiB)\n";
    return 1;
  }
  std::filesystem::path path = ToPath(path_string);
  if (std::filesystem::exists(path)) {
    std::cerr << "error: " << path_string << " already exists\n";
    return 1;
  }
  auto device = ImageBlockDevice::Create(path, bytes, 512);
  if (!device) {
    std::cerr << "error: cannot create " << path_string << "\n";
    return 1;
  }
  device->Close();
  std::cout << "created " << path_string << " (" << HumanBytes(bytes) << ")\n";
  return 0;
}

int CommandLs(const std::string& target, const std::string& path) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto fs = XfsFilesystem::Mount(std::move(dev), MountOptions{});
  if (!fs.ok()) {
    PrintError(fs.error());
    return 1;
  }
  auto inode = fs.value()->LookupPath(path);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  auto entries = fs.value()->ListDirectory(inode.value().ino);
  if (!entries.ok()) {
    PrintError(entries.error());
    return 1;
  }
  std::sort(entries.value().begin(), entries.value().end(),
            [](const XfsDirEntry& a, const XfsDirEntry& b) {
              return a.name < b.name;
            });
  for (const auto& entry : entries.value()) {
    if (entry.name == "." || entry.name == "..") continue;
    auto child = fs.value()->OpenInode(entry.ino);
    if (!child.ok()) {
      std::cout << "?---------   ? ?         ? " << entry.name
                << "   [unreadable: " << child.error().message << "]\n";
      continue;
    }
    const XfsInode& ino = child.value();
    WindowsName win = XfsNameToWindows(entry.name);
    std::string name = entry.name;
    if (win.escaped) {
      name += "  (win: " + Utf16ToUtf8(win.name).value_or("?") + ")";
    }
    uint64_t size = ino.IsDir() ? 0 : ino.size;
    std::printf("%s %3u %6u %6u %12llu %s %s\n", ModeString(ino.mode).c_str(),
                ino.nlink, ino.uid, ino.gid,
                static_cast<unsigned long long>(size),
                FormatTime(ino.mtime).c_str(), name.c_str());
    if (ino.IsSymlink()) {
      auto link = fs.value()->ReadSymlink(ino.ino);
      if (link.ok()) {
        std::cout << "    -> " << link.value() << "\n";
      }
    }
  }
  return 0;
}

int CommandCat(const std::string& target, const std::string& path) {
  // Windows stdio defaults to text mode, which would translate every '\n' in
  // file data to CRLF; file contents must be byte-exact.
  _setmode(_fileno(stdout), _O_BINARY);
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto fs = XfsFilesystem::Mount(std::move(dev), MountOptions{});
  if (!fs.ok()) {
    PrintError(fs.error());
    return 1;
  }
  auto inode = fs.value()->LookupPath(path);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  if (inode.value().IsSymlink()) {
    auto link = fs.value()->ReadSymlink(inode.value().ino);
    if (!link.ok()) {
      PrintError(link.error());
      return 1;
    }
    std::cout << link.value() << "\n";
    return 0;
  }
  if (inode.value().IsDir()) {
    std::cerr << "error: " << path << " is a directory\n";
    return 1;
  }
  constexpr size_t kChunk = 1 << 20;
  std::vector<char> buffer(kChunk);
  uint64_t offset = 0;
  while (offset < inode.value().size) {
    size_t want = static_cast<size_t>(
        std::min<uint64_t>(kChunk, inode.value().size - offset));
    auto read = fs.value()->ReadFile(inode.value().ino, offset, buffer.data(),
                                     want);
    if (!read.ok()) {
      PrintError(read.error());
      return 1;
    }
    if (read.value() == 0) break;
    std::cout.write(buffer.data(), static_cast<std::streamsize>(read.value()));
    offset += read.value();
  }
  return 0;
}

int CommandStat(const std::string& target, const std::string& path) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto fs = XfsFilesystem::Mount(std::move(dev), MountOptions{});
  if (!fs.ok()) {
    PrintError(fs.error());
    return 1;
  }
  auto inode = fs.value()->LookupPath(path);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  uint64_t ino = inode.value().ino;
  auto info = fs.value()->GetFileInfo(ino);
  if (!info.ok()) {
    PrintError(info.error());
    return 1;
  }
  const XfsFileInfo& i = info.value();
  std::cout << "  inode    : " << i.ino << "\n";
  std::cout << "  mode     : " << ModeString(i.mode)
            << " (0" << std::oct << (i.mode & 07777) << std::dec << ")\n";
  std::cout << "  uid/gid  : " << i.uid << "/" << i.gid << "\n";
  std::cout << "  links    : " << i.nlink << "\n";
  std::cout << "  size     : " << i.size << " bytes\n";
  std::cout << "  blocks   : " << i.nblocks << "\n";
  std::cout << "  extents  : " << i.nextents << " data, " << i.anextents
            << " attr\n";
  std::cout << "  atime    : " << FormatTime(i.atime) << "\n";
  std::cout << "  mtime    : " << FormatTime(i.mtime) << "\n";
  std::cout << "  ctime    : " << FormatTime(i.ctime) << "\n";
  std::cout << "  crtime   : " << FormatTime(i.crtime) << "\n";
  if (i.immutable || i.append_only) {
    std::cout << "  flags    : " << (i.immutable ? "immutable " : "")
              << (i.append_only ? "append-only" : "") << "\n";
  }
  return 0;
}

int CommandFormat(const std::string& target, const Options& options) {
  std::string error;
  bool is_raw = IsRawDevicePath(target);

  if (StartsWith(target, "\\\\.\\HarddiskVolume") ||
      StartsWith(target, "\\\\.\\Volume")) {
    std::cerr << "error: formatting an individual volume is not supported in "
                 "this version; format the whole physical disk instead\n";
    return 1;
  }

  // Safety analysis first, before opening anything writable.
  if (is_raw) {
    auto disks = EnumeratePhysicalDrives(&error);
    const RawDiskInfo* info = nullptr;
    for (const auto& disk : disks) {
      std::wstring path = L"\\\\.\\PhysicalDrive" +
                          std::to_wstring(disk.disk_number);
      if (_stricmp(WideToUtf8(path).c_str(), target.c_str()) == 0) {
        info = &disk;
      }
    }
    if (!info) {
      std::cerr << "error: cannot find " << target
                << " (use `xfswin list`)\n";
      if (!error.empty()) std::cerr << "       " << error << "\n";
      return 1;
    }
    std::cout << "target: PhysicalDrive" << info->disk_number << " ("
              << (info->model.empty() ? "unknown model" : info->model)
              << "), " << HumanBytes(info->size_bytes) << "\n";
    if (info->is_system_disk) {
      std::cerr << "refusing to format the system disk\n";
      return 3;
    }
    if (info->has_pagefile) {
      std::cerr << "refusing to format a disk that holds a pagefile\n";
      return 3;
    }
    if (!info->partitions.empty()) {
      std::cout << "warning: disk contains " << info->partitions.size()
                << " partition(s); all data on this disk will be destroyed:\n";
      for (const auto& p : info->partitions) {
        std::printf("  partition #%u type %s offset %llu length %s\n", p.number,
                    p.type.c_str(),
                    static_cast<unsigned long long>(p.offset_bytes),
                    HumanBytes(p.length_bytes).c_str());
      }
    }
  } else {
    std::filesystem::path path = ToPath(target);
    bool exists = std::filesystem::exists(path);
    uint64_t size = 0;
    if (exists) {
      size = std::filesystem::file_size(path);
      std::cout << "target: existing file " << target << " ("
                << HumanBytes(size) << "); the file will be overwritten\n";
      auto probe_dev = OpenTarget(target, false, false, &error);
      if (probe_dev) {
        std::string found = ProbeExistingSignatures(*probe_dev);
        if (!found.empty()) {
          std::cout << "warning: the image already contains " << found
                    << "\n";
        }
        probe_dev->Close();
      }
    } else {
      std::cerr << "error: image does not exist; create one first with "
                   "`xfswin mkimage <file> <size>`\n";
      return 1;
    }
  }

  std::cout << "plan: create XFS v5 filesystem (crc=1, internal log, "
               "4KiB blocks, no realtime device)\n";
  if (!options.yes_i_really_mean_it) {
    std::cout << "dry run: pass --yes-i-really-mean-it to actually format\n";
    return 2;
  }

  auto dev = OpenTarget(target, true, true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  if (is_raw) {
    auto* raw = dynamic_cast<WindowsRawBlockDevice*>(dev.get());
    if (!raw) {
      std::cerr << "error: internal error (unexpected device backend)\n";
      return 1;
    }
    std::string lock_error;
    if (!raw->LockVolumes(&lock_error)) {
      std::cerr << "error: cannot obtain exclusive access: " << lock_error
                << "\n       close all programs using this disk and retry\n";
      return 1;
    }
  }

  mkfs::FormatOptions format_options;
  format_options.quiet = !options.verbose && !options.debug;
  auto report = mkfs::Format(*dev, format_options);
  if (!report.ok()) {
    PrintError(report.error());
    return 1;
  }
  const mkfs::FormatReport& r = report.value();
  std::cout << "formatted OK\n";
  std::cout << "  uuid        : " << FormatUuid(r.uuid) << "\n";
  std::cout << "  allocation  : " << r.ag_count << " AGs x "
            << r.ag_size_blocks << " blocks\n";
  std::cout << "  log         : start " << r.log_start << ", "
            << r.log_blocks << " blocks\n";
  std::cout << "  root inode  : " << r.root_ino << "\n";
  std::cout << "  inode chunks: " << r.inode_chunks << "\n";
  return 0;
}

int CommandMount(const std::string& target, const std::string& drive,
                 const Options& options) {
#ifdef XFSWIN_HAS_WINFSP
  if (options.rw_experimental && IsRawDevicePath(target)) {
    std::cerr << "error: writable mounting of real devices is not enabled in "
                 "this version.\n"
                 "       Read-write support is restricted to image files "
                 "(--rw-experimental).\n";
    return 3;
  }
  return winfsp::RunMountHost(target, drive, options.force_unsafe_ro,
                              options.verbose || options.debug,
                              options.rw_experimental);
#else
  (void)target;
  (void)drive;
  (void)options;
  std::cerr << "error: this build was compiled without WinFsp support.\n"
               "Install the WinFsp SDK (WINFSP_ROOT) and reconfigure with "
               "-DXFSWIN_BUILD_WINFSP=ON.\n";
  return 1;
#endif
}

int CommandUnmount(const std::string& drive) {
#ifdef XFSWIN_HAS_WINFSP
  return winfsp::StopMountHost(drive);
#else
  (void)drive;
  std::cerr << "error: this build was compiled without WinFsp support.\n";
  return 1;
#endif
}

// ---------------------------------------------------------------------------
// pwrite: the same in-place write the WinFsp Write callback performs, without
// a mount. This is what lets CI verify the write path against Linux oracles
// on machines where the WinFsp driver is not installed.
// ---------------------------------------------------------------------------
int CommandPwrite(const std::string& target, const std::string& path,
                  const std::string& offset_text, const std::string& text,
                  const std::string& from_file, const Options& options) {
  if (!options.rw_experimental) {
    std::cerr << "error: pwrite modifies the image; pass --rw-experimental to "
                 "confirm (this build only supports overwriting existing "
                 "allocated data, images only)\n";
    return 3;
  }
  if (IsRawDevicePath(target)) {
    std::cerr << "error: pwrite is restricted to image files in this version; "
                 "read-write support for real devices is not enabled yet\n";
    return 3;
  }
  if (text.empty() == from_file.empty()) {
    std::cerr << "error: pass exactly one data source: --text=<string> or "
                 "--from=<file>\n";
    return 1;
  }
  // Strict offset parsing: a typo must not silently become offset 0.
  bool offset_ok = false;
  uint64_t offset = 0;
  if (!offset_text.empty()) {
    const char* begin = offset_text.c_str();
    char* end = nullptr;
    int base = 10;
    if (offset_text.size() > 2 && offset_text[0] == '0' &&
        (offset_text[1] == 'x' || offset_text[1] == 'X')) {
      base = 16;
    }
    offset = std::strtoull(begin, &end, base);
    offset_ok = end != nullptr && *end == '\0' && end != begin;
  }
  if (!offset_ok) {
    std::cerr << "error: <offset> must be a decimal or 0x-prefixed byte "
                 "offset\n";
    return 1;
  }

  std::vector<uint8_t> payload;
  if (!text.empty()) {
    payload.assign(text.begin(), text.end());
  } else {
    std::filesystem::path source = ToPath(from_file);
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(source, ec);
    if (ec || size == 0) {
      std::cerr << "error: cannot read data file " << from_file << "\n";
      return 1;
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
      std::cerr << "error: cannot open data file " << from_file << "\n";
      return 1;
    }
    payload.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
    if (!in) {
      std::cerr << "error: short read from " << from_file << "\n";
      return 1;
    }
  }

  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options;
  mount_options.read_write = true;
  auto fs = XfsFilesystem::Mount(std::move(dev), mount_options);
  if (!fs.ok()) {
    PrintError(fs.error());
    return 1;
  }
  auto inode = fs.value()->LookupPath(path);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  if (inode.value().IsDir()) {
    std::cerr << "error: " << path << " is a directory\n";
    return 1;
  }
  auto written = fs.value()->OverwriteFileData(inode.value().ino, offset,
                                              payload.data(), payload.size());
  if (!written.ok()) {
    PrintError(written.error());
    if (written.error().code == XfsError::kWriteExtendsFile ||
        written.error().code == XfsError::kWriteNeedsAllocation ||
        written.error().code == XfsError::kWriteNeedsConversion) {
      std::cerr << "note: this build can only overwrite bytes that are "
                   "already allocated and written (no growth, no holes, no "
                   "unwritten extents).\n";
    }
    return 1;
  }
  Status flush = fs.value()->Flush();
  if (!flush.ok()) {
    PrintError(flush.error());
    return 1;
  }
  std::cout << "wrote " << written.value() << " bytes at offset " << offset
            << " in " << path << " (inode " << inode.value().ino << ", size "
            << inode.value().size << ")\n";
  return 0;
}

}  // namespace

int Run(const std::vector<std::string>& args) {
  Options options;
  std::vector<std::string> positional;
  std::string pwrite_text;
  std::string pwrite_from;
  bool explicit_rw = false;
  for (const auto& arg : args) {
    if (arg == "--verbose" || arg == "-v") {
      options.verbose = true;
    } else if (arg == "--debug" || arg == "-d") {
      options.debug = true;
      options.verbose = true;
    } else if (arg == "--force-unsafe-ro") {
      options.force_unsafe_ro = true;
    } else if (arg == "--yes-i-really-mean-it") {
      options.yes_i_really_mean_it = true;
    } else if (arg == "--rw-experimental") {
      options.rw_experimental = true;
    } else if (arg == "--rw") {
      explicit_rw = true;
    } else if (StartsWith(arg, "--text=")) {
      pwrite_text = arg.substr(7);
    } else if (StartsWith(arg, "--from=")) {
      pwrite_from = arg.substr(7);
    } else if (arg == "--help" || arg == "-h" || arg == "help") {
      PrintUsage();
      return 0;
    } else if (arg == "--version") {
      std::cout << kVersion << "\n";
      return 0;
    } else {
      positional.push_back(arg);
    }
  }

  // --rw is the eventual name for ordinary read-write mounting. It stays
  // refused until the writer can also allocate, extend and delete: a
  // half-working --rw would silently produce filesystems Linux rejects.
  if (explicit_rw) {
    std::cerr << "error: --rw is not enabled yet. Read-write support is "
                 "experimental and limited to in-place overwrites of existing "
                 "data; use\n"
                 "       xfswin mount <image> <drive> --rw-experimental\n"
                 "       (image files only; real devices stay read-only in "
                 "this version)\n";
    return 3;
  }

  log::SetLevel(options.debug ? log::Level::kDebug
                              : (options.verbose ? log::Level::kInfo
                                                 : log::Level::kWarn));

  if (positional.empty()) {
    PrintUsage();
    return 1;
  }
  const std::string& command = positional[0];

  if (command == "list") return CommandList();
  if (command == "info") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin info <device|image>\n";
      return 1;
    }
    return CommandInfo(positional[1]);
  }
  if (command == "check") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin check <device|image>\n";
      return 1;
    }
    return CommandCheck(positional[1], options);
  }
  if (command == "mkimage") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin mkimage <file> <size>\n";
      return 1;
    }
    return CommandMkImage(positional[1], positional[2]);
  }
  if (command == "ls") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin ls <device|image> <path>\n";
      return 1;
    }
    return CommandLs(positional[1], positional[2]);
  }
  if (command == "cat") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin cat <device|image> <path>\n";
      return 1;
    }
    return CommandCat(positional[1], positional[2]);
  }
  if (command == "stat") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin stat <device|image> <path>\n";
      return 1;
    }
    return CommandStat(positional[1], positional[2]);
  }
  if (command == "pwrite") {
    if (positional.size() != 4) {
      std::cerr << "usage: xfswin pwrite <image> <path> <offset> "
                   "--rw-experimental [--text=<string> | --from=<file>]\n";
      return 1;
    }
    return CommandPwrite(positional[1], positional[2], positional[3],
                         pwrite_text, pwrite_from, options);
  }
  if (command == "format") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin format <device|image> "
                   "[--yes-i-really-mean-it]\n";
      return 1;
    }
    return CommandFormat(positional[1], options);
  }
  if (command == "mount") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin mount <device|image> <drive-letter|directory>\n";
      return 1;
    }
    return CommandMount(positional[1], positional[2], options);
  }
  if (command == "unmount") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin unmount <drive-letter|directory>\n";
      return 1;
    }
    return CommandUnmount(positional[1]);
  }
  if (command == "selftest") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin selftest <device|image> "
                   "[--rw-experimental]\n";
      return 1;
    }
#ifdef XFSWIN_HAS_WINFSP
    {
      if (options.rw_experimental && IsRawDevicePath(positional[1])) {
        std::cerr << "error: the read-write self-test is restricted to image "
                     "files\n";
        return 3;
      }
      std::string error;
      auto dev = OpenTarget(positional[1], options.rw_experimental, false,
                            &error);
      if (!dev) {
        std::cerr << "error: " << error << "\n";
        return 1;
      }
      MountOptions mount_options;
      mount_options.read_write = options.rw_experimental;
      auto fs = XfsFilesystem::Mount(std::move(dev), mount_options);
      if (!fs.ok()) {
        PrintError(fs.error());
        return 1;
      }
      return xfs::winfsp::RunAdapterSelfTest(std::move(fs.value()));
    }
#else
    std::cerr << "error: this build has no WinFsp support\n";
    return 1;
#endif
  }

  std::cerr << "unknown command: " << command << "\n\n";
  PrintUsage();
  return 1;
}

}  // namespace xfs::cli
