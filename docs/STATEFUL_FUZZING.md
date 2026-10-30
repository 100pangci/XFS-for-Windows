# Stateful filesystem fuzzing (FUZZ-2)

FUZZ-1 (the `tests/fuzz/` harnesses) answers one question: can arbitrary
untrusted metadata make a parser read out of bounds, use freed memory, hit UB
or hang?  FUZZ-2 answers a different one:

> Can a long sequence of individually legal filesystem mutations stay
> consistent across allocation pressure, directory/tree transitions and crash
> windows, and can an independent implementation recover and read the result?

The harness is deliberately test-only.  It does not add an XFS feature, change
the writable profile, alter a transaction, or expose allocator/journal internals.
It uses the same public mutation API, crash points, formatter and image
`BlockDevice` as the product code.

## 1. Architecture

```text
seed + workload
      |
      v
Generator ----> ModelFs (independent abstract state)
      |               |
      |               +--> expected paths/types/sizes/content/xattrs/nlink
      v
  Operation list
      |
      v
Runner ----> XfsFilesystem (real image) ----> trace + manifest
      |
      +--> optional crash point (XFSWIN_CRASH_AT)
      |
      v
Recovery (project engine or Linux kernel) ----> semantic verifier
      |
      +--> independent Python raw-image oracle
      +--> xfs_repair -n / xfs_db / xfs_scrub
      +--> real kernel mount
```

Source layout:

| Path | Responsibility |
|---|---|
| `tests/stateful/model.*` | independent filesystem model, operation encoding, manifest/state JSON |
| `tests/stateful/generator.*` | deterministic SplitMix64 generator, preconditions and workloads |
| `tests/stateful/runner.*` | image creation, operation execution, trace, crash/recovery modes |
| `tests/stateful/verifier.*` | comparison against the model and crash data contract |
| `tests/stateful/stateful_fuzz.cpp` | CLI, failure report and reproducer |
| `tests/tools/stateful_probe.py` | independent raw-image semantic oracle |
| `tests/tools/stateful_mount_check.py` | kernel-mounted semantic oracle |
| `tests/scripts/verify_stateful_linux.sh` | xfs_repair/mount/parent/scrub qualification |
| `tests/scripts/fuzz2_crash_matrix.sh` | deterministic crash-point matrix |
| `tests/scripts/reduce_stateful_trace.py` | delta-debugging reducer over the operation log |

The model never calls the project's directory parser, inode parser, extent
reader or xattr parser to decide an expected result.  It knows only the
operation and the previous model state.  The real filesystem is read through
the public API for cheap in-process checks; the independent Python parser and
the Linux kernel are the final judges.

## 2. Model

`ModelFs` contains directories, files, symlinks, hard-link groups (one node
with several paths), logical content, sizes, symlink targets and xattr values.
File content is generated deterministically from
`(seed, operation data_seed, offset)`; the trace therefore stores only the
operation, not a multi-megabyte payload.  Holes are logical zeros.  `nlink` is
derived from the number of directory entries, never stored as a separate
mutable field.  Directory nlink is `2 + subdirectory_count`.

The generator only emits operations whose precondition is true in the model:
unlink picks an existing non-directory, rmdir an existing empty directory,
hardlink a file/symlink, rename builds a legal destination (including
replace), xattr replace/remove picks an existing attribute, and reflink is
only generated on the reflink profile.  Rare illegal requests are not the
workload; the goal is to reach deep mutation paths.

## 3. Operations

`stateful_fuzz` currently generates:

```text
mkdir
create regular file
write
extend
overwrite
sparse write (hole before EOF moves)
truncate smaller
truncate larger
rename (same and cross directory, optional replace)
hardlink
unlink
rmdir
symlink (inline and remote)
set xattr
replace xattr
remove xattr
reflink/clone range                (modern profile)
CoW overwrite                      (modern profile, cloned range)
preallocate / unwritten extent
convert unwritten
```

Workloads select different pressure patterns:

| Workload | Emphasis |
|---|---|
| `mixed` | all operations, random legal sequence |
| `dir` | up to 950 entries in one directory, churn, rename, shortform → block → leaf → node |
| `xattr` | small/large/replace/remove, remote values, leaf/node and forkoff relayout |
| `alloc` | sequential and fragmented writes, sparse holes, truncate, btree growth/collapse, reflink/CoW |
| `enospc` | large preallocations, xattr growth and creates until `kNoSpace`, then continued operations; a deterministic release/reuse probe runs at the first `kNoSpace` |

The modern profile is the formatter's `modern_default_profile` (finobt,
inobtcount, sparse inodes, bigtime, NREXT64, rmapbt, reflink, parent pointers,
exchange-range bit).  The `default` profile is the legacy 4 KiB/512-byte
formatter profile; it exercises the same namespace/xattr/write paths without
rmap/reflink/parent pointers.

## 4. In-process checks

Every operation updates the model first and then the real filesystem.  On
success the runner compares paths, types, sizes, nlink, content, symlink
targets, xattrs, directory listings and hard-link inode identity at a
configurable interval (`--check-every`, default 5).  `CheckConsistency` is a
clean-log operation in this project, so it is run after the final clean
unmount rather than in the middle of a dirty-log sequence.  The final
`CheckConsistency` plus the independent Linux oracles are hard gates.

If `WriteFileData`, `AllocateUnwritten`, xattr allocation or a namespace
operation returns `kNoSpace`, the operation must leave the model state
unchanged for the operations whose transaction contract says so; the runner
restores the model and verifies the image before continuing.

The `enospc` workload additionally checks that space becomes usable again.  At
the first `kNoSpace` it truncates the largest data file to zero, requires every
one of that inode's data extents to disappear, preallocates one block on the
same file, requires a new extent to appear, and verifies the model again.  The
probe is deterministic for a fixed seed and is recorded in the trace as
`reuse-release` / `reuse-alloc`.

## 5. Trace, manifest and reproducer

A normal run writes:

```text
<image>.trace.txt      seed, profile, workload, every planned/executed operation
<image>.manifest.json  the exact final semantic state
```

A crash run writes the `before` and `after` model states in the crash-plan
manifest, then exits through the normal `XFSWIN_CRASH_AT` mechanism
(`0x58435348`). Range-contract manifests also carry a `.range.bin` sidecar with
the target file's before and after logical bytes and hashes; both independent
Python oracles validate allowed byte values from this sidecar. A failure prints:

```text
FUZZ-2 FAILURE
seed: ...
steps: ...
operation: ...
crash point: ...
image profile: ...
reproduce:
<exact command>
```

The same command is deterministic.  `--ops-out` writes every operation in a
tab-separated replay format; `--ops-file` replays that exact list and is what
the reducer uses.  A minimized failure is a small textual operation file, not a
large image.

`reduce_stateful_trace.py` parses an operation file, removes chunks while the
original failure signature still reproduces, and prints the reduced file and
command.  This is a plain delta debugger over the operation list; it does not
change the generator or the filesystem contract.

## 6. Crash model and contract

The harness never assumes that every operation is atomic.  The allowed states
after recovery are:

| Operation class | Contract |
|---|---|
| create, mkdir, symlink, link, unlink, rmdir, rename (all forms), set/replace/remove xattr | one namespace/attribute transaction: exactly the complete old or complete new state; no half dirent/nlink/parent-pointer state |
| convert unwritten | one transaction and no logical content change: old/new state is identical |
| write, extend, overwrite, sparse write, CoW overwrite | data is not journaled: size is old or new; bytes below the old EOF are old or new; newly visible bytes are new or zero; never uninitialised garbage |
| truncate smaller | size old or new; `[0,new_size)` is the old content; bytes beyond the new EOF may be old or zero |
| truncate larger, preallocate | size old or new; the prefix is unchanged; the new range reads as zero |
| reflink/clone | destination size old or new; the cloned range is old or new; bytes outside the range are unchanged; newly visible tail is zero or new |

These are the contracts already documented by `docs/ALLOCATOR.md`,
`docs/NAMESPACE.md` and `include/xfs/filesystem.h`; FUZZ-2 does not invent a
stronger POSIX durability promise.  Namespace recovery may leave a zero-link
inode on the AGI unlinked list; that is a legal crash state and the normal
recovery path truncates/frees it before the semantic check.

Crash points used by the matrix come from the writer's existing points:
transaction log and metadata phases, applicable data-write and remote-xattr
flush phases, and reflink/CoW staging and remap phases. A point that does not
occur in a selected operation is reported as skipped. `CRASH_DENSITY` selects
up to that many occurrences of each planned operation type. Recovery can be
performed twice:

1. the project engine (`--recover`), then Linux/xfsprogs verify the result;
2. the Linux kernel directly on the pre-recovery image, with the before/after
   manifest as the oracle.  This is the cross-implementation replay check.

## 7. Linux qualification

```bash
sudo tests/scripts/verify_stateful_linux.sh <image> <manifest> [workdir]
sudo tests/scripts/verify_stateful_linux.sh <image> <manifest> [workdir] --kernel-replay
```

The second form is for a dirty pre-recovery crash image: the raw semantic
probe is run only after the kernel has replayed the log.  The script refuses to
run as a non-root user (exit 2): it mounts a loop device.  Both forms require:

1. `xfs_repair -n` with no corruption/would-fix output;
2. the independent raw-image oracle (`stateful_probe.py`) matching the
   manifest;
3. the independent parent-pointer oracle on modern images;
4. a real kernel mount whose paths, types, nlink, sizes, content, xattrs,
   symlinks and hard-link identities match the manifest
   (`stateful_mount_check.py`);
5. `xfs_scrub -n` where `xfs_scrub` is installed and the running kernel exposes
   the metadata scrub ioctl; an absent tool or ioctl is recorded as a skip, not
   a pass;
6. a clean log (`xfs_logprint -t` reports `state: <CLEAN>`) and silent
   `xfs_repair -n` after unmount.

`xfs_repair` is never allowed to modify an image and then call it a pass;
`-n` is the only repair mode used here.  A kernel journal replay is not a
repair and is labelled separately.

## 8. CI levels

PR smoke:

* `stateful_fuzz_smoke` (CTest): 120 mixed operations on the modern profile;
* `stateful_fuzz_enospc` (CTest): 400 operations on a 300 MiB image,
  `--require-enospc` (the run must actually return `kNoSpace`, then pass the
  deterministic release/reuse probe);
* `linux-stateful-fuzz`: fixed mixed/dir/xattr/alloc/default/enospc seeds,
  each judged by xfsprogs and a kernel mount; the crash matrix runs the
  `mixed`/`xattr` plans (the only workloads whose plans contain namespace,
  xattr and data operations; `dir`/`alloc` plans are for the non-crash runs
  and `enospc` has no crash mode);
* `linux-stateful-fuzz-sanitizer`: the same bounded stateful smoke under
  ASan+UBSan;
* `FUZZ-2 soak` (weekly and `workflow_dispatch`): three fixed seeds ×
  `mixed dir xattr alloc`, 2000 operations each, kernel verification; failure
  artifacts keep the image and log;
* `windows-build-test` generates the Windows-writer images;
* `linux-verify-stateful-windows` verifies those images, including kernel
  replay of a Windows crash image.

Longer manual/stress runs use the same binary with more steps/seeds, for
example 1000–10000 operations with a higher crash density; `fuzz2_soak.sh`
below is the supported driver. They are not forced into every PR.

### Running the crash matrix

Without root, `SKIP_LINUX=1` runs every case through the project recovery
engine and the in-process verifier:

```bash
SKIP_LINUX=1 tests/scripts/fuzz2_crash_matrix.sh
```

Kernel qualification uses the loop-mounted images and therefore needs root:

```bash
sudo env EXE="$PWD/build/tests/stateful_fuzz" WORK=/var/tmp/fuzz2-crash SEED=0x123456789abcdef STEPS=80 CRASH_DENSITY=1 PROFILE=modern WORKLOAD=mixed tests/scripts/fuzz2_crash_matrix.sh
```

Keep that command on one line when copying it: a space after a backslash makes
the shell run the variable assignments as separate commands.  A non-root run
with `SKIP_LINUX=0` exits 2 before creating any image or mount; use `sudo` or
`SKIP_LINUX=1`.

Two wrappers keep the long steps separately executable and log every phase:

```bash
sudo bash tests/scripts/fuzz2_verify_prepared.sh   # SRC, NAMES
sudo bash tests/scripts/fuzz2_kernel_matrix.sh     # WORKLOAD, SEED, STEPS, ...
```

`fuzz2_verify_prepared.sh` expects `<name>.pre.img`, `<name>.img` and
`<name>.manifest.json` under `SRC` (default `/tmp/fuzz2-nonroot`, names
`ns xattr data`), copies each pre-recovery image before replaying it, and
writes `<name>.kernel.log` / `<name>.rec.log`.  `fuzz2_kernel_matrix.sh`
defaults to a workload-specific `WORK` and `/tmp/fuzz2-kernel-<workload>.log`.
Both refuse to start if a previous run left a mount or loop device under their
paths and print the exact detach command.

Each case verifies the pre-recovery image through a kernel replay and the
project-recovered image through `xfs_repair -n`, the raw oracle, the parent
oracle and a kernel mount. Unmount retries five times and then falls back to
`umount -l`; if the mount is still present it prints the exact cleanup
command. The matrix also registers an exit trap that lazy-unmounts any mount
left under `WORK` and, when started through sudo, chowns failure artifacts
back to the invoking user. If a process is killed with `SIGKILL`, finish the
cleanup explicitly:

```bash
findmnt -rn -o TARGET | grep '^/var/tmp/fuzz2-crash/'
sudo umount -l <mountpoint>
sudo losetup -a | grep fuzz2-crash
sudo rm -rf /var/tmp/fuzz2-crash
```

### Long-running soak

`fuzz2_soak.sh` runs every (seed, workload) pair, verifies each clean image
with the Linux oracles unless `SKIP_LINUX=1`, deletes successful images, keeps
failed ones, and resumes from `<base>.pass` markers:

```bash
sudo bash tests/scripts/fuzz2_soak.sh
SKIP_LINUX=1 bash tests/scripts/fuzz2_soak.sh
```

Defaults are three fixed seeds × `mixed dir xattr alloc`, 2000 operations,
512 MiB images and the `modern` profile.  Override with `SEEDS`, `WORKLOADS`,
`STEPS`, `PROFILE`, `SIZE_MB`, `CHECK_EVERY`, `KEEP_IMAGES`, `RESUME=0`,
`FAIL_FAST=0` and `RUN_TIMEOUT`; `WORK` selects the output directory
(`/var/tmp/fuzz2-soak` by default).  The `linux-stateful-soak` job in `ci.yml`
runs the same script weekly and on `workflow_dispatch` with `soak_only: true`,
with optional `soak_steps`, `soak_seeds` and `soak_workloads` inputs.

## 9. Bugs found by FUZZ-2

The first campaign found five real invariant violations.  Each has a
regression test; none was fixed by changing the model or weakening an
oracle.

### 9.1 Cached data chunk keeps the old EOF after a hole-only grow

* Reproducer: `seed=0x73a8175d0c41a9e3`, 120 mixed operations; the first
  failure is at operation 119, a `truncate_larger` on a file with a cached
  data chunk, followed by a read at the old EOF.
* Violated invariant: after `di_size` grows, a read at the old EOF must observe
  the new zero-filled range.
* Root cause: `SetFileSize` grow only called
  `InvalidateCaches(DataRange(ino, old_size, new_size-old_size))` inside the
  branch that zeroes existing written extents.  A pure-hole grow left a data
  cache chunk sized to the old EOF, so `ReadFile` returned a short count.
* Fix: invalidate the data range for every grow, outside the zero-range branch
  (`src/write/file_write.cpp`).
* Regression: `rw2_truncate_grow_invalidates_cached_tail` in
  `tests/unit/rw2_tests.cpp`.

### 9.2 Symlink with a parent pointer returns ENOSPC for a legal target

* Reproducer: `seed=0x1111111111111111`, mixed workload; a 298-byte symlink
  target on a parent=1 inode failed with
  `not enough literal space for the local data fork`.
* Violated invariant: every non-empty target below `XFS_SYMLINK_MAXLEN` must be
  creatable; the parent pointer reduces the literal area but does not make the
  target illegal.
* Root cause: `CreateSymlink` chose inline/local using the raw literal area
  (336 bytes for a 512-byte inode), then the parent-pointer attribute fork
  reduced the data fork below the target size.  The remote representation was
  never considered.
* Fix: compute the reduced local limit when `features.parent` is set and use
  the remote symlink representation above it
  (`src/write/namespace/namespace_symlink.cpp`).
* Regression: long-target case in
  `rw6e_create_mkdir_symlink_have_parents`
  (`tests/unit/rw6e_tests.cpp`).

### 9.3 rmap fragments split by an interleaved shared owner

* Reproducer: `seed=0x2222222222222222`, 250 mixed operations; a source extent
  was cloned in the middle, then extended contiguously.  The bmap extent
  normalized to one run while rmap kept two fragments around the clone owner.
  `xfs_repair -n` reported:
  `Incorrect reverse-mapping: saw (0/28) len 28 owner 132; should be ... len 35`.
  A later unlink then failed with
  `rmap unmap is only partially covered`.
* Violated invariant: one owner's physically and logically contiguous data
  extent has one canonical rmap record; `xfs_repair` treats the fragmented
  form as corruption.
* Root cause: `RmapEditor::MapExtent` only considered the immediate key
  neighbours.  A different owner's shared record sat between the two pieces,
  so the merge was missed even though both records were sharing-capable data.
* Fix: search all records for a legal left/right merge candidate, and merge
  across interleaved records only when every overlapped record is
  sharing-capable (`src/core/rmap_editor.cpp`).
* Regression: `rw6b_rmap_editor_merges_across_interleaved_owner`
  (`tests/unit/rw6b_tests.cpp`).

### 9.4 Data-fork btree stays after the attribute fork shrinks

* Reproducer: `seed=0x4444444444444444`, 400-operation xattr workload,
  `seed=0x3333333333333333` directory workload is not involved; operation 345
  sets a new attribute after a data extent was added at operation 343.
* Violated invariant: a data fork must use extents format whenever its extent
  count fits the literal area; `xfs_repair` reports
  `extent count for ino 137 data fork too low (6) for file format`.
* Root cause: the xattr forkoff relayout called `DataForkEditor` with
  `SetAllowCollapse(false)`.  When the attribute fork shrank and the data fork
  grew from 88 to 192 literal bytes, the existing btree became unnecessary but
  was kept.
* Fix: allow collapse when the data fork grows
  (`src/write/xattr_write.cpp`); shrinking still keeps the btree or moves its
  in-fork root.
* Regression: CTest `stateful_fuzz_xattr_regression` (seed `0x4444...`,
  400 xattr operations).  The harness also gained the optional
  `CheckOptions::all_allocated_inodes` walk so the invariant is checked for
  nested inodes, not only root children; the independent xfs_repair oracle
  remains the final gate.

### 9.5 v5 superblock omits XFS_SB_VERSION_ATTRBIT

* Reproducer: any clean modern image with a parent pointer or xattr, for
  example `stateful_fuzz --seed 1 --steps 1 --profile modern --workload mixed
  --image <img>`.  A kernel mount of the pre-fix image and `xfs_scrub -n`
  reports `/f1 attr block map: Repairs are required`; the image's
  `sb_versionnum` is `0xb4a5` instead of the `0xb4b5` that `mkfs.xfs` writes.
* Violated invariant: a v5 filesystem that stores attr forks must advertise
  `XFS_SB_VERSION_ATTRBIT` (0x0010).  The kernel maps the bit to
  `XFS_FEAT_ATTR`; `xchk_bmap` reports every inode with `i_forkoff != 0` as
  corrupt when the feature is absent.
* Root cause: `BuildSuperblockImage` wrote the canonical v5 versionnum without
  the attribute bit.  The bit is not part of the kernel's
  `xfs_sb_good_version` required mask (0xb4a0), so the project parser and
  `xfs_repair -n` accepted the malformed superblock.
* Fix: set `XFS_SB_VERSION_ATTRBIT` in the shared superblock image
  (`src/mkfs/superblock_builder.cpp`); the same image is written as the
  primary and every AG secondary superblock.
* Regression: `formatter_roundtrip` asserts the bit in the primary and every
  secondary superblock; the independent `xfs_probe.py` and `rw2_probe.py`
  reject a v5 image without it, so the stateful and crash pipelines gate on
  it without root; the hardcoded fuzz fixtures use the canonical `0xb4b5`.

## 10. Remaining blind spots

* Single-threaded deterministic sequences only; concurrent stateful fuzzing is
  a later phase.
* The crash matrix exercises the writer's named points, not every possible
  process kill instruction boundary.
* Reflink/CoW crash semantics are checked structurally and by the documented
  data contract, but the first phase does not enumerate every BUI/CUI/RUI
  intent combination.
* Directory format pressure in PR smoke is bounded; the weekly soak and the
  local `fuzz2_soak.sh` are what reach the largest leaf/node trees.
* The independent Python probe validates the model's semantic objects; it is
  not a replacement for `xfs_repair -n`, `xfs_scrub` or a kernel mount.
