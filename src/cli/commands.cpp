// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "cli/cli.h"

#include <memory>
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
#include <optional>
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
#include "xfs/parent.h"
#include "xfs/refcount.h"
#include "xfs/rmap.h"
#include "xfs/superblock.h"
#include "xfs/win_name_map.h"
#include "xfs/perf_stats.h"
#include "xfs/windows_raw_device.h"

#ifdef XFSWIN_HAS_WINFSP
#include "winfsp/mount_host.h"
#include "winfsp/winfsp_adapter.h"
#endif

namespace xfs::cli {
namespace {

std::string JsonEscape(const std::string& text);

constexpr const char* kVersion = "xfswin 0.1.0 (XFS v5 reader, writer, journal recovery)";

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
      "  rmap <device|image> [--ag=N] human-readable reverse mappings\n"
      "  refcount <device|image> [--ag=N]  typed refcountbt records\n"
      "  reflink <image> <src> <dst> <off> <len>  shared-extent clone\n"
      "      --rw-experimental         writable mount (images only; create,\n"
      "                                overwrite, extend, truncate, delete, rename,\n"
      "                                links, symlinks, xattrs and reflinks)\n"
      "  journal <device|image>        dump the internal log structure\n"
      "      --json                    machine-readable (recovery differential)\n"
      "  mkimage <file> <size>         create a sparse test image (e.g. 300M)\n"
      "  ls <device|image> <path>      list a directory [--json]\n"
      "  cat <device|image> <path>     write a file (or symlink target) to stdout\n"
      "  stat <device|image> <path>    show inode information\n"
      "  pwrite <image> <path> <off>   write bytes at an offset (may extend the\n"
      "      --rw-experimental [--text=<s> | --from=<file>]\n"
      "  fallocate <image> <path> <off> <len>  allocate unwritten extents\n"
      "      --rw-experimental\n"
      "  convert-unwritten <image> <path> <off> <len>  mark them written\n"
      "      --rw-experimental\n"
      "  bmap <image> <path>          dump the physical data extents [--json]\n"
      "  xattr list <image> <path>    list native extended attributes [--json]\n"
      "  xattr get <image> <path> <name>  raw value to stdout, or --hex/\n"
      "      --output=<file>          --include-private reads a parent.* value\n"
      "  xattr set <image> <path> <name> [value | --value-file=<f> |\n"
      "      --value-hex=<hex>] --rw-experimental [--create|--replace]\n"
      "  xattr remove <image> <path> <name>  --rw-experimental\n"
      "      --include-private         xattr list: show parent.* records\n"
      "  parents <image> <path>       list reverse parent pointers [--json]\n"
      "  extend <image> <path> <size>  grow a file to <size> bytes by writing a\n"
      "      --rw-experimental [--fill=0xNN]\n"
      "  truncate <image> <path> <size>  set the file size (allocate or free)\n"
      "      --rw-experimental\n"
      "  create <image> <path>         create a regular file\n"
      "      --rw-experimental [--mode=0644]\n"
      "  mkdir <image> <path>          create a directory\n"
      "      --rw-experimental [--mode=0755]\n"
      "  unlink <image> <path>         remove a regular file\n"
      "      --rw-experimental\n"
      "  rmdir <image> <path>          remove an empty directory\n"
      "      --rw-experimental\n"
      "  rename <image> <src> <dst>    atomically rename (file, symlink or dir)\n"
      "      --rw-experimental [--replace]  --replace overwrites an existing\n"
      "                                     file or empty directory target\n"
      "  link <image> <existing> <new> create a hard link\n"
      "      --rw-experimental\n"
      "  symlink <image> <target> <path>  create a symlink (target kept verbatim)\n"
      "      --rw-experimental\n"
      "  format <device|image>         create a new XFS v5 filesystem\n"
      "      [--modern-inode-profile]  enable finobt + inobtcount + sparse\n"
      "                                inodes + bigtime + NREXT64\n"
      "      [--finobt] [--inobtcount] [--sparse-inodes] [--bigtime]\n"
      "      [--nrext64]               select individual RW-6A features\n"
      "      [--rmapbt]                format with rmapbt=1\n"
      "      [--reflink]               format with reflink=1 (refcountbt)\n"
      "      [--rmapbt --reflink]      Linux-legal combined profile\n"
      "                                (both are ordinary writable features)\n"
      "      [--parent]                parent pointers (implies exchange-range)\n"
      "      [--modern-default-profile] the xfsprogs 7.2 default feature set\n"
      "  mount <device|image> <X:>     mount via WinFsp (read-only by default)\n"
      "      --rw-experimental         writable mount (images only; create,\n"
      "                                overwrite, extend, truncate, delete, rename,\n"
      "                                links, symlinks, xattrs and reflinks)\n"
      "  unmount <X:>                  stop an xfswin mount\n"
      "\n"
      "global options:\n"
      "  --verbose                     informational output\n"
      "  --debug                       detailed parser traversal output\n"
      "  --perf-stats                  collect I/O, journal, cache and lock counters\n"
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
            << " inobtcnt=" << flags_on(sb.features.inobtcnt)
            << " spinodes=" << flags_on(sb.features.spinodes)
            << " rmapbt=" << flags_on(sb.features.rmapbt)
            << " reflink=" << flags_on(sb.features.reflink)
            << " bigtime=" << flags_on(sb.features.bigtime)
            << " nrext64=" << flags_on(sb.features.nrext64)
            << " exchrange=" << flags_on(sb.features.exchrange)
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

// ---------------------------------------------------------------------------
// journal: read-only structural dump of the internal log. This is the input
// of the recovery differential (tests/tools/recovery_probe.py and
// xfs_logprint), and the first thing to look at when recovery refuses.
// ---------------------------------------------------------------------------
namespace {

std::string LsnString(uint64_t lsn) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "0x%llx",
                static_cast<unsigned long long>(lsn));
  return buffer;
}

const char* ItemTypeName(uint16_t type) {
  const char* name = LogItemTypeName(type);
  return name != nullptr ? name : "unknown";
}

void PrintJsonString(const std::string& text) {
  std::cout << '"';
  for (char c : text) {
    switch (c) {
      case '"': std::cout << "\\\""; break;
      case '\\': std::cout << "\\\\"; break;
      case '\n': std::cout << "\\n"; break;
      default: std::cout << c; break;
    }
  }
  std::cout << '"';
}

void PrintJournalText(const LogScan& scan, bool verbose) {
  std::cout << "state: " << ToString(scan.state) << " (" << scan.detail << ")\n";
  std::cout << "head: block=" << scan.head_block << " cycle="
            << scan.head_cycle << "\n";
  std::cout << "tail: block=" << scan.tail_block << " cycle="
            << scan.tail_cycle << "\n";
  std::cout << "records: " << scan.records.size() << "\n";
  for (const LogRecordInfo& record : scan.records) {
    std::cout << "record lsn=" << LsnString(record.lsn)
              << " block=" << record.block << " cycle=" << record.cycle
              << " len=" << record.data_blocks * 512 << " ops="
              << record.num_ops << " prev=" << record.prev_block
              << " tail_lsn=" << LsnString(record.tail_lsn) << "\n";
  }
  std::cout << "transactions: " << scan.transactions.size() << "\n";
  uint64_t item_count = 0;
  for (const RecoveredTransaction& trans : scan.transactions) {
    item_count += trans.items.size();
    std::cout << "transaction tid=" << trans.tid
              << " lsn=" << LsnString(trans.lsn)
              << " items=" << trans.items.size() << "\n";
    for (const RecoveredItem& item : trans.items) {
      std::cout << "  item " << ItemTypeName(item.type) << " type=0x"
                << std::hex << item.type << std::dec
                << " regions=" << item.regions.size();
      if (item.type == logfmt::kItemBuf && item.regions[0].size() >= 20) {
        const uint8_t* p = item.regions[0].data();
        const uint16_t blf_size = LoadLE16(p + 2);
        const uint16_t flags = LoadLE16(p + 4);
        const uint16_t len = LoadLE16(p + 6);
        const uint32_t lo = LoadLE32(p + 8);
        const uint32_t hi = LoadLE32(p + 12);
        const uint32_t map_size = LoadLE32(p + 16);
        std::cout << " blkno=" << ((static_cast<uint64_t>(hi) << 32) | lo)
                  << " len=" << len << " flags=0x" << std::hex << flags
                  << std::dec << " type_id="
                  << ((flags >> logfmt::kBlftShift) & logfmt::kBlftMask)
                  << " blf_size=" << blf_size << " map_size=" << map_size;
        if (flags & logfmt::kBlfCancel) {
          std::cout << " cancel";
        }
      } else if (item.type == logfmt::kItemInode && item.regions[0].size() >= 56) {
        const uint8_t* p = item.regions[0].data();
        const uint32_t fields = LoadLE32(p + logfmt::kIlfFields);
        const uint64_t ino = static_cast<uint64_t>(LoadLE32(p + logfmt::kIlfIno)) |
                             (static_cast<uint64_t>(LoadLE32(p + logfmt::kIlfIno + 4)) << 32);
        const uint32_t boffset = LoadLE32(p + logfmt::kIlfBoffset);
        const uint32_t lo = LoadLE32(p + logfmt::kIlfBlkno);
        const uint32_t hi = LoadLE32(p + logfmt::kIlfBlkno + 4);
        const uint32_t blen = LoadLE32(p + logfmt::kIlfLen);
        std::cout << " ino=" << ino << " fields=0x" << std::hex << fields
                  << std::dec << " blkno="
                  << ((static_cast<uint64_t>(hi) << 32) | lo)
                  << " len=" << blen << " boffset=" << boffset;
      } else if (item.type == logfmt::kItemIcreate && item.regions[0].size() >= 28) {
        const uint8_t* p = item.regions[0].data();
        std::cout << " ag=" << GetBE32(p, logfmt::kIclAg)
                  << " agbno=" << GetBE32(p, logfmt::kIclAgbno)
                  << " count=" << GetBE32(p, logfmt::kIclCount)
                  << " length=" << GetBE32(p, logfmt::kIclLength);
      } else if ((item.type == logfmt::kItemEfi ||
                  item.type == logfmt::kItemEfd ||
                  item.type == logfmt::kItemBui ||
                  item.type == logfmt::kItemBud) &&
                 item.regions[0].size() >= logfmt::kIntentLogFormatSize) {
        const uint8_t* p = item.regions[0].data();
        uint64_t id = 0;
        for (int byte = 7; byte >= 0; --byte) {
          id = (id << 8) | p[logfmt::kIntentIdOffset + byte];
        }
        std::cout << " id=" << id << " extents="
                  << LoadLE32(p + logfmt::kIntentNextentsOffset);
      }
      std::cout << "\n";
      if (verbose) {
        for (size_t r = 1; r < item.regions.size(); ++r) {
          std::cout << "    region " << r << " bytes="
                    << item.regions[r].size() << "\n";
        }
      }
    }
  }
  std::cout << "items: " << item_count << "\n";
  std::cout << "unsupported: " << scan.unsupported_items.size() << "\n";
  for (const std::string& item : scan.unsupported_items) {
    std::cout << "  " << item << "\n";
  }
  for (const std::string& warning : scan.warnings) {
    std::cout << "warning: " << warning << "\n";
  }
}

void PrintJournalJson(const LogScan& scan) {
  std::cout << "{\n  \"state\": ";
  PrintJsonString(ToString(scan.state));
  std::cout << ",\n  \"head\": {\"block\": " << scan.head_block
            << ", \"cycle\": " << scan.head_cycle << "},\n";
  std::cout << "  \"tail\": {\"block\": " << scan.tail_block
            << ", \"cycle\": " << scan.tail_cycle << "},\n";
  std::cout << "  \"newest_lsn\": \"" << LsnString(scan.newest_lsn) << "\",\n";
  std::cout << "  \"records\": [";
  for (size_t i = 0; i < scan.records.size(); ++i) {
    const LogRecordInfo& record = scan.records[i];
    std::cout << (i == 0 ? "" : ",") << "\n    {\"lsn\": \""
              << LsnString(record.lsn) << "\", \"block\": " << record.block
              << ", \"cycle\": " << record.cycle
              << ", \"len\": " << record.data_blocks * 512
              << ", \"ops\": " << record.num_ops
              << ", \"prev\": " << record.prev_block << ", \"tail_lsn\": \""
              << LsnString(record.tail_lsn) << "\"}";
  }
  std::cout << "\n  ],\n  \"transactions\": [";
  for (size_t i = 0; i < scan.transactions.size(); ++i) {
    const RecoveredTransaction& trans = scan.transactions[i];
    std::cout << (i == 0 ? "" : ",") << "\n    {\"tid\": " << trans.tid
              << ", \"lsn\": \"" << LsnString(trans.lsn) << "\", \"items\": [";
    for (size_t j = 0; j < trans.items.size(); ++j) {
      const RecoveredItem& item = trans.items[j];
      std::cout << (j == 0 ? "" : ", ") << "{\"type\": ";
      PrintJsonString(ItemTypeName(item.type));
      std::cout << ", \"type_id\": " << item.type
                << ", \"regions\": " << item.regions.size();
      if (item.type == logfmt::kItemBuf && item.regions[0].size() >= 20) {
        const uint8_t* p = item.regions[0].data();
        const uint16_t flags = LoadLE16(p + 4);
        const uint32_t map_size = LoadLE32(p + 16);
        std::cout << ", \"blkno\": "
                  << ((static_cast<uint64_t>(LoadLE32(p + 12)) << 32) |
                      LoadLE32(p + 8))
                  << ", \"len\": " << LoadLE16(p + 6) << ", \"flags\": "
                  << flags << ", \"blft\": "
                  << ((flags >> logfmt::kBlftShift) & logfmt::kBlftMask)
                  << ", \"blf_size\": " << LoadLE16(p + 2)
                  << ", \"map_size\": " << map_size
                  << ", \"cancel\": "
                  << ((flags & logfmt::kBlfCancel) ? "true" : "false")
                  << ", \"format_bytes\": "
                  << logfmt::kBufLogFormatSize +
                         static_cast<size_t>(map_size) * 4;
      } else if (item.type == logfmt::kItemInode &&
                 item.regions[0].size() >= 56) {
        const uint8_t* p = item.regions[0].data();
        std::cout << ", \"ino\": "
                  << ((static_cast<uint64_t>(LoadLE32(p + logfmt::kIlfIno + 4))
                       << 32) |
                      LoadLE32(p + logfmt::kIlfIno))
                  << ", \"fields\": "
                  << LoadLE32(p + logfmt::kIlfFields)
                  << ", \"blkno\": "
                  << ((static_cast<uint64_t>(LoadLE32(p + logfmt::kIlfBlkno + 4))
                       << 32) |
                      LoadLE32(p + logfmt::kIlfBlkno))
                  << ", \"boffset\": " << LoadLE32(p + logfmt::kIlfBoffset);
      } else if (item.type == logfmt::kItemIcreate &&
                 item.regions[0].size() >= 28) {
        const uint8_t* p = item.regions[0].data();
        std::cout << ", \"ag\": " << GetBE32(p, logfmt::kIclAg)
                  << ", \"agbno\": " << GetBE32(p, logfmt::kIclAgbno)
                  << ", \"count\": " << GetBE32(p, logfmt::kIclCount);
      } else if ((item.type == logfmt::kItemEfi ||
                  item.type == logfmt::kItemEfd ||
                  item.type == logfmt::kItemBui ||
                  item.type == logfmt::kItemBud ||
                  item.type == logfmt::kItemRui ||
                  item.type == logfmt::kItemRud ||
                  item.type == logfmt::kItemCui ||
                  item.type == logfmt::kItemCud) &&
                 item.regions[0].size() >= logfmt::kIntentLogFormatSize) {
        const uint8_t* p = item.regions[0].data();
        uint64_t id = 0;
        for (int byte = 7; byte >= 0; --byte) {
          id = (id << 8) | p[logfmt::kIntentIdOffset + byte];
        }
        std::cout << ", \"id\": " << id;
        if (item.type == logfmt::kItemCui) {
          const uint32_t nextents = LoadLE32(p + logfmt::kIntentNextentsOffset);
          std::cout << ", \"nextents\": " << nextents << ", \"extents\": [";
          for (uint32_t ext_i = 0; ext_i < nextents; ++ext_i) {
            const size_t base = logfmt::kIntentLogFormatSize + ext_i * 16;
            if (base + 16 > item.regions[0].size()) break;
            const uint8_t* e = p + base;
            std::cout << (ext_i == 0 ? "" : ", ") << "{\"startblock\": "
                      << (static_cast<uint64_t>(LoadLE32(e + 0)) |
                          (static_cast<uint64_t>(LoadLE32(e + 4)) << 32))
                      << ", \"len\": " << LoadLE32(e + 8)
                      << ", \"op\": " << (LoadLE32(e + 12) & 0xff) << "}";
          }
          std::cout << "]";
        }
      }
      std::cout << "}";
    }
    std::cout << "]}";
  }
  std::cout << "\n  ],\n  \"unsupported\": [";
  for (size_t i = 0; i < scan.unsupported_items.size(); ++i) {
    std::cout << (i == 0 ? "" : ", ");
    PrintJsonString(scan.unsupported_items[i]);
  }
  std::cout << "]\n}\n";
}

}  // namespace

int CommandJournal(const std::string& target, const Options& options) {
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/false, /*exclusive=*/false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto sb = ReadSuperblock(*dev, 0, true);
  if (!sb.ok()) {
    PrintError(sb.error());
    return 1;
  }
  XfsGeometry geo = MakeGeometry(sb.value());
  auto scan = ScanLog(*dev, sb.value(), geo);
  if (!scan.ok()) {
    PrintError(scan.error());
    return 1;
  }
  if (options.json) {
    PrintJournalJson(scan.value());
  } else {
    PrintJournalText(scan.value(), options.verbose || options.debug);
  }
  return scan.value().state == LogState::kUnknown ? 2 : 0;
}

int CommandRmap(const std::string& target, int only_ag) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto sb = ReadSuperblock(*dev, 0, true);
  if (!sb.ok()) {
    PrintError(sb.error());
    return 1;
  }
  const XfsGeometry geo = MakeGeometry(sb.value());
  if (!geo.features.rmapbt) {
    std::cout << "rmapbt: disabled\n";
    return 0;
  }
  if (only_ag >= static_cast<int>(geo.agcount)) {
    std::cerr << "error: AG " << only_ag << " is out of range\n";
    return 1;
  }
  int failures = 0;
  for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
    if (only_ag >= 0 && static_cast<uint32_t>(only_ag) != agno) continue;
    auto tree = LoadRmapTree(*dev, geo, agno, sb.value().lsn);
    if (!tree.ok()) {
      std::cout << "AG " << agno << ": CORRUPT - "
                << tree.error().Format() << "\n";
      ++failures;
      continue;
    }
    std::cout << "AG " << agno << "\n"
              << "  level: " << tree.value().level << "\n"
              << "  blocks: " << tree.value().blocks << "\n"
              << "  records: " << tree.value().records.size() << "\n";
    for (const RmapRecord& r : tree.value().records) {
      std::cout << "  block " << r.key.startblock << ".."
                << (r.EndBlock() - 1) << " owner "
                << RmapOwnerName(r.key.owner);
      if (!RmapNonInodeOwner(r.key.owner)) {
        std::cout << " offset " << r.key.offset;
        if (r.key.flags & kRmapAttrFork) std::cout << " attr";
        else std::cout << " data";
        if (r.key.flags & kRmapBmbtBlock) std::cout << " bmbt";
        if (r.unwritten()) std::cout << " unwritten";
        else std::cout << " written";
      }
      std::cout << "\n";
    }
  }
  return failures == 0 ? 0 : 1;
}

int CommandRefcount(const std::string& target, int only_ag) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  auto sb = ReadSuperblock(*dev, 0, true);
  if (!sb.ok()) {
    PrintError(sb.error());
    return 1;
  }
  const XfsGeometry geo = MakeGeometry(sb.value());
  if (!geo.features.reflink) {
    std::cout << "reflink: disabled\n";
    return 0;
  }
  if (only_ag >= static_cast<int>(geo.agcount)) {
    std::cerr << "error: AG " << only_ag << " is out of range\n";
    return 1;
  }
  int failures = 0;
  for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
    if (only_ag >= 0 && static_cast<uint32_t>(only_ag) != agno) continue;
    auto tree = LoadRefcountTree(*dev, geo, agno, sb.value().lsn);
    if (!tree.ok()) {
      std::cout << "AG " << agno << ": CORRUPT - "
                << tree.error().Format() << "\n";
      ++failures;
      continue;
    }
    std::cout << "AG " << agno << "\n"
              << "  root: " << tree.value().root << "\n"
              << "  level: " << tree.value().level << "\n"
              << "  blocks: " << tree.value().blocks << "\n"
              << "  records: " << tree.value().records.size() << "\n";
    for (const RefcountRecord& r : tree.value().records) {
      std::cout << "  block " << r.key.startblock << ".."
                << (r.EndBlock() - 1) << " "
                << (r.cow() ? "cow" : "shared") << " refcount "
                << r.refcount << "\n";
    }
  }
  return failures == 0 ? 0 : 1;
}

int CommandCheck(const std::string& target, const Options& options) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options;
  // `check` never writes: mount the pre-recovery view if the log is dirty (or
  // not parseable) and report the journal state explicitly below.
  mount_options.force_unsafe_ro = true;
  auto fs = XfsFilesystem::Mount(std::move(dev), mount_options);
  if (!fs.ok()) {
    std::cout << "UNSUPPORTED/CORRUPT: " << fs.error().Format() << "\n";
    return 2;
  }
  auto journal = fs.value()->AnalyzeJournal();
  if (!journal.ok()) {
    std::cout << "journal: cannot be analysed - " << journal.error().Format()
              << "\n";
  } else if (journal.value().needed) {
    const auto& summary = journal.value();
    std::cout << "journal: " << ToString(summary.state)
              << " - recovery required (recoverable transactions="
              << summary.recoverable_transactions
              << ", items=" << summary.items;
    if (!summary.unsupported_items.empty()) {
      std::cout << ", unsupported items="
                << summary.unsupported_items.size() << ": "
                << summary.unsupported_items.front();
    }
    std::cout << ")\n";
  } else {
    std::cout << "journal: " << ToString(journal.value().state) << " - "
              << journal.value().detail << "\n";
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

// `parents`: show the reverse namespace records of one inode.  This is the
// differential interface against xfs_db ("attr"), xfs_io -c getparents and
// xfs_scrub's parent checker.
int CommandParents(const std::string& target, const std::string& path,
                   const Options& options) {
  std::string error;
  auto dev = OpenTarget(target, false, false, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options;
  // Diagnostic reads must work on a crash image before recovery.
  mount_options.force_unsafe_ro = true;
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
  auto parents =
      ListParents(fs.value()->device(), fs.value()->geometry(), inode.value());
  if (!parents.ok()) {
    PrintError(parents.error());
    return 1;
  }
  if (options.json) {
    std::cout << "{\"inode\":" << inode.value().ino << ",\"parents\":[";
    bool first = true;
    for (const ParentPointer& p : parents.value()) {
      std::cout << (first ? "" : ",") << "\n  {\"parent\":"
                << p.parent_ino << ",\"generation\":" << p.parent_gen
                << ",\"name\":\"" << JsonEscape(p.name) << "\"}";
      first = false;
    }
    std::cout << (first ? "]}\n" : "\n]}\n");
    return 0;
  }
  std::cout << "inode " << inode.value().ino << " has "
            << parents.value().size() << " parent pointer(s)\n";
  for (const ParentPointer& p : parents.value()) {
    std::cout << "  parent " << p.parent_ino << " gen " << p.parent_gen
              << " name '" << p.name << "'\n";
  }
  return 0;
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

int CommandLs(const std::string& target, const std::string& path,
              const Options& options) {
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
  bool first_json = true;
  if (options.json) std::cout << "[";
  for (const auto& entry : entries.value()) {
    if (entry.name == "." || entry.name == "..") continue;
    auto child = fs.value()->OpenInode(entry.ino);
    if (!child.ok()) {
      if (options.json) {
        if (!first_json) std::cout << ",";
        first_json = false;
        std::cout << "{\"name\":";
        PrintJsonString(entry.name);
        std::cout << ",\"inode\":" << entry.ino
                  << ",\"unreadable\":true}";
      } else {
        std::cout << "?---------   ? ?         ? " << entry.name
                  << "   [unreadable: " << child.error().message << "]\n";
      }
      continue;
    }
    const XfsInode& ino = child.value();
    WindowsName win = XfsNameToWindows(entry.name);
    std::string name = entry.name;
    if (win.escaped) {
      name += "  (win: " + Utf16ToUtf8(win.name).value_or("?") + ")";
    }
    uint64_t size = ino.IsDir() ? 0 : ino.size;
    std::optional<std::string> link_target;
    if (ino.IsSymlink()) {
      auto link = fs.value()->ReadSymlink(ino.ino);
      if (link.ok()) link_target = link.value();
    }
    if (options.json) {
      if (!first_json) std::cout << ",";
      first_json = false;
      std::cout << "{\"name\":";
      PrintJsonString(entry.name);
      std::cout << ",\"inode\":" << ino.ino << ",\"mode\":" << ino.mode
                << ",\"nlink\":" << ino.nlink << ",\"size\":" << size
                << ",\"ftype\":" << static_cast<unsigned>(entry.ftype)
                << ",\"dir\":" << (ino.IsDir() ? "true" : "false")
                << ",\"symlink\":" << (ino.IsSymlink() ? "true" : "false")
                << ",\"target\":";
      if (link_target.has_value()) {
        PrintJsonString(*link_target);
      } else {
        std::cout << "null";
      }
      std::cout << "}";
      continue;
    }
    std::printf("%s %3u %6u %6u %12llu %s %s\n", ModeString(ino.mode).c_str(),
                ino.nlink, ino.uid, ino.gid,
                static_cast<unsigned long long>(size),
                FormatTime(ino.mtime).c_str(), name.c_str());
    if (link_target.has_value()) std::cout << "    -> " << *link_target << "\n";
  }
  if (options.json) std::cout << "]\n";
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
  std::cout << "  blocks   : " << i.nblocks << " (filesystem blocks)\n";
  std::cout << "  extents  : " << i.nextents << " data, " << i.anextents
            << " attr\n";
  std::cout << "  atime    : " << FormatTime(i.atime) << "\n";
  std::cout << "  mtime    : " << FormatTime(i.mtime) << "\n";
  std::cout << "  ctime    : " << FormatTime(i.ctime) << "\n";
  std::cout << "  crtime   : " << FormatTime(i.crtime) << "\n";
  if ((i.mode & kSIfmt) == kSIflnk) {
    auto link = fs.value()->ReadSymlink(ino);
    if (link.ok()) {
      std::cout << "  target   : " << link.value() << "\n";
    }
  }
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
  format_options.finobt = options.finobt;
  format_options.inobtcount = options.inobtcount;
  format_options.sparse_inodes = options.sparse_inodes;
  format_options.bigtime = options.bigtime;
  format_options.nrext64 = options.nrext64;
  format_options.rmapbt = options.rmapbt;
  format_options.reflink = options.reflink;
  format_options.parent_pointers = options.parent_pointers;
  format_options.modern_default_profile = options.modern_default_profile;
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
  // Safety policy, enforced regardless of whether this build has WinFsp:
  // read-write mounting is restricted to image files until the writer has
  // passed crash-consistency and interoperability testing.
  if (options.rw_experimental && IsRawDevicePath(target)) {
    std::cerr << "error: writable mounting of real devices is not enabled in "
                 "this version.\n"
                 "       Read-write support is restricted to image files "
                 "(--rw-experimental).\n";
    return 3;
  }
#ifdef XFSWIN_HAS_WINFSP
  return winfsp::RunMountHost(target, drive, options.force_unsafe_ro,
                              options.verbose || options.debug,
                              options.rw_experimental, options.perf_stats,
                              options.rmapbt_qualification,
                              options.reflink_qualification);
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
// pwrite: the same write the WinFsp Write callback performs, without a mount.
// This is what lets CI verify the write path against Linux oracles on machines
// where the WinFsp driver is not installed.
// ---------------------------------------------------------------------------
namespace {

bool ParseU64(const std::string& text, uint64_t* out) {
  if (text.empty()) return false;
  const char* begin = text.c_str();
  char* end = nullptr;
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
  }
  *out = std::strtoull(begin, &end, base);
  return end != nullptr && *end == '\0' && end != begin;
}

// Every ordinary mutation opens a writable mount through this helper.  The
// legacy qualification flags are forwarded as accepted no-ops; the ordinary
// --rw-experimental flag is the real opt-in for every writable feature.
MountOptions WritableMountOptions(const Options& options) {
  MountOptions mount_options;
  mount_options.read_write = true;
  mount_options.perf_stats = options.perf_stats;
  mount_options.allow_rmapbt_qualification =
      options.rmapbt_qualification;
  mount_options.allow_reflink_qualification =
      options.reflink_qualification;
  mount_options.allow_parent_qualification =
      options.parent_qualification;
  return mount_options;
}

}  // namespace
int CommandPwrite(const std::string& target, const std::string& path,
                  const std::string& offset_text, const std::string& text,
                  const std::string& from_file, const Options& options) {
  if (!options.rw_experimental) {
    std::cerr << "error: pwrite modifies the image; pass --rw-experimental to confirm (image files only)\n";
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
  MountOptions mount_options = WritableMountOptions(options);
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
  auto written = fs.value()->WriteFileData(inode.value().ino, offset,
                                          payload.data(), payload.size());
  if (!written.ok()) {
    PrintError(written.error());
    return 1;
  }
  auto info = fs.value()->GetFileInfo(inode.value().ino);
  // Close the journal with an unmount record, then push everything to stable
  // storage: the next mount (Linux or this build) must find a clean log.
  Status closed = fs.value()->WriteCleanUnmountRecord();
  if (!closed.ok()) {
    PrintError(closed.error());
    return 1;
  }
  Status flush = fs.value()->Flush();
  if (!flush.ok()) {
    PrintError(flush.error());
    return 1;
  }
  std::cout << "wrote " << written.value() << " bytes at offset " << offset
            << " in " << path << " (inode " << inode.value().ino << ", size "
            << (info.ok() ? info.value().size : 0) << ")\n";
  return 0;
}

// extend: grow a file to a given size by writing a deterministic pattern at the
// old EOF. This is the user-visible form of the RW-2 write path (allocation +
// extent map + size, all through transactions).
int CommandExtend(const std::string& target, const std::string& path,
                  const std::string& size_text, uint8_t fill,
                  bool have_fill, const Options& options) {
  if (!options.rw_experimental) {
    std::cerr << "error: extend modifies the image; pass --rw-experimental\n";
    return 3;
  }
  if (IsRawDevicePath(target)) {
    std::cerr << "error: extend is restricted to image files in this version\n";
    return 3;
  }
  uint64_t new_size = 0;
  if (!ParseU64(size_text, &new_size)) {
    std::cerr << "error: <size> must be a decimal or 0x-prefixed byte count\n";
    return 1;
  }
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options = WritableMountOptions(options);
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
  if (!inode.value().IsRegular()) {
    std::cerr << "error: " << path << " is not a regular file\n";
    return 1;
  }
  if (new_size <= inode.value().size) {
    std::cerr << "error: file is already " << inode.value().size
              << " bytes; extend needs a larger size\n";
    return 1;
  }
  const uint64_t offset = inode.value().size;
  std::vector<uint8_t> payload(static_cast<size_t>(new_size - offset));
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = have_fill
                     ? fill
                     : static_cast<uint8_t>(((offset + i) * 131u + 7u) & 0xFFu);
  }
  auto written = fs.value()->WriteFileData(inode.value().ino, offset,
                                           payload.data(), payload.size());
  if (!written.ok()) {
    PrintError(written.error());
    return 1;
  }
  auto info = fs.value()->GetFileInfo(inode.value().ino);
  Status closed = fs.value()->WriteCleanUnmountRecord();
  if (!closed.ok()) {
    PrintError(closed.error());
    return 1;
  }
  if (!fs.value()->Flush().ok()) {
    std::cerr << "error: flush failed\n";
    return 1;
  }
  std::cout << "extended " << path << " (inode " << inode.value().ino << ") to "
            << (info.ok() ? info.value().size : new_size) << " bytes\n";
  return 0;
}

// truncate: SetFileSize, in both directions.
int CommandTruncate(const std::string& target, const std::string& path,
                    const std::string& size_text, const Options& options) {
  if (!options.rw_experimental) {
    std::cerr << "error: truncate modifies the image; pass --rw-experimental\n";
    return 3;
  }
  if (IsRawDevicePath(target)) {
    std::cerr << "error: truncate is restricted to image files in this "
                 "version\n";
    return 3;
  }
  uint64_t new_size = 0;
  if (!ParseU64(size_text, &new_size)) {
    std::cerr << "error: <size> must be a decimal or 0x-prefixed byte count\n";
    return 1;
  }
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options = WritableMountOptions(options);
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
  auto info = fs.value()->SetFileSize(inode.value().ino, new_size);
  if (!info.ok()) {
    PrintError(info.error());
    return 1;
  }
  Status closed = fs.value()->WriteCleanUnmountRecord();
  if (!closed.ok()) {
    PrintError(closed.error());
    return 1;
  }
  if (!fs.value()->Flush().ok()) {
    std::cerr << "error: flush failed\n";
    return 1;
  }
  std::cout << "resized " << path << " (inode " << inode.value().ino << ") to "
            << info.value().size << " bytes\n";
  return 0;
}

namespace {

// Shared front matter for fallocate/convert-unwritten: byte range arguments,
// opt-in flag, image only, exclusive writable mount.
int ParseRangeArguments(const std::string& target, const std::string& path,
                        const std::string& offset_text,
                        const std::string& length_text, const Options& options,
                        uint64_t* offset, uint64_t* length) {
  if (!options.rw_experimental) {
    std::cerr << "error: this command modifies the image; pass "
                 "--rw-experimental\n";
    return 3;
  }
  if (IsRawDevicePath(target)) {
    std::cerr << "error: this command is restricted to image files in this "
                 "version\n";
    return 3;
  }
  if (!ParseU64(offset_text, offset) || !ParseU64(length_text, length)) {
    std::cerr << "error: <offset> and <length> must be decimal or 0x-prefixed "
                 "byte counts\n";
    return 1;
  }
  if (*length == 0) {
    std::cerr << "error: <length> must be nonzero\n";
    return 1;
  }
  (void)path;
  return 0;
}

}  // namespace

int CommandFallocate(const std::string& target, const std::string& path,
                     const std::string& offset_text,
                     const std::string& length_text, const Options& options) {
  uint64_t offset = 0, length = 0;
  int parsed = ParseRangeArguments(target, path, offset_text, length_text,
                                   options, &offset, &length);
  if (parsed != 0) return parsed;
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options = WritableMountOptions(options);
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
  auto info = fs.value()->AllocateUnwritten(inode.value().ino, offset, length);
  if (!info.ok()) {
    PrintError(info.error());
    return 1;
  }
  Status closed = fs.value()->WriteCleanUnmountRecord();
  if (!closed.ok()) {
    PrintError(closed.error());
    return 1;
  }
  if (!fs.value()->Flush().ok()) {
    std::cerr << "error: flush failed\n";
    return 1;
  }
  std::cout << "preallocated " << length << " unwritten bytes at " << offset
            << " in " << path << " (inode " << inode.value().ino
            << ", size " << info.value().size << ")\n";
  return 0;
}

int CommandConvertUnwritten(const std::string& target,
                            const std::string& path,
                            const std::string& offset_text,
                            const std::string& length_text,
                            const Options& options) {
  uint64_t offset = 0, length = 0;
  int parsed = ParseRangeArguments(target, path, offset_text, length_text,
                                   options, &offset, &length);
  if (parsed != 0) return parsed;
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options = WritableMountOptions(options);
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
  auto info = fs.value()->ConvertUnwritten(inode.value().ino, offset, length);
  if (!info.ok()) {
    PrintError(info.error());
    return 1;
  }
  Status closed = fs.value()->WriteCleanUnmountRecord();
  if (!closed.ok()) {
    PrintError(closed.error());
    return 1;
  }
  if (!fs.value()->Flush().ok()) {
    std::cerr << "error: flush failed\n";
    return 1;
  }
  std::cout << "converted unwritten range [" << offset << ", "
            << (offset + length) << ") in " << path << " (inode "
            << inode.value().ino << ", size " << info.value().size << ")\n";
  return 0;
}

// bmap: print the physical data-fork extents. The free-block-reuse and
// structure-transition matrices use this to prove that a freed block was
// actually reused by a different owner.
int CommandBmap(const std::string& target, const std::string& path,
                const Options& options) {
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
  auto extents = fs.value()->GetDataExtents(inode.value().ino);
  if (!extents.ok()) {
    PrintError(extents.error());
    return 1;
  }
  if (options.json) {
    std::cout << "{\"inode\":" << inode.value().ino << ",\"size\":"
              << inode.value().size << ",\"extents\":[";
    for (size_t i = 0; i < extents.value().size(); ++i) {
      const XfsExtent& e = extents.value()[i];
      if (i != 0) std::cout << ",";
      std::cout << "{\"startoff\":" << e.startoff
                << ",\"startblock\":" << e.startblock
                << ",\"blockcount\":" << e.blockcount
                << ",\"unwritten\":" << (e.unwritten ? "true" : "false")
                << "}";
    }
    std::cout << "]}\n";
    return 0;
  }
  std::cout << "inode " << inode.value().ino << ", size "
            << inode.value().size << ", " << extents.value().size()
            << " extents\n";
  for (const XfsExtent& e : extents.value()) {
    std::cout << "  logical " << e.startoff << " physical " << e.startblock
              << " blocks " << e.blockcount
              << (e.unwritten ? " unwritten\n" : " written\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// RW-3 namespace commands: create, mkdir, unlink, rmdir. They drive the same
// transactions the WinFsp callbacks use, so CI can verify them against Linux
// without a driver.
// ---------------------------------------------------------------------------
namespace {

bool ParseOctal(const std::string& text, uint32_t* out) {
  if (text.empty() || text.size() > 6) return false;
  uint32_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '7') return false;
    value = value * 8 + static_cast<uint32_t>(c - '0');
  }
  *out = value;
  return true;
}

// Splits an XFS path into (parent inode, final component) for the namespace
// API. The root itself cannot be created or removed.
Result<std::pair<uint64_t, std::string>> ParentAndName(XfsFilesystem& fs,
                                                       std::string_view path) {
  if (path.empty() || path == "/" || path.back() == '/') {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "path must name a file or directory (no trailing '/')")
        .Metadata("path");
  }
  const size_t slash = path.rfind('/');
  const std::string_view parent =
      slash == std::string_view::npos ? std::string_view("/")
                                      : path.substr(0, slash);
  const std::string_view name =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (name.empty() || name == "." || name == "..") {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid final path component")
        .Metadata("path");
  }
  auto ino = fs.LookupPath(parent.empty() ? std::string_view("/") : parent);
  if (!ino.ok()) return ino.error();
  if (!ino.value().IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "parent is not a directory")
        .Ino(ino.value().ino);
  }
  return std::make_pair(ino.value().ino, std::string(name));
}

// Shared front matter for the namespace commands: opt-in flag, image only,
// exclusive read-write mount.
int OpenWritable(const std::string& target, const Options& options,
                 std::unique_ptr<XfsFilesystem>* fs_out) {
  if (!options.rw_experimental) {
    std::cerr << "error: this command modifies the image; pass "
                 "--rw-experimental to confirm (images only)\n";
    return 3;
  }
  if (IsRawDevicePath(target)) {
    std::cerr << "error: read-write commands are restricted to image files "
                 "in this version\n";
    return 3;
  }
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options = WritableMountOptions(options);
  auto fs = XfsFilesystem::Mount(std::move(dev), mount_options);
  if (!fs.ok()) {
    PrintError(fs.error());
    return 1;
  }
  *fs_out = std::move(fs.value());
  return 0;
}

// Closes the journal with an unmount record and flushes to stable storage.
bool CloseWritable(XfsFilesystem& fs) {
  Status closed = fs.WriteCleanUnmountRecord();
  if (!closed.ok()) {
    PrintError(closed.error());
    return false;
  }
  if (!fs.Flush().ok()) {
    std::cerr << "error: flush failed\n";
    return false;
  }
  return true;
}

int CommandReflink(const std::string& target, const std::string& src_path,
                   const std::string& dst_path,
                   const std::string& src_offset_text,
                   const std::string& dst_offset_text,
                   const std::string& length_text, const Options& options) {
  uint64_t src_offset = 0, dst_offset = 0, length = 0;
  if (!ParseU64(src_offset_text, &src_offset) ||
      !ParseU64(dst_offset_text, &dst_offset) ||
      !ParseU64(length_text, &length)) {
    std::cerr << "error: offsets and length must be decimal or 0x-prefixed "
                 "integers\n";
    return 1;
  }
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto src = fs->LookupPath(src_path);
  if (!src.ok()) {
    PrintError(src.error());
    return 1;
  }
  auto dst = fs->LookupPath(dst_path);
  if (!dst.ok()) {
    PrintError(dst.error());
    return 1;
  }
  auto cloned = fs->ReflinkRange(src.value().ino, src_offset,
                                 dst.value().ino, dst_offset, length);
  if (!cloned.ok()) {
    PrintError(cloned.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "reflinked " << cloned.value() << " bytes\n";
  return 0;
}

// ---------------------------------------------------------------------------
// recover: journal replay as a standalone command (RW-5). Writes metadata, so
// it demands --yes-i-really-mean-it; the same image-only policy as the other
// read-write commands applies.
// ---------------------------------------------------------------------------
int CommandRecover(const std::string& target, const Options& options) {
  if (!options.yes_i_really_mean_it) {
    std::cerr << "error: `recover` replays the journal and writes metadata; "
                 "pass --yes-i-really-mean-it to confirm\n";
    return 3;
  }
  if (IsRawDevicePath(target)) {
    std::cerr << "error: journal recovery is restricted to image files in "
                 "this version\n";
    return 3;
  }
  std::string error;
  auto dev = OpenTarget(target, /*writable=*/true, /*exclusive=*/true, &error);
  if (!dev) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions mount_options;
  mount_options.read_write = true;
  mount_options.perf_stats = options.perf_stats;
  // The standalone shape ends with a clean unmount record so every later
  // mount (Linux or this build) starts from a recovered, clean log.
  mount_options.clean_log_after_recovery = true;
  // Recovery may need to finish rmapbt/RUI work before the ordinary writable
  // gate is opened; it is still never a general read-write mount.
  mount_options.recovery_only = true;
  auto fs = XfsFilesystem::Mount(std::move(dev), mount_options);
  if (!fs.ok()) {
    PrintError(fs.error());
    return 1;
  }
  // Mount may have committed post-recovery AGI/Cow cleanup after the journal
  // pass wrote its first closing record. Close the log again if needed.
  Status finished = fs.value()->FinishRecoveryClean();
  if (!finished.ok()) {
    PrintError(finished.error());
    return 1;
  }
  const auto& report = fs.value()->recovery_report();
  if (!report.has_value()) {
    // Already clean. (The mount may still have freed unlinked inodes, which
    // writes transactions; close them so the image leaves clean either way.)
    if (!CloseWritable(*fs.value())) return 1;
    std::cout << "log is clean; nothing to recover\n";
    return 0;
  }
  const ReplayStats& replay = report->replay;
  std::cout << "recovery complete\n";
  std::cout << "  records scanned          : " << replay.records_scanned << "\n";
  std::cout << "  committed transactions   : " << replay.transactions_replayed
            << "\n";
  std::cout << "  items replayed           : " << replay.items_replayed << "\n";
  std::cout << "  buffers replayed         : " << replay.buffers_replayed << "\n";
  std::cout << "  buffers skipped by LSN   : " << replay.buffers_skipped_newer
            << "\n";
  if (replay.cancels_seen != 0) {
    std::cout << "  cancel items             : " << replay.cancels_seen
              << "\n";
    std::cout << "  cancel refs consumed     : " << replay.cancels_consumed
              << " (unconsumed " << replay.cancel_refs_unconsumed << ")\n";
    std::cout << "  buffers cancelled        : " << replay.buffers_cancelled
              << "\n";
    std::cout << "  inode items cancelled    : "
              << replay.inode_items_cancelled << "\n";
    std::cout << "  icreate items cancelled  : "
              << replay.icreate_items_cancelled << "\n";
  }
  std::cout << "  intents completed        : " << report->intents_completed
            << "\n";
  std::cout << "  unlinked inodes processed: "
            << report->unlinked_inodes_processed << "\n";
  std::cout << "  final log state          : clean\n";
  if (options.verbose) {
    std::cout << "  inodes replayed          : " << replay.inodes_replayed
              << "\n";
    std::cout << "  icreate chunks replayed  : "
              << replay.icreate_chunks_replayed << "\n";
    std::cout << "  bytes written            : "
              << HumanBytes(replay.bytes_written) << "\n";
    std::cout << "  AGI buckets cleared      : "
              << report->agi_buckets_cleared << "\n";
  }
  return 0;
}

}  // namespace

int CommandCreate(const std::string& target, const std::string& path,
                  uint32_t mode, const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto split = ParentAndName(*fs, path);
  if (!split.ok()) {
    PrintError(split.error());
    return 1;
  }
  auto created = fs->CreateRegularFile(split.value().first, split.value().second,
                                static_cast<uint16_t>(mode), 0, 0);
  if (!created.ok()) {
    PrintError(created.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "created " << path << " (inode " << created.value() << ")\n";
  return 0;
}

int CommandMkdir(const std::string& target, const std::string& path,
                 uint32_t mode, const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto split = ParentAndName(*fs, path);
  if (!split.ok()) {
    PrintError(split.error());
    return 1;
  }
  Status status = fs->Mkdir(split.value().first, split.value().second,
                            static_cast<uint16_t>(mode), 0, 0);
  if (!status.ok()) {
    PrintError(status.error());
    return 1;
  }
  auto entry = fs->LookupName(split.value().first, split.value().second);
  if (!CloseWritable(*fs)) return 1;
  std::cout << "created directory " << path << " (inode "
            << (entry.ok() ? std::to_string(entry.value().ino)
                           : std::string("?"))
            << ")\n";
  return 0;
}

int CommandUnlink(const std::string& target, const std::string& path,
                  const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto split = ParentAndName(*fs, path);
  if (!split.ok()) {
    PrintError(split.error());
    return 1;
  }
  auto entry = fs->LookupName(split.value().first, split.value().second);
  if (!entry.ok()) {
    PrintError(entry.error());
    return 1;
  }
  auto inode = fs->OpenInode(entry.value().ino);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  if (inode.value().IsDir()) {
    std::cerr << "error: " << path << " is a directory; use rmdir\n";
    return 1;
  }
  Status status = fs->Unlink(split.value().first, split.value().second,
                             /*defer_inode_free=*/false);
  if (!status.ok()) {
    PrintError(status.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "unlinked " << path << " (inode " << entry.value().ino << ")\n";
  return 0;
}

int CommandRmdir(const std::string& target, const std::string& path,
                 const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto split = ParentAndName(*fs, path);
  if (!split.ok()) {
    PrintError(split.error());
    return 1;
  }
  auto entry = fs->LookupName(split.value().first, split.value().second);
  if (!entry.ok()) {
    PrintError(entry.error());
    return 1;
  }
  Status status = fs->RemoveEmptyDirectory(split.value().first, split.value().second,
                                      /*defer_inode_free=*/false);
  if (!status.ok()) {
    PrintError(status.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "removed directory " << path << " (inode " << entry.value().ino
            << ")\n";
  return 0;
}

// RW-4: rename, hard link and symlink creation. The atomic namespace work
// lives in XfsFilesystem::Rename/Link/CreateSymlink - these commands only
// resolve paths, choose the replace policy and report the result.
int CommandRename(const std::string& target, const std::string& src_path,
                  const std::string& dst_path, bool replace,
                  const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto src = ParentAndName(*fs, src_path);
  if (!src.ok()) {
    PrintError(src.error());
    return 1;
  }
  auto dst = ParentAndName(*fs, dst_path);
  if (!dst.ok()) {
    PrintError(dst.error());
    return 1;
  }
  auto outcome = fs->Rename(src.value().first, src.value().second,
                            dst.value().first, dst.value().second, replace,
                            /*defer_replaced_free=*/false);
  if (!outcome.ok()) {
    PrintError(outcome.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "renamed " << src_path << " -> " << dst_path << " (inode "
            << outcome.value().moved_ino;
  if (outcome.value().replaced_ino != 0) {
    std::cout << ", replaced inode " << outcome.value().replaced_ino;
    if (outcome.value().replaced_zero_link) {
      std::cout << " (freed)";
    } else {
      std::cout << " (nlink " << outcome.value().replaced_new_nlink << ")";
    }
  }
  std::cout << ")\n";
  return 0;
}

int CommandLink(const std::string& target, const std::string& existing_path,
                const std::string& new_path, const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto existing = fs->LookupPath(existing_path);
  if (!existing.ok()) {
    PrintError(existing.error());
    return 1;
  }
  if (existing.value().IsDir()) {
    std::cerr << "error: cannot hard link a directory\n";
    return 1;
  }
  auto dest = ParentAndName(*fs, new_path);
  if (!dest.ok()) {
    PrintError(dest.error());
    return 1;
  }
  Status status =
      fs->Link(existing.value().ino, dest.value().first, dest.value().second);
  if (!status.ok()) {
    PrintError(status.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "linked " << existing_path << " -> " << new_path << " (inode "
            << existing.value().ino << ", nlink "
            << existing.value().nlink + 1 << ")\n";
  return 0;
}

int CommandSymlink(const std::string& target, const std::string& link_target,
                   const std::string& link_path, const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto split = ParentAndName(*fs, link_path);
  if (!split.ok()) {
    PrintError(split.error());
    return 1;
  }
  auto created = fs->CreateSymlink(split.value().first, split.value().second,
                                   link_target, 0777, 0, 0);
  if (!created.ok()) {
    PrintError(created.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "created symlink " << link_path << " -> " << link_target
            << " (inode " << created.value() << ")\n";
  return 0;
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

bool ReadBinaryFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream in(ToPath(path), std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

bool ParseHexValue(const std::string& text, std::vector<uint8_t>* out) {
  std::string compact;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':' ||
        c == '-') {
      continue;
    }
    compact.push_back(c);
  }
  if (compact.size() % 2 != 0) return false;
  out->clear();
  out->reserve(compact.size() / 2);
  auto nibble = [](char c, int* v) {
    if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
    return false;
  };
  for (size_t i = 0; i < compact.size(); i += 2) {
    int hi = 0, lo = 0;
    if (!nibble(compact[i], &hi) || !nibble(compact[i + 1], &lo)) {
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

int CommandXattrList(const std::string& target, const std::string& path,
                     const Options& options) {
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
  auto attrs =
      fs.value()->ListXattrs(inode.value().ino, options.include_private);
  if (!attrs.ok()) {
    PrintError(attrs.error());
    return 1;
  }
  std::sort(attrs.value().begin(), attrs.value().end(),
            [&](const XattrEntry& a, const XattrEntry& b) {
              return a.DisplayName(options.include_private) <
                     b.DisplayName(options.include_private);
            });
  if (options.json) {
    std::cout << "[";
    bool first = true;
    for (const XattrEntry& e : attrs.value()) {
      std::cout << (first ? "" : ",") << "\n  {\"name\":\""
                << JsonEscape(e.DisplayName(options.include_private))
                << "\",\"length\":" << e.value.size() << ",\"remote\":"
                << (e.remote ? "true" : "false") << "}";
      first = false;
    }
    std::cout << (first ? "]\n" : "\n]\n");
    return 0;
  }
  for (const XattrEntry& e : attrs.value()) {
    std::cout << e.DisplayName(options.include_private) << "\n";
  }
  return 0;
}

int CommandXattrGet(const std::string& target, const std::string& path,
                    const std::string& name, const Options& options) {
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
  auto attr =
      fs.value()->GetXattr(inode.value().ino, name, options.include_private);
  if (!attr.ok()) {
    PrintError(attr.error());
    return 1;
  }
  if (!options.xattr_output.empty()) {
    std::ofstream out(ToPath(options.xattr_output), std::ios::binary);
    if (!out) {
      std::cerr << "error: cannot create " << options.xattr_output << "\n";
      return 1;
    }
    out.write(reinterpret_cast<const char*>(attr.value().value.data()),
              static_cast<std::streamsize>(attr.value().value.size()));
    if (!out) {
      std::cerr << "error: failed to write " << options.xattr_output << "\n";
      return 1;
    }
    return 0;
  }
  if (options.xattr_hex) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(attr.value().value.size() * 2);
    for (uint8_t b : attr.value().value) {
      text.push_back(kHex[b >> 4]);
      text.push_back(kHex[b & 0xf]);
    }
    std::cout << text << "\n";
    return 0;
  }
  if (!attr.value().value.empty()) {
    std::fwrite(attr.value().value.data(), 1, attr.value().value.size(),
                stdout);
  }
  return 0;
}

int CommandXattrSet(const std::string& target, const std::string& path,
                    const std::string& name, const std::string& literal,
                    const Options& options, bool replace_mode) {
  std::vector<uint8_t> value;
  if (options.xattr_value_is_hex) {
    if (!ParseHexValue(options.xattr_value_file, &value)) {
      std::cerr << "error: --value-hex must contain an even number of hex "
                   "digits\n";
      return 1;
    }
  } else if (!options.xattr_value_file.empty()) {
    if (!ReadBinaryFile(options.xattr_value_file, &value)) {
      std::cerr << "error: cannot read " << options.xattr_value_file << "\n";
      return 1;
    }
  } else {
    value.assign(reinterpret_cast<const uint8_t*>(literal.data()),
                 reinterpret_cast<const uint8_t*>(literal.data()) +
                     literal.size());
  }
  if (value.size() > kXattrMaxValueBytes) {
    std::cerr << "error: xattr value exceeds " << kXattrMaxValueBytes
              << " bytes\n";
    return 1;
  }
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto inode = fs->LookupPath(path);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  XfsFilesystem::XattrSetMode mode = XfsFilesystem::XattrSetMode::kUpsert;
  if (options.xattr_create) {
    mode = XfsFilesystem::XattrSetMode::kCreate;
  } else if (replace_mode) {
    mode = XfsFilesystem::XattrSetMode::kReplace;
  }
  auto stored = fs->SetXattr(inode.value().ino, name,
                             value.empty() ? nullptr : value.data(),
                             value.size(), mode);
  if (!stored.ok()) {
    PrintError(stored.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "set " << name << " (" << value.size() << " bytes"
            << (stored.value().remote ? ", remote" : ", local") << ")\n";
  return 0;
}

int CommandXattrRemove(const std::string& target, const std::string& path,
                       const std::string& name, const Options& options) {
  std::unique_ptr<XfsFilesystem> fs;
  int rc = OpenWritable(target, options, &fs);
  if (rc != 0) return rc;
  auto inode = fs->LookupPath(path);
  if (!inode.ok()) {
    PrintError(inode.error());
    return 1;
  }
  Status removed = fs->RemoveXattr(inode.value().ino, name);
  if (!removed.ok()) {
    PrintError(removed.error());
    return 1;
  }
  if (!CloseWritable(*fs)) return 1;
  std::cout << "removed " << name << "\n";
  return 0;
}

}  // namespace

int Run(const std::vector<std::string>& args) {
  Options options;
  std::vector<std::string> positional;
  std::string pwrite_text;
  std::string pwrite_from;
  uint8_t extend_fill = 0;
  bool have_fill = false;
  uint32_t namespace_mode = 0;
  bool have_mode = false;
  int rmap_ag = -1;
  bool explicit_rw = false;
  bool replace_existing = false;
  for (const auto& arg : args) {
    if (arg == "--verbose" || arg == "-v") {
      options.verbose = true;
    } else if (arg == "--perf-stats") {
      options.perf_stats = true;
    } else if (arg == "--debug" || arg == "-d") {
      options.debug = true;
      options.verbose = true;
    } else if (arg == "--force-unsafe-ro") {
      options.force_unsafe_ro = true;
    } else if (arg == "--yes-i-really-mean-it") {
      options.yes_i_really_mean_it = true;
    } else if (arg == "--rw-experimental") {
      options.rw_experimental = true;
    } else if (arg == "--json") {
      options.json = true;
    } else if (arg == "--replace") {
      // rename only: overwrite an existing file or empty directory target.
      replace_existing = true;
    } else if (arg == "--finobt") {
      options.finobt = true;
    } else if (arg == "--inobtcount") {
      options.inobtcount = true;
    } else if (arg == "--sparse-inodes") {
      options.sparse_inodes = true;
    } else if (arg == "--bigtime") {
      options.bigtime = true;
    } else if (arg == "--nrext64") {
      options.nrext64 = true;
    } else if (arg == "--rmapbt") {
      options.rmapbt = true;
    } else if (arg == "--reflink") {
      options.reflink = true;
    } else if (arg == "--parent") {
      options.parent_pointers = true;
    } else if (arg == "--modern-default-profile") {
      options.modern_default_profile = true;
    } else if (arg == "--rw-parent-qualification") {
      options.parent_qualification = true;
    } else if (arg == "--include-private") {
      options.include_private = true;
    } else if (arg == "--parents") {
      options.parent_check = true;
    } else if (arg == "--rw-rmapbt-qualification") {
      // Legacy RW-6B acceptance switch, now accepted as a no-op.
      options.rmapbt_qualification = true;
    } else if (arg == "--rw-reflink-qualification") {
      // Legacy RW-6C acceptance switch, now accepted as a no-op.
      options.reflink_qualification = true;
    } else if (arg == "--create") {
      options.xattr_create = true;
    } else if (arg == "--hex") {
      options.xattr_hex = true;
    } else if (StartsWith(arg, "--value-file=")) {
      options.xattr_value_file = arg.substr(13);
    } else if (StartsWith(arg, "--value-hex=")) {
      options.xattr_value_is_hex = true;
      options.xattr_value_file = arg.substr(12);
    } else if (StartsWith(arg, "--output=")) {
      options.xattr_output = arg.substr(9);
    } else if (arg == "--modern-inode-profile") {
      options.finobt = true;
      options.inobtcount = true;
      options.sparse_inodes = true;
      options.bigtime = true;
      options.nrext64 = true;
    } else if (arg == "--rw") {
      explicit_rw = true;
    } else if (StartsWith(arg, "--text=")) {
      pwrite_text = arg.substr(7);
    } else if (StartsWith(arg, "--from=")) {
      pwrite_from = arg.substr(7);
    } else if (StartsWith(arg, "--fill=")) {
      uint64_t value = 0;
      if (!ParseU64(arg.substr(7), &value) || value > 0xFF) {
        std::cerr << "error: --fill must be a byte value (e.g. --fill=0x5A)\n";
        return 1;
      }
      extend_fill = static_cast<uint8_t>(value);
      have_fill = true;
    } else if (StartsWith(arg, "--ag=")) {
      uint64_t value = 0;
      if (!ParseU64(arg.substr(5), &value) || value > 0xffffffffu) {
        std::cerr << "error: --ag must be an AG number\n";
        return 1;
      }
      rmap_ag = static_cast<int>(value);
    } else if (StartsWith(arg, "--mode=")) {
      if (!ParseOctal(arg.substr(7), &namespace_mode) ||
          namespace_mode > 07777) {
        std::cerr << "error: --mode must be an octal mode (e.g. --mode=0644)\n";
        return 1;
      }
      have_mode = true;
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

  // --rw is the eventual stable alias. The qualified writer is still opt-in
  // as --rw-experimental; real disks stay read-only regardless.
  if (explicit_rw) {
    std::cerr << "error: --rw is not enabled yet as a stable alias. Use --rw-experimental for the qualified image-file writer\n" "       (create, overwrite, extend, truncate, delete, rename, hard links, symlinks, xattrs and reflinks).\n" "       Real devices stay read-only in this version.\n";
    return 3;
  }

  if (options.perf_stats) xfs::PerfStats::Get().SetEnabled(true);
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
  if (command == "rmap") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin rmap <device|image> [--ag=N]\n";
      return 1;
    }
    return CommandRmap(positional[1], rmap_ag);
  }
  if (command == "refcount") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin refcount <device|image> [--ag=N]\n";
      return 1;
    }
    return CommandRefcount(positional[1], rmap_ag);
  }
  if (command == "reflink") {
    if (positional.size() != 7) {
      std::cerr << "usage: xfswin reflink <image> <src> <dst> <src-offset> "
                   "<dst-offset> <length> --rw-experimental\n"
                   "       length 0 clones to the source EOF\n";
      return 1;
    }
    return CommandReflink(positional[1], positional[2], positional[3],
                          positional[4], positional[5], positional[6],
                          options);
  }
  if (command == "recover") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin recover <image> --yes-i-really-mean-it "
                   "[--verbose]\n";
      return 1;
    }
    return CommandRecover(positional[1], options);
  }
  if (command == "journal") {
    if (positional.size() != 2) {
      std::cerr << "usage: xfswin journal <device|image> [--json] [--verbose]\n";
      return 1;
    }
    return CommandJournal(positional[1], options);
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
      std::cerr << "usage: xfswin ls <device|image> <path> [--json]\n";
      return 1;
    }
    return CommandLs(positional[1], positional[2], options);
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
  if (command == "fallocate") {
    if (positional.size() != 5) {
      std::cerr << "usage: xfswin fallocate <image> <path> <offset> <length> "
                   "--rw-experimental\n";
      return 1;
    }
    return CommandFallocate(positional[1], positional[2], positional[3],
                            positional[4], options);
  }
  if (command == "convert-unwritten") {
    if (positional.size() != 5) {
      std::cerr << "usage: xfswin convert-unwritten <image> <path> <offset> "
                   "<length> --rw-experimental\n";
      return 1;
    }
    return CommandConvertUnwritten(positional[1], positional[2],
                                   positional[3], positional[4], options);
  }
  if (command == "bmap") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin bmap <image> <path> [--json]\n";
      return 1;
    }
    return CommandBmap(positional[1], positional[2], options);
  }
  if (command == "extend") {
    if (positional.size() != 4) {
      std::cerr << "usage: xfswin extend <image> <path> <new-size> "
                   "--rw-experimental [--fill=0xNN]\n";
      return 1;
    }
    return CommandExtend(positional[1], positional[2], positional[3],
                         extend_fill, have_fill, options);
  }
  if (command == "truncate") {
    if (positional.size() != 4) {
      std::cerr << "usage: xfswin truncate <image> <path> <new-size> "
                   "--rw-experimental\n";
      return 1;
    }
    return CommandTruncate(positional[1], positional[2], positional[3],
                           options);
  }
  if (command == "parents") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin parents <device|image> <path> [--json]\n";
      return 1;
    }
    return CommandParents(positional[1], positional[2], options);
  }
  if (command == "xattr") {
    if (positional.size() < 4) {
      std::cerr << "usage: xfswin xattr <list|get|set|remove> <image> <path> "
                   "[name] [value] [--rw-experimental]\n";
      return 1;
    }
    const std::string& sub = positional[1];
    if (sub == "list") {
      if (positional.size() != 4) {
        std::cerr << "usage: xfswin xattr list <image> <path> [--json]\n";
        return 1;
      }
      return CommandXattrList(positional[2], positional[3], options);
    }
    if (sub == "get") {
      if (positional.size() != 5) {
        std::cerr << "usage: xfswin xattr get <image> <path> <name> "
                     "[--hex] [--output=<file>]\n";
        return 1;
      }
      return CommandXattrGet(positional[2], positional[3], positional[4],
                             options);
    }
    if (sub == "set") {
      const bool has_inline = positional.size() == 6;
      const bool has_external =
          options.xattr_value_is_hex || !options.xattr_value_file.empty();
      if (positional.size() < 5 || positional.size() > 6 ||
          (!has_inline && !has_external)) {
        std::cerr << "usage: xfswin xattr set <image> <path> <name> "
                     "[value | --value-file=<file> | --value-hex=<hex>] "
                     "--rw-experimental [--create|--replace]\n";
        return 1;
      }
      return CommandXattrSet(positional[2], positional[3], positional[4],
                             has_inline ? positional[5] : std::string(),
                             options, replace_existing);
    }
    if (sub == "remove") {
      if (positional.size() != 5) {
        std::cerr << "usage: xfswin xattr remove <image> <path> <name> "
                     "--rw-experimental\n";
        return 1;
      }
      return CommandXattrRemove(positional[2], positional[3], positional[4],
                                options);
    }
    std::cerr << "error: unknown xattr subcommand '" << sub << "'\n";
    return 1;
  }
  if (command == "create") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin create <image> <path> --rw-experimental "
                   "[--mode=0644]\n";
      return 1;
    }
    return CommandCreate(positional[1], positional[2],
                         have_mode ? namespace_mode : 0644u, options);
  }
  if (command == "mkdir") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin mkdir <image> <path> --rw-experimental "
                   "[--mode=0755]\n";
      return 1;
    }
    return CommandMkdir(positional[1], positional[2],
                        have_mode ? namespace_mode : 0755u, options);
  }
  if (command == "unlink") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin unlink <image> <path> --rw-experimental\n";
      return 1;
    }
    return CommandUnlink(positional[1], positional[2], options);
  }
  if (command == "rmdir") {
    if (positional.size() != 3) {
      std::cerr << "usage: xfswin rmdir <image> <path> --rw-experimental\n";
      return 1;
    }
    return CommandRmdir(positional[1], positional[2], options);
  }
  if (command == "rename") {
    if (positional.size() != 4) {
      std::cerr << "usage: xfswin rename <image> <src> <dst> "
                   "--rw-experimental [--replace]\n";
      return 1;
    }
    return CommandRename(positional[1], positional[2], positional[3],
                         replace_existing, options);
  }
  if (command == "link") {
    if (positional.size() != 4) {
      std::cerr << "usage: xfswin link <image> <existing> <new> "
                   "--rw-experimental\n";
      return 1;
    }
    return CommandLink(positional[1], positional[2], positional[3], options);
  }
  if (command == "symlink") {
    if (positional.size() != 4) {
      std::cerr << "usage: xfswin symlink <image> <target> <linkpath> "
                   "--rw-experimental\n";
      return 1;
    }
    return CommandSymlink(positional[1], positional[2], positional[3], options);
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
      mount_options.allow_rmapbt_qualification =
          options.rmapbt_qualification;
      mount_options.allow_reflink_qualification =
          options.reflink_qualification;
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
