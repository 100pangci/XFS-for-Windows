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

Every push runs five jobs:

1. **windows-build-test** (windows-2022): builds with MSVC, runs the full test
   suite, formats a 300 MiB image, runs `xfswin check`, and cross-checks the
   image with the independent Python probe. Uploads `xfswin.exe` and the
   Windows-formatted image.
2. **linux-reference-probe** (ubuntu-24.04): dumps what the distro's real
   `mkfs.xfs` produces (geometries, root/rt inodes, AGI/AGF counters, repair
   output) for comparison against the formatter.
3. **linux-verify-windows** (ubuntu-24.04): takes the Windows-formatted image
   and runs the real `xfs_db` + `xfs_repair -n` + loop mounts (read-only and a
   writable copy). Then it creates a *populated golden image* using Linux
   `mkfs.xfs` and the kernel (Unicode names, large file, sparse file, extent
   btree, 5000-entry directory, symlink, hardlink) plus a sha256 manifest.
4. **windows-read-linux-image**: runs `xfswin check` and compares directory
   listings, sizes, sha256 hashes, sparse/large file contents and symlink
   targets against the Linux-generated manifest (differential test).
5. **windows-mount**: installs the WinFsp runtime, builds with the WinFsp SDK,
   runs `xfswin selftest` (adapter callbacks without the driver), then mounts
   the Linux image at `X:`, verifies reads/hashes/Unicode names, and asserts
   that create/delete/rename/mkdir are rejected as write-protected before
   unmounting.

This is the environment-independent version of the Linux/WinFsp acceptance
checks; see the jobs for the exact commands. All five jobs are green on the
current `main`.
