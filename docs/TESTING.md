# Testing

Correctness for a filesystem is not "Explorer shows a file". The strategy is:

1. deterministic unit tests for primitives and parsers,
2. a formatter ↔ reader round-trip on freshly created images,
3. corruption tests that require errors, not crashes,
4. **Linux oracles** for both directions:
   - Linux-created images must be read correctly on Windows,
   - Windows-formatted images must pass `xfs_repair -n` and mount on Linux,
5. differential checks against `xfs_db`.

## Local tests (Windows)

```powershell
$cmake = 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
& $cmake -S . -B build -DXFSWIN_BUILD_TESTS=ON
& $cmake --build build --config RelWithDebInfo
& .\build\tests\RelWithDebInfo\xfs_tests.exe
# or: ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Covered today:

- CRC32c known vector and XFS checksum round-trip
- endian helpers
- extent record encode/decode and edge values
- directory name hash basics
- deterministic Windows name mapping round-trips (reserved names, `%`,
  trailing dot/space, Unicode)
- synthetic file reads across extents, holes and unwritten extents
- synthetic bmbt (extent B+tree) read
- directory formats: shortform, single-block, leaf (LEAF1) and node (da btree)
  listing + lookup\n- inline and remote symlink targets (including header CRC/owner/blkno checks)
- formatter → mount → info → check → root directory round-trip
- geometry scaling (300 MiB and 1.2 GiB images) and too-small rejection
- corruption detection: superblock magic/CRC, root inode magic, AGF CRC,
  bnobt magic, inobt record count, unknown log content (with and without
  `--force-unsafe-ro`)

Additionally, `tests/tools/xfs_probe.py` is an independent Python
implementation of the structural checks (its own CRC32c and layout parsing).
Generate an image with `xfswin mkimage` + `xfswin format` and run:

```powershell
python tests\tools\xfs_probe.py build\smoke.img
```

It must print `ALL STRUCTURAL CHECKS PASSED`. This catches bugs where the C++
reader and writer share a wrong assumption.

## Read-write tests (RW-0 data, RW-1 transactions, RW-2 allocator, RW-3 namespace)

The write path is the same core code for the CLI and for WinFsp, so it is
tested in layers. RW-1 added the journal, so the layers now include a log
format oracle and a crash matrix that Linux replays. RW-2 adds the allocator
and the extent map, RW-3 the inode allocator and the namespace; every layer
below runs on them as well.

### 1. Unit and integration tests (no driver, no Linux)

`xfs_tests` covers the range rules (holes, unwritten extents, EOF, immutable
and append-only inodes, extent splits), the round trip through a real image,
cache invalidation, the mount policy and the crash-injection configuration
(`tests/unit/write_tests.cpp`). RW-1 added `tests/unit/journal_tests.cpp`:

- the record layout (header fields, op chain, cycle stamping, CRC) checked
  against a second, independent CRC computation in the test,
- rejected records (too large, bad block/cycle, empty),
- the write position a clean log establishes (`ReadLogPosition`),
- the clean unmount record closing a dirty log,
- `SetFileTimes`: only the core fields it owns change, the CRC stays valid and
  the inode LSN equals the newest log record's LSN,
- write-ahead ordering observed through a recording device: file data, then
  log header, log data, flush, then the inode, then flush.

RW-2 adds `tests/unit/rw2_tests.cpp`, all against the real fixture image and a
fresh remount afterwards:

- append one block, a multi-block append, a sparse extension (the gap must be
  a hole and free-block accounting must only change for the written blocks);
- writing into a hole inside the file without changing the size;
- truncate shrink (blocks return to the allocator), truncate grow (no
  allocation, zeros), truncate to zero (`nextents`/`nblocks` = 0), and the
  zeroing of the partially kept last block (a later extension can only see
  zeros);
- reuse: truncating the file frees its extent, and the next allocation is
  carved out of the merged free extent;
- the extent map converting to btree format at 22 extents and back to extents
  format when the truncate drops below the fork capacity, with `di_nblocks`
  counting data *and* bmbt blocks;
- the allocator counters after mixed allocate/free operations.

RW-3 adds `tests/unit/rw3_tests.cpp`, also against the real fixture image with
a fresh remount and the built-in consistency check after every scenario:

- the inode allocator itself: first free inode, allocation order, freeing and
  reuse, a new chunk allocated in transaction, a fully freed chunk removed with
  its blocks returned, the unlinked buckets, and the chunk layout (aligned run,
  full inobt record, `agi_newino`);
- `CreateRegularFile`/`Mkdir` and the counters they move (parent nlink for
  mkdir), `Unlink` freeing the data blocks *and* the inode,
  `RemoveEmptyDirectory`, and the open-but-unlinked deferral
  (`defer_inode_free` then `FreeUnlinkedInode`);
- a directory growing through all four formats (640 creates with 8-byte names,
  then half of them removed) and an `rmdir` of a grown directory returning its
  leaf/free blocks;
- inode reuse after a delete without stale data (the new file has a higher
  generation and size 0) and the refusals (non-empty rmdir, invalid names,
  duplicate names, directories in `unlink`, files in `rmdir`).

Every one of these images is also checked by `xfswin check`, which now also
recomputes `di_nblocks` from the extent map and the bmbt blocks, and verifies
`agf_btreeblks` against the number of non-root free-space btree blocks.

The images come from `tests/support/fixture_image.cpp`, which builds a
*consistent* filesystem with one allocated file on top of the real formatter:

```powershell
build\tests\RelWithDebInfo\xfs_tests.exe --make-fixture C:\temp\fixture.img
python tests\tools\fixture_probe.py C:\temp\fixture.img
python tests\tools\log_probe.py C:\temp\fixture.img --expect clean
```

The fixture is verified by an independent Python probe, and in CI by Linux
`xfs_repair -n`, `xfs_db` and a loop mount
(`tests/scripts/verify_fixture_linux.sh`) - the unit tests are only meaningful
if the image they use is a real XFS filesystem.

`tests/tools/log_probe.py` is the log oracle: it reimplements the record
format from the kernel specification (magic/version/cycle/uuid, `h_lsn`,
`h_len`, cycle stamping, the CRC over header plus stamped data, the op chain,
the `xfs_inode_log_format`/`xfs_log_dinode` payloads, buffer items, cleared
space) and also reads logs that the kernel itself wrote, which is checked in
CI against the Linux-populated and golden images.

### 2. CLI write path

```powershell
xfswin pwrite test.img /hello.bin 4096 --rw-experimental --from=overlay.bin
xfswin extend test.img /hello.bin 45000 --rw-experimental
xfswin truncate test.img /hello.bin 20000 --rw-experimental
xfswin create test.img /new.bin --rw-experimental --mode=0644
xfswin mkdir test.img /newdir --rw-experimental
xfswin unlink test.img /new.bin --rw-experimental
xfswin rmdir test.img /newdir --rw-experimental
xfswin check test.img
python tests\tools\rw2_probe.py test.img --inode 67
python tests\tools\rw3_probe.py test.img
```

`pwrite`, `extend` and `truncate` perform the same core mutations the WinFsp
callbacks do (image only, require `--rw-experimental`), so CI can verify writes
against Linux oracles without the WinFsp driver. `pwrite` supports growth now;
`extend` writes the deterministic pattern `((offset * 131) + 7) & 0xFF` (or a
`--fill=0xNN` constant) past EOF; `truncate` is `SetFileSize`. `create`,
`mkdir`, `unlink` and `rmdir` are the namespace mutations (`CreateRegularFile`,
`Mkdir`, `Unlink`, `RemoveEmptyDirectory`) with the same opt-in flag.

`tests/tools/rw3_probe.py` is the independent structural oracle for RW-3: it
parses the inobt and the AGI from the raw image, cross-checks their counters
against each other and the superblock, walks all four directory formats
(data/leaf/free blocks, bestfree tables, the da btree keys and the leaf chain)
and - with `--expect <spec.json>` - verifies an exact name/type/size listing.
It shares no code with the C++ writer.

### 3. WinFsp read-write mount

Without a driver, `xfswin selftest <image> --rw-experimental` drives the real
callbacks (open, write, flush, re-open, read back), then writes past EOF,
shrinks the file, grows it sparsely and shrinks it again, verifying the size and
the data through a fresh node; RW-3 extends it with create + write + re-open,
mkdir with enumeration, the non-empty rmdir refusal, delete-on-close with reads
on the unlinked inode after `Cleanup`, rmdir and name reuse. With a driver,
`tests/scripts/verify_mount_rw_windows.ps1` mounts the image read-write,
overwrites bytes in a file through WinFsp, verifies the read-back, asserts that
rename, symlink creation and writes to 0444 files are refused (create, delete
and mkdir are exercised by the adapter self-test and the RW-3 CLI images),
exercises growth, append handles and `SetEndOfFile` shrink/grow on an
existing file, then writes a second overlay through `xfswin pwrite` and runs
`xfswin check`.

### 4. Linux interoperability round trip (CI)

```text
Windows  format an image (read-write safe profile)
Linux    populate it with deterministic files (tests/tools/rw_content.py)
Windows  mount --rw-experimental, overwrite bytes in two files
Linux    sudo tests/scripts/verify_rw_linux.sh modified.img rw.img recipe overlay
```

The last step requires, in this order: `xfs_repair -n` clean, both modified
files equal to the content regenerated from `rw_content.py` with the overlays
applied, every other file byte-identical to the pristine image, identical
directory trees and a clean log. "Explorer shows the file" is not evidence;
this is.

### 5. Crash injection

```powershell
$env:XFSWIN_CRASH_AT = 'before_data_write'
xfswin pwrite image.img /hello.bin 0 --rw-experimental --text=x
# process terminates with exit code 0x58435348, no clean unmount
```

`:<hit>` selects which occurrence crashes (`after_metadata_write:3`). Point
names are stable and listed by `xfs::ParseCrashPoint`. RW-1 turns this into a
matrix over every point of one write:

```powershell
pwsh -File tests/scripts/verify_crash_windows.ps1 `
  -Exe build\src\RelWithDebInfo\xfswin.exe `
  -Tests build\tests\RelWithDebInfo\xfs_tests.exe -WorkDir out\crash
```

For each point the script copies the fixture image, runs the write with that
point armed and checks the *pre-replay* state with
`tests/tools/rw_crash_check.py`:

- the log has at most one torn record and it is the newest,
- the inode is never torn (magic, version, CRC),
- if the inode changed, a complete log record with the same LSN *and* the same
  timestamps describes it (write-ahead violation otherwise),
- the file data is old or new.

On Linux, `tests/scripts/verify_crash_linux.sh` mounts every image (so the
kernel replays the log) and requires the window contract:

| crash point | timestamps | data |
|---|---|---|
| `before_data_write` | old | old |
| `after_data_write`, `before_log_write`, `mid_log_record` | old | new |
| `after_log_commit`, `before_metadata_write`, `after_metadata_write`, `before/after_device_flush` | **new** (the kernel replayed our record) | new |
| `mid_data_write`, `mid_metadata_write` (need more than one mapping/buffer to trigger) | old, new | old, new, mixed |

plus a clean log after the unmount and `xfs_repair -n` staying silent. The
"new" timestamps are not guessed: `tests/tools/rw_replay_check.py` reads them
out of the complete log record in the image, which is what the kernel replays.

RW-2 doubles the matrix across an extend and a truncate
(`verify_crash_rw2_windows.ps1` + `rw2_crash_check.py`, replayed by
`verify_crash_rw2_linux.sh` + `rw2_crash_replay_check.py`): every point of both
write transactions, the data write, the flushes and the truncate transactions,
and the contract is old or new *size* with pixels that are never garbage.

RW-3 adds the namespace matrix:

```powershell
pwsh -File tests/scripts/verify_crash_rw3_windows.ps1 `
  -Exe build\src\RelWithDebInfo\xfswin.exe `
  -Tests build\tests\RelWithDebInfo\xfs_tests.exe -WorkDir out\crash-rw3
```

It kills `create` and `mkdir` (one transaction each), `unlink` (three: the
namespace record, the truncate, the ifree) and `rmdir` (two: the namespace
record and the ifree) at every crash point and hit. Each image is checked
before replay by `tests/tools/rw3_crash_check.py`: every written metadata buffer
individually intact, at most one torn newest record, the write-ahead contract
for every logged inode (a buffer carrying a complete record's LSN must match
that record's item), and - when the log is clean - the full inobt/AGI/directory
check plus an old-or-new namespace state. On Linux,
`tests/scripts/verify_crash_rw3_linux.sh` mounts every image (replay) and
requires via `rw3_crash_replay_check.py` that the name is absent or completely
created, or the original file/directory intact (with its original size and
content for unlink), then a clean log, silent `xfs_repair -n` and a passing
`rw3_probe.py`.

## Linux oracles (required for release)

These steps need a Linux environment. On the current development machine no
WSL/Docker installation is available, so they are scripted but not yet
executed here. Run them in CI or in a Linux VM/WSL distribution with
`xfsprogs` installed.

### A. Windows formatter → Linux validation

On Windows:

```powershell
.\build\src\RelWithDebInfo\xfswin.exe mkimage C:\temp\windows.img 300M
.\build\src\RelWithDebInfo\xfswin.exe format C:\temp\windows.img --yes-i-really-mean-it
copy C:\temp\windows.img \\wsl$\<distro>\tmp\windows.img   # or any transfer
```

On Linux:

```bash
sudo tests/scripts/verify_windows_image_linux.sh /tmp/windows.img
```

The script runs:

```bash
xfs_db -r -c 'sb 0' -c 'p' "$img"
xfs_repair -n "$img"          # must report no corruption
mount -o loop,ro "$img" /mnt  # must succeed
ls -laR /mnt
umount /mnt
```

Then (still on Linux, on a writable copy) exercise normal use:

```bash
truncate -s 300M copy.img && dd if=/tmp/windows.img of=copy.img conv=notrunc
sudo mkfs.xfs -f copy.img    # optional sanity of the toolchain
sudo mount -o loop copy.img /mnt
sudo touch /mnt/a /mnt/中文文件名.txt
sudo mkdir -p /mnt/dir/sub && sudo dd if=/dev/urandom of=/mnt/dir/big.bin bs=1M count=32
sudo ln -s /a /mnt/link
sudo umount /mnt
```

Copy the resulting image back to Windows and verify `xfswin check` and
`xfswin cat` (this is the reader test against a kernel-written filesystem).

### B. Golden images from Linux

```bash
sudo tests/scripts/make_golden_images.sh tests/images
```

The script creates (when run as root, loop-mounted and populated):

- empty default mkfs image
- empty image with all optional features off
- empty image with `bigtime=1` and with `nrext64=1`
- populated image: `hello.txt`, `folder/test.bin`, `中文文件名.txt`,
  `large.bin` (64 MiB), `sparse.img` (1 GiB sparse with a tail), symlink,
  hardlink, a 2000-file directory, deep directories, and a fragmented file
  requiring an extent B+tree.

Commit only small images (or none) to the repository; generate large ones in
CI. Expected content/baselines should be recorded from `xfs_db` output, not
by eyeballing Explorer.

### C. Differential testing with xfs_db

For every populated image, compare our answers with `xfs_db`:

```bash
# root inode
xfs_db -r -c 'inode 64' -c 'p core.mode core.size core.nlink core.format' img
# directory
xfs_db -r -c 'inode 64' -c 'p u' img
# extents of an inode
xfs_db -r -c 'inode <ino>' -c 'bmap' img
```

Our `xfswin stat` / `xfswin ls` output must match: inode number, size, mode,
extent list, block counts, timestamps.

## Corruption / fuzzing roadmap

The parser is designed to fail safely; extend the existing corruption tests
to:

- flip every byte of a superblock / AGF / AGI / inode / dir block and assert
  an error or a clean parse (never a crash, hang or out-of-range read),
- fuzz the superblock, inode, directory and B+tree parsers with libFuzzer
  once a Linux/CI fuzz harness is available (the core has no Windows
  dependency),
- verify that btree traversal stops on cycles, sibling loops, wrong levels,
  impossible record counts and cross-AG pointers.

## Regression rule

Any bug found in the wild must first become a failing test (unit, synthetic
image, or golden image), then be fixed. Do not "fix" metadata by guessing.
## CI (GitHub Actions, `.github/workflows/ci.yml`)

Every push runs fifteen jobs:

1. **windows-build-test** (windows-2022): builds with MSVC, runs the full test
   suite (including the journal, allocator and namespace tests), formats a
   300 MiB image, runs `xfswin check`, cross-checks the image with the
   independent Python probe, builds the fixture image (one allocated file) and
   probes it, exercises the CLI write path (`pwrite`, growth, truncate,
   refusals, `check` after the writes), generates the RW-2 round-trip images
   (`verify_rw2_windows.ps1`), the RW-3 round-trip images
   (`verify_rw3_windows.ps1`: create, mkdir, unlink, rmdir, a leaf directory
   with churn and a node-format directory, all verified with `rw3_probe.py`)
   and runs all three crash matrices (`verify_crash_windows.ps1`,
   `verify_crash_rw2_windows.ps1`, `verify_crash_rw3_windows.ps1`), uploading
   every image for the Linux jobs.
2. **linux-reference-probe** (ubuntu-24.04): dumps what the distro's real
   `mkfs.xfs` produces (geometries, root/rt inodes, AGI/AGF counters, repair
   output) for comparison against the formatter.
3. **linux-verify-windows** (ubuntu-24.04): takes the Windows-formatted image
   and runs the real `xfs_db` + `xfs_repair -n` + loop mounts (read-only and a
   writable copy), verifies the fixture image with `xfs_repair -n`, `xfs_db`
   and a content check, then populates the Windows-formatted image for the
   read-write round trip (`make_rw_image.sh`) and uploads it with the overlay
   and recipe. It also creates a *populated golden image* using Linux
   `mkfs.xfs` and the kernel (Unicode names, large file, sparse file, extent
   btree, 5000-entry directory, symlink, hardlink) plus a sha256 manifest.
4. **windows-read-linux-image**: runs `xfswin check` and compares directory
   listings, sizes, sha256 hashes, sparse/large file contents and symlink
   targets against the Linux-generated manifest (differential test).
5. **windows-mount**: installs the WinFsp runtime, builds with the WinFsp SDK,
   runs `xfswin selftest` (adapter callbacks without the driver, read-only and
   read-write, including RW-3 create/mkdir/delete-on-close/rmdir), mounts the
   Linux image at `X:`, verifies reads/hashes/Unicode names, asserts that every
   write-path callback is rejected as write-protected, then mounts the
   read-write image with `--rw-experimental`, overwrites bytes through the
   WinFsp `Write` callback, exercises growth, append handles and `SetEndOfFile`
   on an existing file, exercises create/mkdir/delete/rmdir (with rename and
   writes to 0444 files still refused), writes a second overlay through
   `xfswin pwrite` and uploads the modified image.
6. **linux-verify-rw** (ubuntu-24.04): verifies the read-write round trip:
   `xfs_repair -n` must stay clean, the two modified files must match the
   content regenerated by `tests/tools/rw_content.py` with the overlays
   applied, and every other file must be byte-identical to the pristine image.
   It also runs `tests/tools/log_probe.py` over the Linux-written and
   Windows-modified logs, so the parser oracle stays compatible with records
   the kernel writes.
7. **linux-verify-crash** (ubuntu-24.04): mounts every crash image from job 1,
   which makes the kernel *replay* the log record this project wrote, and
   requires the window contract from `docs/TESTING.md` §5 (old timestamps
   before the record is durable, the logged new ones after it, data old or new,
   a clean log afterwards and `xfs_repair -n` silence).
8. **linux-verify-rw2** (ubuntu-24.04): mounts the Windows-extended,
   Windows-truncated, block-reusing and btree-format images (allocator and
   extent-map mutations), verifies their content through the kernel and with
   the independent `rw2_probe.py`, requires `xfs_repair -n` silence, then
   appends to the extended file so the reverse job can read it back.
9. **linux-verify-rw2-crash** (ubuntu-24.04): replays every crash window of an
   extend and a truncate (`verify_crash_rw2_linux.sh`) and requires a legal old
   or new state with a clean log and clean repair.
10. **windows-verify-rw2-reverse** (windows-2022): reads the image Linux
    appended to with `xfswin cat` and checks the size and tail bytes - the
    reverse direction of interoperability (Linux continues writing, Windows
    reads it back).
11. **linux-verify-rw3** (ubuntu-24.04): mounts the RW-3 images (shortform
    files and directories, a grown leaf directory with churn, a node-format
    directory), verifies the exact names/types/sizes/contents through the
    kernel from the manifest, requires `xfs_repair -n` silence and a clean log,
    then creates, writes and removes files and directories inside the
    Windows-created `/r3dir` so the reverse job can read the result back.
12. **linux-verify-rw3-crash** (ubuntu-24.04): replays every RW-3 crash window
    (create, mkdir, unlink, rmdir) and requires the old or the complete new
    state, a clean log after replay, `xfs_repair -n` silence and a passing
    structural oracle.
13. **windows-verify-rw3-reverse** (windows-2022): lists the Linux-modified
    image with `xfswin ls`, checks that the Linux-created names are visible and
    the deleted one is gone, and reads the Linux-written content back with
    `xfswin cat`.
14. **windows-rawdevice-vhd** (windows-2022, Administrator): creates a fixed
    VHD, attaches it as a real disk, formats it through `\\.\PhysicalDriveN`
    with the raw device backend, reads it back with `info`/`check`/`ls`, and
    detaches it. Uploads the VHD.
15. **linux-verify-rawdevice** (ubuntu-24.04): strips the 512-byte VHD footer
    from the artifact and runs the full Linux verification (`xfs_db`,
    `xfs_repair -n`, read-only mount, writable copy) on the filesystem written
    through the raw device path -- the CI stand-in for Case C hardware.

This is the environment-independent version of the Linux/WinFsp acceptance
checks; see the jobs for the exact commands.
