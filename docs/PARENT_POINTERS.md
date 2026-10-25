# Parent pointers (RW-6E)

Status: **implemented and qualified by the Windows matrix and independent raw
oracle in this tree; the Linux kernel / xfsprogs qualification harness is part
of the change but its hosted run is the external acceptance gate.**  Ordinary
`--rw-experimental` now accepts a parent-pointer filesystem once the profile
passes `CheckWritableProfile`.  No CI run URL is claimed in this document; the
ledger below distinguishes local evidence from the Linux jobs that must be
recorded before a release is called qualified.

This is the qualification record and the research notebook for RW-6E.  The
authority is the source in `reference/linux/fs/xfs/` and
`reference/xfsprogs-7.2.0/`; where this note and the source differ, the source
wins.

## Source map

| Area | Files |
|---|---|
| Typed model, encode/decode, list/find | `include/xfs/parent.h`, `src/core/parent.cpp` |
| Namespace ↔ reverse audit | `src/core/parent.cpp` (`XfsFilesystem::VerifyParentPointers`) |
| Namespace mutation | `src/write/namespace.cpp` |
| Parent mutations through the attr engine | `src/write/parent_write.cpp`, `src/write/xattr_write_internal.h` |
| `ATTRI`/`ATTRD` parser | `src/core/xattr_intent.cpp` |
| Intent completion | `src/write/recovery_host.cpp` |
| Policy | `src/write/policy.cpp` |
| Formatter | `src/mkfs/geometry.cpp`, `src/mkfs/superblock_builder.cpp` |
| CLI | `src/cli/commands.cpp` (`parents`, `xattr list --include-private`) |
| Unit tests | `tests/unit/rw6e_tests.cpp` |
| Independent oracle | `tests/tools/rw6e_parent_probe.py` |
| Linux harness | `tests/scripts/make_rw6e_linux.sh`, `verify_rw6e_linux.sh`, `qualify_rw6e_host.sh` |

## On-disk encoding

Parent pointers are attributes in the private `XFS_ATTR_PARENT` namespace
(bit 3):

```text
attribute namespace   XFS_ATTR_PARENT (0x08)
attribute name        exact dirent name bytes (not a path)
attribute value       struct xfs_parent_rec (12 bytes, packed)
                        +0  __be64 p_ino
                        +8  __be32 p_gen
attribute hash        xfs_dir2_hashname(name)
                        ^ upper_32_bits(p_ino)
                        ^ lower_32_bits(p_ino)
```

Source: `libxfs/xfs_da_format.h` (`xfs_parent_rec`),
`libxfs/xfs_parent.c` (`xfs_parent_namecheck`, `xfs_parent_valuecheck`,
`xfs_parent_hashval`), `libxfs/xfs_dir2.c` (`xfs_dir2_namecheck`).

Consequences that the implementation enforces:

* the name is a dirent name: at most 255 bytes, no NUL, no slash, never empty;
* `.` and `..` are never stored as forward entries and are rejected as parent
  pointer names;
* the value is local, exactly 12 bytes, and never remote;
* the generation is **not** in the hash, but is part of the value lookup; the
  same parent inode and name with another generation is a stale pointer, not
  a second valid link;
* a parent inode is checked with the directory-inode rules (in range, not an
  internal rt/quota inode, and the live inode must be a directory);
* two hard links with the same name under different parents produce the same
  attribute *name* but different hashes and values.  The attribute key must
  therefore not be `namespace + name` alone.  RW-6E's duplicate check uses
  `(hash, name, value)` and the incremental tree match uses the full record.

## Root, directories and hard links

Upstream rules (`scrub/parent.c`, `repair/pptr.c`):

| Object | Parent pointers |
|---|---|
| root directory | **none**; root's `..` points to root |
| linked directory | exactly **one** pointer, regardless of `di_nlink` |
| empty/linked directory | `di_nlink == 2 + subdirectory_count` (normal directory rule) |
| unlinked directory (`nlink == 0`) | none while it waits for inactivation |
| regular file / symlink / device node | one per hard link; count equals `di_nlink` |
| `.` and `..` | no reverse records |

`xchk_parent_count_pptrs()` adds a virtual pointer for the dirtree root before
comparing directory counts.  The forward walk in this project reproduces that
rule directly: it never writes a root pointer, and the audit pretends the root
has one.

## Namespace lifecycles

All of these are one committed transaction in the Windows writer.  The child's
forward dirent and reverse attr change are in the same transaction; there is no
"dirent committed, parent pointer later" state.

### create / mkdir / symlink

`xfs_dir_create_child` inserts the dirent, updates the parent timestamps (and
the parent link count for a directory), then calls `xfs_parent_addname`.  The
new child inode is built with no attr fork; the first parent pointer creates
the shortform attr fork.  The Windows transaction does the same through the
RW-6D attr engine and logs one inode item carrying the new core and attr fork.

### link

`xfs_dir_add_child` inserts the destination dirent, bumps `nlink`, and adds the
`(dst_parent, dst_name)` record.  Existing parent pointers are preserved.

### unlink / rmdir

`xfs_dir_remove_child` drops the link, removes the dirent, then removes the
exact `(parent, name)` record.  For `rmdir` it also drops the parent's ".."
link, drops the child's "." link, and (when the parent is not root) rewrites the
now-unlinked child's `..` to root.  The parent pointer is removed first in the
Windows transaction, then the child directory editor is loaded from that
already-updated image so both edits share one forkoff.

The last-link path then follows the existing AGI-unlinked/inode-free lifecycle.
On a parent filesystem, `xfs_attr_shortform_remove` deliberately keeps an empty
shortform attr fork (`!xfs_has_parent(mp)` is part of the fork-removal
condition); the writer preserves that empty fork until the inode is freed.

### rename (same directory and cross-directory)

`xfs_dir_rename_children` always calls
`xfs_parent_replacename(old_dp, old_name, new_dp, new_name, child)` for the
moved inode, and `xfs_parent_removename(target_dp, target_name, target)` for a
replaced target.  A cross-directory directory move also rewrites `..` to the
new parent and adjusts both parents' link counts.  The Windows writer performs
the parent replace through the attr engine, re-parses the resulting image, then
runs the directory `..` editor on that image so the forkoff is consistent.

### rename replace

The target inode loses its pointer and one link (two for an empty directory
target); the moved inode replaces its old pointer with the new parent/name.
The target's zero-link lifecycle (AGI unlinked list, later free) is the normal
RW-4 path.  Same-inode rename is a no-op in the VFS and in this writer.

### same-inode rename

`vfs_rename`: `if (source == target) return 0;`.  No dirent, link count or
parent pointer changes.  The Windows code already returns the no-op outcome.

## Attribute tree interaction

Parent pointers use the same `xfs_attr` shortform → leaf → node → BMBT engine as
user attributes.  There is no separate parent-pointer tree writer.

* A new pointer can split a leaf, split a node, grow a multi-level node, or
  push the attr extent map into an attribute BMBT.
* Removing pointers can merge leaves, borrow from siblings, collapse the root,
  shrink the tree and eventually return to shortform.
* The exact entry (including the 12-byte value) is matched, so hard links with
  the same name under different parents never replace each other.
* `di_forkoff` changes go through the RW-6D data-fork relayout path, including
  data extents → BMBT and in-fork bmbt root promotion.
* An empty parent-only fork is retained on a parent filesystem, matching
  `xfs_attr_sf_removename`.

## Recovery and logged attributes

Linux parent-pointer updates are deferred, logged attribute operations:

```text
xfs_parent_addname/removename/replacename
    -> xfs_attr_setname -> XFS_DA_OP_LOGGED
    -> xfs_attr_defer_add
    -> ATTRI (XFS_ATTRI_OP_FLAGS_PPTR_SET/REMOVE/REPLACE)
    -> ATTRD
```

`xfs_attri_log_format` (40 bytes, host order) carries `alfi_igen` (child inode
generation) for parent operations.  `PPTR_REPLACE` overlays old/new name
lengths over the name-length field and logs five regions:

```text
format, old name, new name, old xfs_parent_rec, new xfs_parent_rec
```

`PPTR_SET` and `PPTR_REMOVE` log three regions: format, name, parent record.
All regions are 4-byte rounded.  Recovery opens the child by inode **and
generation**, so a reused inode number cannot receive a stale intent.

This project's normal writer logs the inode and attr-fork buffers directly in
one atomic transaction (no ATTRI/ATTRD of its own).  It can replay and complete
Linux ATTRI/ATTRD parent intents; completion is idempotent:

* `PPTR_SET`: upsert the exact record;
* `PPTR_REMOVE`: remove it if present, otherwise no-op;
* `PPTR_REPLACE`: remove the old record if present and add the new one; if the
  new record already exists, converge without duplicating it.

The parser validates namespace, value length, local placement, name rules,
inode range, hash and generation field before any replay write.  A parent
operation on a filesystem without the PARENT feature is refused.

`XFS_SB_FEAT_INCOMPAT_LOG_XATTRS` is **not** required by parent pointers.
Parent operations set `XFS_DA_OP_LOGGED` regardless of that mount-level
feature; `xfs_attri_validate()` gates PPTR operations on `xfs_has_parent()`,
not on `using_logged_xattrs`.  The two features are accepted independently.

## Exchange-range relationship

`xfs_mkfs.c` turns `exchange-range` on when parent pointers are enabled unless
the caller explicitly disabled it.  The formatter mirrors that dependency:
`--parent` and `--modern-default-profile` set both incompat bits.

The exchange-range **feature bit** does not change ordinary inode, directory,
attribute or reflink maintenance.  The exchange-range **operation** is not
implemented.  `XFS_LI_XMI`/`XFS_LI_XMD` and the realtime intent items are
classified as refused, so a log containing one is rejected before the first
write, never replayed halfway.  See `docs/DEFAULT_PROFILE.md`.

## `xfswin check` oracle

`CheckConsistency` performs a full bidirectional audit on `parent=1`:

1. walk the forward tree from the root, validating every directory format,
   `.`, `..`, link counts and dirent inode validity;
2. build the expected reverse set: one exact record per non-dot dirent, one
   for each linked directory, none for root, none for unlinked directories;
3. parse every allocated inode's attr fork (including inodes that are no
   longer reachable from the root tree, enumerated through the inobt);
4. compare exact records and classify:
   `missing`, `unexpected`, `wrong_parent_generation`, `wrong_name`,
   `duplicate`, `dangling_parent`, `non_directory_parent`, `wrong_dotdot`,
   `link_count_mismatch`, `malformed`, `unreachable_with_parents`.

A writable mount also runs the audit before setting `writable_` (after journal
recovery, never on the pre-replay view).  A missing, extra, stale or dangling
record therefore refuses the read-write mount instead of letting the next
operation compound the damage.  `xfswin parents <image> <path>` prints the
records for diagnostics, and `xfswin xattr list --include-private` shows
`parent.*` entries, and `xfswin xattr get <image> <path> parent.<name>
--include-private` returns the raw 12-byte record.  Ordinary `xattr
set/remove` refuses the parent namespace; only the namespace core and recovery
may mutate it.

## Independent raw oracle

`tests/tools/rw6e_parent_probe.py` does not call the C++ parser.  It uses the
independent Python inode/inobt/attr/directory parsers, reimplements the parent
hash, walks the tree, decodes raw attr forks, and reports the same categories
plus `unreachable_with_parents`.  It also verifies the root invariant and
`..` targets.

```bash
python tests/tools/rw6e_parent_probe.py image.img --require-parent \
    --json parent-oracle.json
```

## Linux qualification environment

`tests/scripts/qualify_rw6e_host.sh` refuses to run if `mkfs.xfs` does not
understand `parent=` or if the running kernel cannot mount a `parent=1` image.
It records `mkfs.xfs -V`, `xfs_info`, `xfs_db -r -c 'sb 0' -c p`, the kernel
`uname -a`, and the relevant config symbols.  It never falls back to
`xfs_repair`-only evidence.

The harness expects:

```text
xfsprogs 7.2.0 built from kernel.org
Linux kernel with XFS parent pointers and logged attrs (6.16 or newer)
CONFIG_XFS_DEBUG=y when the ATTRI/ATTRD debug path is required
```

The GitHub-hosted `linux-rw6e-parent`, `linux-verify-rw6e-windows` and
`linux-verify-rw6e-mutation` jobs run on `ubuntu-24.04` (kernel
6.17.0-1022-azure in October 2026). `tests/scripts/install_xfsprogs_7_2_0.sh`
builds the reference xfsprogs from kernel.org because the distribution package
is too old for `-n parent=1`. No opt-in variable, self-hosted runner, or QEMU
kernel artifact is involved. A job that cannot format or mount `parent=1`
fails; the harness never substitutes `xfs_repair`-only evidence.

The three hosted directions are:

1. `linux-verify-rw6e-windows` verifies the Windows formatter and 1 KiB
   Windows fixture with the real kernel, `xfs_repair -n`, `xfs_scrub` and the
   independent Python oracle.
2. `linux-rw6e-parent` creates a plain `mkfs.xfs -f` fixture, runs the
   deterministic 1000-hardlink workload, and publishes it for Windows.
3. `windows-rw6e-linux-roundtrip` mutates that Linux image through Windows
   (create, mkdir, link, cross-directory rename, replace, directory move,
   unlink, rmdir, symlink, xattr), audits it with `xfswin check --parents` and
   the Python oracle, then `linux-verify-rw6e-mutation` verifies both the
   original Linux workload and the Windows-added entries through the kernel.

## Qualification ledger

Rows marked "local" are evidence from this working tree.  Rows marked
"harness" require the recorded Linux run; this document does not claim them
without a URL.

| # | Gate | Local status | Linux/independent evidence |
|---|---|---|---|
| 1 | typed reader/encoder and hash | unit matrix pass | Python oracle recomputes hash/values |
| 2 | forward dir ↔ reverse pointer audit | `xfswin check` + oracle on fixture | harness |
| 3 | create / mkdir / link / unlink / rmdir | unit matrix pass, check clean | harness |
| 4 | same/cross-directory rename and replace | unit matrix pass, dotdot + PP checked | harness |
| 5 | directory move, parent generation and inode reuse | unit matrix pass | harness |
| 6 | hardlink-heavy parent tree: shortform → leaf → node → BMBT and back | 600-link/1 KiB unit test reaches node+BMBT and returns | harness |
| 7 | parent + user/trusted/security/ACL/remote xattrs | unit matrix pass | harness |
| 8 | corruption detection: missing PP, stale generation | unit matrix pass | Python oracle reports the same categories |
| 9 | ATTRI PPTR parser and completion | synthetic committed PPTR_REMOVE recovery test pass | harness with genuine Linux log |
| 10 | crash matrix for namespace + PP in one transaction | existing RW-3/RW-4 crash injection covers the transaction shape; parent-enabled fixture job added | harness |
| 11 | ENOSPC leaves dirent and PP unchanged | inherited RW-6D/RW-4 transaction behavior; parent fixture job added | harness |
| 12 | plain modern `mkfs.xfs` profile | formatter `--modern-default-profile` check + oracle clean; ordinary RW policy now accepts parent | harness |
| 13 | README/docs consistency | rewritten | review gate |

## Windows local evidence (2026-10-26)

* full suite: **193 tests passed**, including 12 RW-6E tests;
* parent fixture (4 KiB, modern profile): create/mkdir/link/same-name
  hardlinks/cross-directory rename/directory move/rmdir all kept `check` clean;
* independent Python oracle: `directory_edges == parent_pointers`, all error
  arrays empty;
* 600-hardlink/1 KiB test reached an attr BMBT and stepped back to one pointer
  with a clean audit at every sampled stage;
* the Windows namespace crash matrix (`tests/tools/rw6e_crash_windows.py`)
  recovered and audited **64/64** cases (create, mkdir, link, unlink,
  same/cross-directory rename, replace, directory move, rmdir × eight
  transaction write phases);
* synthetic committed `ATTRI` PPTR_REMOVE without `ATTRD` replayed and
  completed idempotently;
* 1 KiB formatter profile with `--modern-default-profile` passed `check` and
  the oracle with the PARENT plus exchange-range bits.

## Known limitations

* The exchange-range **operation** (XMI/XMD) is not implemented; a log that
  contains one is refused before writes.
* PhysicalDrive mounted read-write remains unqualified; experimental RW stays
  image-file-only.
* Quotas, realtime devices, external logs, metadir and zoned filesystems remain
  outside the writable profile.
* The debug-kernel ATTRI/ATTRD capture for ordinary logged xattrs
  (`docs/XATTR.md` row 6) still requires the opt-in modern-kernel harness; the
  parent-path PPTR parser/completion is covered locally and by the same
  harness.
