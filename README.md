# XFS for Windows

[![CI](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml/badge.svg)](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml)

A Windows-native XFS tool: format XFS v5 filesystems and read existing ones,
including a read-only WinFsp mount as a normal drive letter. No WSL, no Linux
VM, no compatibility layer — the parser and formatter talk to
`\\.\PhysicalDriveN`, `\\.\HarddiskVolumeX` or image files directly.

```
Windows / WinFsp
        |
        v
Windows Adapter  (WinFsp native API, path/name mapping)
        |
        v
Platform-independent XFS Core  (C++20, no WinFsp dependency)
        |
        v
Block Device Abstraction  (Image, Windows raw device, memory (tests))
```

## Status

First version scope: **XFS v5 (`crc=1`), internal log, single data device, no
realtime device, no writes to existing filesystems.**

| Feature | Status |
|---|---|
| Image files (`*.img`, `*.raw`) | yes, read + format |
| `\\.\PhysicalDriveN` | yes, read + format (Administrator required) |
| `\\.\HarddiskVolumeX` | read yes; format refused in v1 |
| Superblock + secondary superblocks | parse + CRC/UUID/geometry verify |
| AGF / AGI / AGFL | parse + verify |
| v3 inodes (regular, directory, symlink, device nodes) | parse + verify |
| Extents, holes, unwritten extents | read |
| Bmap B+tree (inode data fork) | read |
| AG free-space / inode btrees | walked and validated by `check` |
| Directories: shortform, block, leaf, node | all four formats unit-tested |
| Remote and inline symlinks | read (target returned) |
| Multi-block directories | read |
| Internal log cleanliness check | yes; replay is **not** implemented |
| WinFsp read-only mount | implemented, needs WinFsp runtime installed |
| Formatter (mkfs) | yes: standard v5 filesystem, no finobt/rmapbt/reflink |
| `xfs_repair`-style read-only consistency check | yes (`xfswin check`) |
| Writes to an existing XFS | **never** (read-only mount, write callbacks return `STATUS_MEDIA_WRITE_PROTECTED`) |
| Journal replay | **not implemented** (dirty logs are refused unless `--force-unsafe-ro`) |
| XFS v4, external log, realtime, parent pointers, metadir, zoned | rejected with a clear error |

## Requirements

- Windows x64
- Visual Studio 2022 with the C++ workload (MSVC 19.4x)
- CMake ≥ 3.24
- WinFsp 2.1 SDK for mount support (optional at build time)

This repository expects the SDKs under `D:\SDK` per the local convention:

- `D:\SDK\CMake\cmake-4.4.4-windows-x86_64`
- `D:\SDK\WinFsp\winfsp-2.1\DYNAMIC` (contains `inc/` and `lib/`)

## Build

```powershell
$cmake = 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
& $cmake --build build --config RelWithDebInfo
```

Or use the helper:

```powershell
.\scripts\build.ps1 -Config RelWithDebInfo
```

CMake locates WinFsp via `-DXFSWIN_WINFSP_ROOT=...`, the `WINFSP_ROOT`
environment variable, `D:\SDK\WinFsp\winfsp-*\DYNAMIC`, or a normal WinFsp
installation. Without the SDK the CLI still builds; `mount` reports that
WinFsp support is disabled.

To actually mount, the WinFsp **driver** must be installed system-wide by the
WinFsp installer (SDK extraction alone is not enough) and `winfsp-x64.dll`
must be loadable (the build copies it next to `xfswin.exe` when found).

## Usage

```text
xfswin list                                   # physical disks + safety flags
xfswin mkimage test.img 300M                  # create a sparse test image
xfswin format test.img --yes-i-really-mean-it # create a fresh XFS v5 fs
xfswin info test.img                          # superblock + geometry
xfswin check test.img                         # read-only consistency validation
xfswin ls test.img /                          # list a directory
xfswin cat test.img /hello.txt                # stream file data to stdout
xfswin stat test.img /hello.txt               # inode details
xfswin mount test.img X:                      # read-only WinFsp mount
xfswin unmount X:
xfswin mount \\.\PhysicalDrive2 X:            # real disk (Administrator)
```

Global options: `--verbose`, `--debug`, `--force-unsafe-ro`,
`--yes-i-really-mean-it`.

Exit codes: `0` success, `1` error, `2` dry-run (format without the
destructive flag), `3` refused for safety reasons.

### Format safety

- `format` never writes anything without `--yes-i-really-mean-it`.
- Physical disks are enumerated and refused when they are the Windows system
  disk or hold a pagefile.
- Raw disks are opened with exclusive access; all volumes on the disk are
  locked and dismounted before writing.
- Volumes are never formatted in v1 — only whole physical disks or images.
- Existing images are only overwritten after an explicit warning (including a
  signature probe for XFS/NTFS/ext4/MBR/GPT/LUKS/exFAT).
- The formatter creates a new filesystem in one pass and never "repairs" or
  modifies an existing XFS.

### Read-only guarantees

- Mounted filesystems always use `FSP_FSCTL_VOLUME_PARAMS::ReadOnlyVolume=1`
  and every write-path callback returns `STATUS_MEDIA_WRITE_PROTECTED`.
- The block device is opened read-only.
- The core library exposes no write API for existing filesystems; the only
  writer is `xfs::mkfs::Format`.
- Dirty logs are detected and mount is refused unless `--force-unsafe-ro` is
  given. This build never replays the log and never modifies it.

## Formatter profile (v1)

Fixed, conservative profile that Linux mounts natively:

- XFS v5, CRC enabled, `ftype=1`, lazy superblock counters, `attr2`,
  `projid32`
- 4 KiB filesystem blocks (or the device logical sector size if larger)
- 512-byte inodes (`-i size=512`), directory blocks = filesystem blocks
- internal log in the middle AG, sized like mkfs (`max(64 MiB, fs/2048)`),
  written as a clean log with an unmount record
- AG count/size from the mkfs `calc_default_ag_geometry` rules (4 AGs for
  most sizes)
- one 64-inode chunk in AG0 containing the root directory inode
- no finobt, rmapbt, reflink, sparse inodes, bigtime, NREXT64, parent
  pointers or metadir (all optional features are off)

The formatter refuses devices that are too small for this profile (a 64 MiB
log must fit inside one AG).

## Known limitations

- Symlinks are exposed through WinFsp as read-only files whose content is the
  link target (allowed fallback for v1; never written to disk). `xfswin stat`
  and `xfswin cat` show the target; real Windows reparse points are planned.
- Windows still routes most Win32 file access case-insensitively; the volume
  declares `CaseSensitiveSearch=1`, and lookups are exact and case-sensitive.
- XFS names that Windows cannot represent are escaped deterministically
  (`%XX` over UTF-8 bytes, trailing dots/spaces and reserved names handled).
  The mapping is reversible; names longer than 255 UTF-16 units are skipped
  from directory listings with a warning.
- No ACL translation: files are read-only for everyone; POSIX uid/gid/mode are
  kept internally and shown by `xfswin stat`.
- `xfswin check` is not `xfs_repair`: it verifies structure and CRCs but never
  fixes anything.
- Leaf/node directory formats are implemented but currently exercise only
  the code paths covered by unit tests; golden images from Linux are the next
  validation step. See `docs/TESTING.md`.

## Testing

```powershell
$cmake = 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
& $cmake -S . -B build -DXFSWIN_BUILD_TESTS=ON
& $cmake --build build --config RelWithDebInfo
& 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\ctest.exe' --test-dir build -C RelWithDebInfo --output-on-failure
```

The test suite covers CRC32c vectors, endian helpers, extent encoding,
shortform directories, synthetic extents/btree reads, a full formatter →
mount → check round-trip, and corruption detection (superblock, inode, AGF,
bnode/inode btrees, log).

Linux verification of Windows-created images and generation of golden images
from Linux are scripted in `tests/scripts/`; see `docs/TESTING.md`. Those
steps require a Linux environment (see the note there).

## Code layout

```
include/xfs/         public core headers (no WinFsp, no Windows UI)
src/core/            parser, verifier, geometry, directories, check
src/block/           image and Windows raw block devices
src/mkfs/            geometry, superblock/AG/inode/log builders, formatter
src/winfsp/          WinFsp adapter and mount host
src/cli/             xfswin.exe
tests/               unit + integration tests, scripts for Linux oracles
reference/           upstream xfsprogs source (not built, gitignored)
```

## References and licensing

The on-disk structures and algorithms were implemented from the public XFS
format documentation and cross-checked against upstream sources
(`fs/xfs/libxfs`, `xfsprogs`' `libxfs`, `mkfs.xfs`, `xfs_db`, `xfs_repair`).
Those sources are GPL-2.0 and are only consulted as a specification; no
upstream code is copied into this project. A copy of the xfsprogs sources is
kept in `reference/` for development and is not part of the build.

Third-party components:

- WinFsp SDK/runtime: <https://winfsp.dev> (GPLv3 with a commercial option).
  This project links against the SDK if present and does not redistribute the
  WinFsp driver.

The license of this project itself has not been chosen yet; add the intended
license before distributing binaries.
