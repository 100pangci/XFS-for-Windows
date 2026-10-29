# XFS for Windows

[![CI](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml/badge.svg)](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](https://mozilla.org/MPL/2.0/)

Windows-native XFS v5: format, read, experimentally modify and recover modern
XFS filesystems directly from Windows — no WSL, no Linux VM, no Linux
filesystem driver passthrough. The parser, writer, journal engine and recovery
path are C++20 in this repository; Linux and xfsprogs are used as external
oracles in CI, not as runtime dependencies.

> **Experimental read-write.** Image-file read-write is qualified in CI for the
> documented profile. `\\.\PhysicalDriveN` mounted read-write is **not yet
> qualified**; real disks are read + format only. Do not point the experimental
> writer at a disk that holds data you care about.

## What it is

| Layer | Role |
|---|---|
| `xfswin` CLI | format, inspect, check, mutate, journal dump/recovery |
| WinFsp adapter | native `X:` mount, read-only or experimental read-write |
| XFS core | on-disk parser, typed structures, verifier, transaction writer |
| Recovery engine | native replay of Windows- and Linux-written internal logs |

The implementation covers XFS v5 with CRC, an internal log, one data device,
nested directories, hard links, symlinks, reflinks, extended attributes,
reverse mappings and parent pointers. It does not aim to be a complete Linux
XFS replacement and it is not production storage software.

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
| **Supported** | Read/write/recovery is exercised by CI against the tested profile; still image-file-only for RW. |
| **Experimental** | Implemented and tested locally/CI, but the API or on-disk workload is not yet part of the ordinary supported profile. |
| **Unsupported / refused** | The reader may still open it, but a writable mount fails before the first write with the blocking feature named. |

* **Image files:** format, read-only mount, experimental read-write.
* **`\\.\PhysicalDriveN`:** read and format; **mounted read-write is not yet
  qualified** and the CLI refuses it.
* **`\\.\HarddiskVolumeX`:** read; format is outside v1.
* **WinFsp:** the mount adapter builds when the SDK is present; CI installs and
  exercises it on a Windows runner.
* **Linux interoperability:** Linux mounts and mutates Windows output, and
  Windows reads and mutates Linux output. `xfs_repair -n`, `xfs_db` and
  `xfs_scrub` are the external oracles.

## Supported writable profile

Ordinary `--rw-experimental` accepts a v5 filesystem with:

```text
crc=1, ftype=1, attr2
finobt, inobtcount, sparse inodes, bigtime, NREXT64
rmapbt on/off
reflink on/off (refcountbt + CoW)
parent pointers (XFS_SB_FEAT_INCOMPAT_PARENT)
exchange-range as a feature bit (operation not implemented)
XFS_SB_FEAT_INCOMPAT_LOG_XATTRS
one internal log, one data device, no realtime device
dirblklog == blocklog
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

## Quick start

Build first (see [Development](#development)):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF
cmake --build build --config RelWithDebInfo
$exe = "build\src\RelWithDebInfo\xfswin.exe"
```

### Read-only mount and inspection

```powershell
& $exe mkimage test.img 300M
& $exe format test.img --yes-i-really-mean-it
& $exe info test.img
& $exe check test.img --verbose
& $exe ls test.img /
& $exe mount test.img X:          # requires a WinFsp build
```

### Experimental read-write (image files only)

```powershell
& $exe mount test.img X: --rw-experimental
& $exe create test.img /file --rw-experimental
& $exe pwrite test.img /file 0 --rw-experimental --text=hello
& $exe mkdir test.img /dir --rw-experimental
& $exe rename test.img /file /dir/file --rw-experimental
& $exe link test.img /dir/file /dir/second --rw-experimental
& $exe unlink test.img /dir/second --rw-experimental
& $exe check test.img
```

`--rw-experimental` is the ordinary opt-in. `--rw-parent-qualification` is a
legacy development switch accepted as a no-op.

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
& $exe xattr set test.img /file user.alpha hello --rw-experimental
& $exe xattr list test.img /file --include-private
& $exe xattr remove test.img /file user.alpha --rw-experimental

& $exe reflink test.img /src /dst 0 0 4096 --rw-experimental
& $exe pwrite test.img /dst 0 --rw-experimental --text=CoW
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
& $exe mount dirty.img X: --rw-experimental   # recovers on mount
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
* **Image files only for mounted RW.** Real disks remain read-only when
  mounted; the CLI rejects the attempt.
* **Experimental.** This is not a drop-in production XFS driver. Keep backups.

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

* **Experimental / image files only.** PhysicalDrive mounted read-write is not
  qualified; the CLI refuses it.
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
