# XFS for Windows

[![CI](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml/badge.svg)](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](https://mozilla.org/MPL/2.0/)

Windows-native XFS v5: format, read, modify and recover modern XFS filesystems
directly from Windows — no WSL, no Linux VM, no Linux filesystem driver
passthrough. The parser, writer, journal engine and recovery path are C++20 in
this repository; Linux and xfsprogs are used as external oracles in CI, not as
runtime dependencies.

> **Read-write is supported for the documented profile.** The filesystem
> writer, WinFsp read-write mount and automount read-write path are supported
> for the allow-listed XFS v5 profile described in
> [docs/RW_STABILITY.md](docs/RW_STABILITY.md). Device qualification is
> separate: image files are supported; an attached VHD/VHDX passed DEV-1; a
> physical removable USB disk passed DEV-1R on **one tested device only**
> (Samsung Flash Drive, USB BOT, 512/512-byte sectors). A fixed physical
> `\\.\PhysicalDriveN` mounted read-write is **not qualified**, and 4Kn/UASP
> are not qualified. The writer is not a drop-in replacement for the Linux XFS
> driver: keep backups and do not widen the profile by hand.

## Download and install

REL-1 packaging is still being qualified. No release-qualified product setup or
portable ZIP is available yet; check the [GitHub Releases
page](https://github.com/100pangci/XFS-for-Windows/releases) for the first
published version. The intended `0.1.0` release, the first delivery in the
`0.x` series, will provide a native Windows setup package and a portable ZIP for
each supported Windows architecture. See [docs/INSTALL.md](docs/INSTALL.md) for
installation, use, upgrade and uninstall instructions.

The setup package is the managed option: it installs for all users, registers
the automount service, and handles the shared WinFsp runtime. The portable ZIP
preserves the manual workflow: extract it to a folder and run `xfswin.exe`
there. It does not register a service or copy WinFsp DLLs; install the official
shared WinFsp runtime separately before mounting. There is no GUI, tray
application or automatic updater.

Once the package is available, start in an elevated PowerShell with WinFsp
installed and change to the folder containing `xfswin.exe`:

```powershell
$exe = (Resolve-Path .\xfswin.exe).Path
& $exe devices
& $exe mount
```

Select a filesystem and accept an available drive letter. Mounts are read-only
by default. A new setup installation starts the automount service with no
entries, so it mounts nothing until you explicitly enroll a filesystem.
Physical-device write policy and qualification are separate; fixed physical
disks are not qualified for read-write, and 4Kn, UASP and media power-loss
durability remain unqualified. Removable-device DEV-1R evidence covers only one
tested device. Keep backups and review
[docs/RW_STABILITY.md](docs/RW_STABILITY.md) before enabling writes.

## What it is

| Layer | Role |
|---|---|
| `xfswin` CLI | format, inspect, check, mutate, journal dump/recovery |
| WinFsp adapter | native `X:` mount, read-only or read-write for the supported profile |
| XFS core | on-disk parser, typed structures, verifier, transaction writer |
| Recovery engine | native replay of Windows- and Linux-written internal logs |

The implementation covers XFS v5 with CRC, an internal log, one data device,
nested directories, hard links, symlinks, reflinks, extended attributes,
reverse mappings and parent pointers. It is not a complete Linux XFS
replacement; read-write support is scoped to the documented profile and the
qualified targets in [docs/RW_STABILITY.md](docs/RW_STABILITY.md).

**Why this is different:** there is no WSL, no Linux VM and no Linux filesystem
driver passthrough. The parser, writer, journal engine and recovery path are
Windows code in this repository. Linux kernels and xfsprogs are used as
external oracles in CI, never as runtime dependencies.

## Performance

PERF-1 (image-file read-write mount) changes the I/O path from a correctness
prototype into a usable driver:

- large physically contiguous reads and writes are coalesced into positioned
  I/O instead of one device operation per filesystem block;
- different files perform ordinary data I/O concurrently (per-inode locking
  plus independent `FILE_FLAG_OVERLAPPED` image I/O); `Flush` remains a real
  durability barrier;
- path/directory/extent/data caches are bounded and generation-checked, and
  the adapter reparses an inode's BMT only when its extent generation changes;
- sequential writers use unwritten preallocation (including the Windows
  allocation-size hint) without moving EOF;
- metadata write-back is flushed in batches while the WAL record itself is
  still flushed before metadata, and deferred timestamps are published at
  `FlushFileBuffers`/close/unmount;
- WinFsp POSIX unlink/rename semantics are enabled, and Windows read-only
  attribute/timestamp changes go through normal metadata transactions, so
  Explorer and PowerShell delete/replace work without a special path.

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the baseline, A/B method,
deterministic I/O-call gates and remaining bottlenecks, and
[docs/LOCKING.md](docs/LOCKING.md) for the lock hierarchy.
## Current status

Three terms are used consistently below:

| Term | Meaning |
|---|---|
| **Supported** | Read, write, recovery and the documented read-write profile are exercised by CI and covered by the contract in [docs/RW_STABILITY.md](docs/RW_STABILITY.md). This is a filesystem-writer claim, not a blanket device or all-XFS claim. |
| **Qualified hardware** | A separate, per-device qualification status for real disks. Qualification never widens the supported filesystem profile. |
| **Unsupported / refused** | The reader may still open it, but a writable mount fails before the first write with the blocking feature named. |

* **Image files:** format, read-only mount, supported read-write.
* **WinFsp:** read-write mounts are supported for the allow-listed profile;
  the read-only path remains available everywhere.
* **Removable media:** the manual, identity-gated DEV-1R qualification passed on
  one device (Samsung Flash Drive, 64,160,400,896 bytes, USB BOT, 512-byte
  sectors) on 2026-11-01. See
  [docs/DEV1_REMOVABLE_QUALIFICATION.md](docs/DEV1_REMOVABLE_QUALIFICATION.md).
* **`\\.\PhysicalDriveN`:** read and format; **fixed-disk mounted read-write
  remains unqualified and is refused by default**; the removable-device path is
  qualified only on the tested hardware.
* **`\\.\HarddiskVolumeX`:** read; format is outside v1.
* **Linux interoperability:** Linux mounts and mutates Windows output, and
  Windows reads and mutates Linux output. `xfs_repair -n`, `xfs_db` and
  `xfs_scrub` are the external oracles.

## Supported writable profile

Ordinary `--rw` accepts a v5 filesystem with:

```text
crc=1, ftype=1, attr2
finobt, inobtcount, sparse inodes, bigtime, NREXT64
rmapbt on/off
reflink on/off (refcountbt + CoW)
parent pointers (XFS_SB_FEAT_INCOMPAT_PARENT)
exchange-range as a feature bit (operation not implemented)
XFS_SB_FEAT_INCOMPAT_LOG_XATTRS
one internal log (512-byte log sector, no stripe unit: superblock value 0 or
1), one data device, no realtime device
512-byte filesystem sectors
ftype=1, at most 64 inodes per block
dirblklog == 0
```

Refused before the first write: metadir, zoned, needs-repair, quotas, realtime,
external logs, case-insensitive (`asciici`) directories, multi-filesystem-block
directory blocks, unsupported allocation-group geometry, and exchange-range
**operations** (XMI/XMD log items). See
[docs/DEFAULT_PROFILE.md](docs/DEFAULT_PROFILE.md) for the measured
`mkfs.xfs` defaults and the modern-default gate.

The modern default profile is the primary RW-6E fixture:

```bash
truncate -s 512M default.img
mkfs.xfs -f default.img          # no -n parent=0
```

The CI records the exact `mkfs.xfs`/kernel versions and asserts the measured
feature set. A future default that adds an unsupported feature is refused,
not assumed to work.

## Feature matrix

| Feature | Read | Write | Recovery |
|---|---|---|---|
| XFS v5 / CRC / ftype | yes | yes | yes |
| superblock + secondary copies | yes | counters | yes |
| AGF / AGI / AGFL | yes | yes | yes |
| v3 inodes (file/dir/symlink/special) | yes | yes | yes |
| extents / holes / unwritten | yes | yes | yes |
| data-fork bmap B+tree | yes | yes | yes |
| directories: shortform/block/leaf/node | yes | yes | yes |
| inline and remote symlinks | yes | yes | yes |
| internal journal | yes | yes | yes |
| `finobt` / `inobtcount` | yes | yes | yes |
| sparse inode chunks | yes | yes | yes |
| `bigtime` / `NREXT64` | yes | yes | yes |
| `rmapbt` | yes | yes | yes |
| `reflink` / `refcountbt` / CoW | yes | yes | yes |
| native xattrs (shortform/leaf/node/remote/BMBT) | yes | yes | yes |
| **parent pointers** (`PARENT`) | yes | **yes (RW-6E)** | yes (PPTR ATTRI/ATTRD) |
| quota | read only | **no** | — |
| realtime device | read only | **no** | — |
| external log | read only | **no** | — |
| metadir | read only | **no** | — |
| zoned / zone gaps | read only | **no** | — |
| exchange-range operation | feature bit accepted | **no** (deferred) | XMI/XMD refused preflight |

Parent pointers are filesystem-owned metadata: forward dirents and reverse
records are updated in the same namespace transaction. The generic
`xfswin xattr` path refuses `parent.*` mutations; only the namespace core and
recovery may touch them.

## Development quick start

Build first (see [Development](#development)):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF
cmake --build build --config RelWithDebInfo
$exe = "build\src\RelWithDebInfo\xfswin.exe"
```

### Mounting XFS media attached to Windows

Run these commands from an elevated PowerShell; discovery opens raw disks
read-only and therefore needs Administrator rights.

```powershell
& $exe devices                 # what XFS filesystems are attached
& $exe mount                   # pick one from the menu, drive letter [auto]
& $exe unmount D:
```

Optionally let a resident service do it whenever the media is inserted:

```powershell
& $exe automount install
& $exe automount enable 11111111-1111-4111-8111-111111111111 --drive auto
& $exe automount start
```

Insert the device and it appears in Explorer as a normal drive; `$exe status`
shows the service state, configured filesystems and live mounts. When Windows
assigned the RAW volume a letter (`F:` with the "You need to format" prompt),
the service takes that letter over and mounts the filesystem there instead of
adding a second drive; a failed mount restores the original Windows mount
point. Automount is **read-only by default**. For automatic read-write, enroll
the device once with `$exe automount enable <uuid> --rw` and confirm the
identity pins it prints (in a script, `--yes-i-really-mean-it`); subsequent
insertions mount read-write only when the UUID, serial/stable id, size and
device class all still match, otherwise they fall back to read-only. Canonical
`--rw` records the removable-device qualification after that identity
confirmation and needs neither `--device-write-qualification` nor
`--allow-removable-disk-write`. The deprecated `--rw-experimental`
compatibility spelling retains the old explicit `--device-write-qualification`
requirement and additionally needs `--allow-removable-disk-write` for
removable disks. Existing `mode = experimental-rw` configuration files load
unchanged and are rewritten as `mode = read-write` on the next save, with
every identity pin preserved. Fixed physical disks are refused. The only
removable hardware with DEV-1R evidence is the Samsung Flash Drive (serial
`AA00000000000489`, USB BOT, 512/512-byte sectors); other removable devices
and all fixed physical-disk read-write remain **NOT READY**.

The configuration is `%ProgramData%\xfswin\automount.ini` (UUID-keyed and
versioned). To disable automount completely, run `automount disable <uuid>`,
`automount stop`, `automount uninstall`, and remove that file. See
[docs/UX.md](docs/UX.md) for the drive-letter rules, Explorer volume
properties, event/retry semantics, troubleshooting and the CI coverage.

### Read-only mount and inspection

```powershell
& $exe mkimage test.img 300M
& $exe format test.img --yes-i-really-mean-it
& $exe info test.img
& $exe check test.img --verbose
& $exe ls test.img /
& $exe mount test.img X:          # requires a WinFsp build
```

### Read-write (images; real devices under DEV-1/DEV-1R qualification)

```powershell
& $exe mount test.img X: --rw
& $exe create test.img /file --rw
& $exe pwrite test.img /file 0 --rw --text=hello
& $exe mkdir test.img /dir --rw
& $exe rename test.img /file /dir/file --rw
& $exe link test.img /dir/file /dir/second --rw
& $exe unlink test.img /dir/second --rw
& $exe check test.img
```

`--rw` is the canonical opt-in for image files. A writable mount or
file-mutating command on a real disk needs `--rw
--device-write-qualification`; fixed and removable disks additionally need
`--allow-fixed-disk-write` / `--allow-removable-disk-write`.
`--rw-experimental` remains accepted as a deprecated alias for old scripts and
prints a warning; it selects the same writer and does not bypass the profile,
device policy, identity pins or volume ownership. `recover` and `format` keep
their own `--yes-i-really-mean-it` confirmation and also require
`--device-write-qualification` for a real device.
`device-info`, `device-check` and `device-selftest` expose the probed geometry
and write policy. See `docs/WINDOWS_BLOCK_DEVICE.md`.

`--rw-parent-qualification`, `--rw-reflink-qualification` and
`--rw-rmapbt-qualification` are legacy development switches accepted as
no-ops.

### Formatting modern / parent-pointer images

```powershell
& $exe format test.img --yes-i-really-mean-it --modern-inode-profile
& $exe format test.img --yes-i-really-mean-it --parent
& $exe format test.img --yes-i-really-mean-it --modern-default-profile
& $exe format test.img --yes-i-really-mean-it --rmapbt --reflink
```

`--parent` enables parent pointers and the exchange-range feature bit, matching
the xfsprogs coupling. `--modern-default-profile` enables the 7.2 reference
default feature set (minus metadir/realtime/quota).

### Extended attributes and reflinks

```powershell
& $exe xattr set test.img /file user.alpha hello --rw
& $exe xattr list test.img /file --include-private
& $exe xattr remove test.img /file user.alpha --rw

& $exe reflink test.img /src /dst 0 0 4096 --rw
& $exe pwrite test.img /dst 0 --rw --text=CoW
```

`--include-private` is a diagnostic view; it is not a mutation path. Values
are opaque bytes and may be supplied with `--value-file=...` or
`--value-hex=...`.

### Parent diagnostics

```powershell
& $exe check test.img --parents --verbose
& $exe parents test.img /file
& $exe parents test.img /file --json
& $exe xattr list test.img /file --include-private
& $exe xattr get test.img /file parent.file --include-private --hex
```

The audit cross-checks both directions:

```text
directory entry (parent, name) -> child
child parent attribute        -> (parent inode, generation, name)
```

Root has no parent pointer and its `..` points to itself. A linked directory
has exactly one parent pointer; a regular file/symlink has one per hard link;
an unlinked directory has none.

### Journal recovery

```powershell
& $exe journal test.img --json      # read-only structural dump
& $exe recover test.img --yes-i-really-mean-it
& $exe mount dirty.img X: --rw   # recovers on mount
```

Recovery refuses, before the first write, any log item it cannot prove
replayable. It handles Windows and Linux records, torn tails, buffer
cancellation, inode/icreate/buffer items, EFI/RUI/BUI/CUI and ATTRI/ATTRD
(including parent-pointer PPTR operations), and AGI unlinked processing.

## Safety model

* **Allow-list, not best effort.** `CheckWritableProfile` refuses a filesystem
  unless every structure used by the mutation paths is maintained.
* **No partial namespace.** A dirent and its parent pointer are in one
  committed transaction. A crash leaves the old namespace + old pointers, or
  the new namespace + new pointers.
* **No writable mount on a split namespace.** A writable parent-pointer mount
  audits forward dirents against reverse records after journal recovery and
  refuses before exposing the volume if they disagree.
* **Write-ahead logging.** The log record is flushed before metadata; recovery
  only applies committed records. Remote xattr values are written and flushed
  before the metadata that references them commits.
* **No repair-as-validation.** Images are checked before and after mutation.
  If recovery cannot prove a state, it refuses instead of “repairing” it.
* **Image files are the default writable target.** Real disks are writable only
  under the DEV-1 policy: exclusive share mode, probed geometry, volume
  ownership (every volume on the disk locked and dismounted), and an explicit
  class opt-in. A `\\.\PhysicalDriveN` path alone never enables writes; fixed
  disks are refused by default. See `docs/WINDOWS_BLOCK_DEVICE.md`.
* **Supported scope, not universal XFS.** The contract applies to the
  allow-listed profile on qualified targets. This is not a drop-in production
  XFS driver for every feature or device; keep backups.

## Validation / why trust it

The project uses several independent oracles, not one implementation checking
itself:

| Oracle | What it proves |
|---|---|
| **Linux kernel** | Mounts, reads, writes and replays the image with the real XFS implementation. |
| **`xfs_repair -n`** | No repair is requested; the image must be clean as written. |
| **`xfs_scrub`** | Online structural checks where the host supports them, including the parent-pointer checker on modern kernels. |
| **Independent Python oracles** | Re-parse raw inodes, directories, btrees, attr forks, rmap, refcount and parent records without the C++ parser. |
| **Crash injection** | Kill every relevant transaction write phase; after replay, only old-or-new states are legal. |
| **ASan / UBSan** | The full unit/integration suite runs under Clang sanitizers on every push. |
| **libFuzzer** | Superblock, AG headers, inode/fork, btree, directory, xattr and log-item parsers are fuzzed from real `mkfs.xfs` seeds. |
| **Stateful fuzzing (FUZZ-2)** | A model-driven generator runs long legal mutation sequences, applies space/directory/xattr pressure, crashes at the writer's injection points and has Linux/xfsprogs recover and judge the result. |
| **Cross-OS churn** | Linux → Windows → Linux → Windows mutations with a final Linux mount/repair. |

The main end-to-end shapes are:

```text
Windows writer       -> Linux mount -> xfs_repair -n
Linux dirty journal  -> Windows recovery -> Linux verify
Windows crash        -> Windows recovery -> Linux verify
Linux-created image  -> Windows mutate -> Linux mutate -> Windows verify
modern mkfs.xfs      -> Windows mutate -> Linux mount/repair/scrub
```

Details and the qualification ledgers:

* [docs/TESTING.md](docs/TESTING.md)
* [docs/STATEFUL_FUZZING.md](docs/STATEFUL_FUZZING.md)
* [docs/RECOVERY.md](docs/RECOVERY.md)
* [docs/DEFAULT_PROFILE.md](docs/DEFAULT_PROFILE.md)
* [docs/MODERN_INODE_FEATURES.md](docs/MODERN_INODE_FEATURES.md)
* [docs/RMAPBT.md](docs/RMAPBT.md)
* [docs/REFLINK.md](docs/REFLINK.md)
* [docs/XATTR.md](docs/XATTR.md)
* [docs/PARENT_POINTERS.md](docs/PARENT_POINTERS.md)

Local checks (the same commands CI runs):

```powershell
ctest --test-dir build -C RelWithDebInfo --output-on-failure
python tests\tools\rw6e_parent_probe.py image.img --require-parent
python tests\tools\rw6d_xattr_probe.py image.img --inode 67
python tests\tools\rw6b_rmap_probe.py image.img
python tests\tools\rw6c_refcount_probe.py image.img
```

## Architecture

```text
WinFsp / xfswin CLI
        |
        v
Windows adapter: path mapping, handles, security descriptors
        |
        v
Platform-independent XFS core (C++20, no WinFsp dependency)
  - parser/verifier (inodes, dirs, btrees, attrs, rmap, refcount)
  - journal scan/replay and intent completion
  - transaction writer (allocator, bmap, ialloc, namespace, xattr)
        |
        v
Block device abstraction: image file, Windows raw device, memory (tests)
```

Every metadata mutation goes through a transaction that emits a log record and
flushes it before writing the metadata buffers it describes. Metadata
write-back is batched until a durability boundary (Flush, clean unmount or log
wrap), with the log tail pinned to the oldest unflushed transaction; the WAL
record itself is never deferred. Large transactions are split at 32 KiB record
boundaries with `CONTINUE`/`WAS_CONT` up to `XLOG_MAX_RECORD_BSIZE`. The
on-disk LSN stamp lets replay skip newer buffers.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the module map and the
single owner of each filesystem invariant.

## Current limitations

* **Raw-device writes are hardware-qualified, not part of the filesystem
  writer claim.** A physical fixed disk mounted read-write is not qualified.
  It is refused by default and reachable only through the explicit
  device-write qualification opt-ins plus the device-class opt-in. The
  removable path has one-device DEV-1R evidence (Samsung Flash Drive, USB BOT,
  512/512-byte sectors, 2026-11-01); other removable devices, fixed physical
  disks, UASP, 4Kn and media power-loss durability remain unqualified.
* **Automount is read-only by default.** The service can mount qualified
  virtual disks and the DEV-1R removable profile read-write only after an
  explicit enrollment, identity pins and device-class opt-in. It never
  declares a new removable device qualified and never enables fixed
  physical-disk read-write.
* **No quotas, realtime devices, external logs, metadir or zoned filesystems**
  for writing. These remain readable where the parser supports them.
* **No exchange-range operation.** The feature bit is accepted; XMI/XMD log
  items are refused before the first write.
* **No ACL/security-descriptor translation.** POSIX ACL and security xattrs are
  preserved as opaque on-disk bytes.
* **No NTFS alternate data streams.**
* **No online repair.** `check` is read-only; recovery is journal replay, not
  arbitrary metadata repair.
* `asciici` directories and multi-filesystem-block directory blocks are
  refused for writing.
* Allocation groups of 2^31 blocks or more are refused (reflink CoW flag).
* Only one data device and one internal log.
* The writer does not log ATTRI/ATTRD for ordinary user xattrs; it uses one
  atomic transaction. Linux ATTRI/ATTRD and PPTR records are replayed.
  Genuine ordinary logged-xattr evidence still needs the opt-in debug-kernel
  harness (`docs/XATTR.md` row 6).

## Documentation

| Document | Contents |
|---|---|
| [docs/RW_STABILITY.md](docs/RW_STABILITY.md) | supported read-write contract, evidence matrix, fail-closed semantics and the hardware boundary |
| [docs/DESIGN.md](docs/DESIGN.md) | layering and invariants |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | module map and invariant ownership |
| [docs/JOURNAL.md](docs/JOURNAL.md) | log format, transactions, recovery boundaries |
| [docs/ALLOCATOR.md](docs/ALLOCATOR.md) | free-space and AGFL rules |
| [docs/INODE_ALLOCATOR.md](docs/INODE_ALLOCATOR.md) | inobt/finobt/AGI chunk lifecycle |
| [docs/NAMESPACE.md](docs/NAMESPACE.md) | create/remove/rename/link/symlink transactions |
| [docs/RECOVERY.md](docs/RECOVERY.md) | replay, intents, refusals |
| [docs/MODERN_INODE_FEATURES.md](docs/MODERN_INODE_FEATURES.md) | finobt, inobtcount, sparse, bigtime, NREXT64 |
| [docs/RMAPBT.md](docs/RMAPBT.md) | reverse mappings |
| [docs/REFLINK.md](docs/REFLINK.md) | refcountbt, reflink, CoW |
| [docs/XATTR.md](docs/XATTR.md) | native extended attributes and ATTRI/ATTRD |
| [docs/PARENT_POINTERS.md](docs/PARENT_POINTERS.md) | parent-pointer format, lifecycles, oracle, ledger |
| [docs/DEFAULT_PROFILE.md](docs/DEFAULT_PROFILE.md) | measured modern `mkfs.xfs` profile |
| [docs/TESTING.md](docs/TESTING.md) | fixtures, crash matrices, external oracles |
| [docs/WINDOWS_BLOCK_DEVICE.md](docs/WINDOWS_BLOCK_DEVICE.md) | Windows open flags, volume ownership, geometry, flush, lifecycle, device policy |
| [docs/DEV1_QUALIFICATION.md](docs/DEV1_QUALIFICATION.md) | DEV-1 VHD evidence, fixed-disk NOT READY decision and remaining qualification steps |
| [docs/DEV1_REMOVABLE_QUALIFICATION.md](docs/DEV1_REMOVABLE_QUALIFICATION.md) | DEV-1R removable-media evidence ledger and manual qualification procedure (one-device hardware run passed 2026-11-01; fixed disks not ready) |
| [docs/UX.md](docs/UX.md) | native Windows discovery, interactive mounting, drive letters, Explorer volume information, automount service and its read-only/DEV-1 boundary |
| [docs/STATEFUL_FUZZING.md](docs/STATEFUL_FUZZING.md) | model-based stateful and crash fuzzing |
| [docs/INSTALL.md](docs/INSTALL.md) | installation, WinFsp prerequisite, first mount, upgrade, uninstall and preserved configuration |
| [INSTALLER.md](INSTALLER.md) | native Rust setup, WinFsp ownership and license, service maintenance, rollback and package build interface |
| [RELEASE.md](RELEASE.md) | maintainer release gate, provenance evidence, signing, manifest and rollback |
| [narrowUX.md](narrowUX.md) | the intentionally limited first packaged UX |

## Development

Requirements: Windows 10/11, Visual Studio 2022 (C++20), CMake ≥ 3.24.
WinFsp SDK is optional; without it the CLI still builds and only the `mount`
command is unavailable.

The portable core and test suite also build on Linux (the Windows CLI and
WinFsp adapter are not built there):

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF \
  -DXFSWIN_WARNINGS_AS_ERRORS=ON
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure

# Fixtures and independent probes
build\tests\RelWithDebInfo\xfs_tests.exe --filter=rw6e
python tests\tools\rw6e_parent_probe.py out\rw6e-parent.img --require-parent
```

Linux qualification scripts:

```bash
sudo bash tests/scripts/qualify_rw6e_host.sh
SIZE_MB=1024 sudo bash tests/scripts/make_rw6e_linux.sh rw6e-linux
sudo bash tests/scripts/verify_rw6e_linux.sh rw6e-linux/rw6e.img \
    rw6e-linux/rw6e.manifest.json
```

They refuse to run on a host whose `mkfs.xfs` or kernel cannot create/mount a
`parent=1` filesystem; they never silently downgrade to `xfs_repair`-only
evidence. The RW-6E Linux jobs run on GitHub-hosted `ubuntu-24.04` (kernel
6.17.0-1022-azure) and build xfsprogs 7.2.0 from kernel.org first; there is no
opt-in variable or self-hosted runner.

## Milestone history

The detailed ledgers and evidence links live in `docs/`; this is only a map.

| Stage | What it added |
|---|---|
| RW-0..RW-5 | core overwrite writer, log transactions, allocator/bmap, inode/directory mutation, rename/link/symlink, native recovery |
| RW-6A | finobt, inobtcount, sparse inodes, bigtime, NREXT64 |
| RW-6B | writable rmapbt |
| RW-6C | reflink / refcountbt / CoW |
| RW-6D | native extended attributes and ATTRI/ATTRD replay |
| RW-6E | parent pointers, bidirectional namespace audit, modern default-profile qualification, README rewrite |

## License

MPL 2.0. See [LICENSE](LICENSE).
