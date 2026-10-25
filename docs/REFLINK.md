# Reflink, refcountbt and CoW (RW-6C)

Status: **qualification only**. Ordinary `--rw-experimental` still refuses a
`reflink=1` filesystem. The implementation is reachable only through the
hidden `--rw-reflink-qualification` switch (CLI `--rw-reflink-qualification`,
`MountOptions::allow_reflink_qualification`) and through the recovery-only
path (`xfswin recover` / `MountOptions::recovery_only`). The public gate is
not opened by this document; it opens only when every gate in the ledger below
has independent Linux evidence.

This milestone implements:

* a typed, fully verified refcountbt reader;
* a writable refcount B+tree with leaf/node split, merge, borrow, root growth
  and collapse;
* native range reflink (`XfsFilesystem::ReflinkRange`);
* out-of-place CoW writes for shared data extents;
* shared-extent truncate, unlink and inode-free lifecycles;
* CUI/CUD log intent parsing, preflight, lifetime matching and completion;
* mount-time orphan CoW staging cleanup;
* formatter support for `reflink=1,rmapbt=0` and `reflink=1,rmapbt=1`;
* a qualification-only CLI and an independent Python oracle.

It deliberately does **not** support realtime refcount, external logs,
parent-pointers, xattrs, quotas, metadir or online repair. Attribute extents
still refuse inode freeing, as in RW-6B.

## Source map

| Area | Files |
|---|---|
| Typed reader / verifier | `include/xfs/refcount.h`, `src/core/refcount.cpp` |
| Writable refcount B+tree and logical adjustments | `include/xfs/refcount_editor.h`, `src/core/refcount_editor.cpp` |
| Allocator/rmap/refcount integration | `include/xfs/alloc.h`, `src/write/alloc.cpp` |
| Native clone | `src/write/reflink.cpp` |
| CoW write and shared-aware size changes | `src/write/file_write.cpp` |
| Data-fork release hook | `src/write/bmap.cpp` |
| CUI/CUD format | `include/xfs/refcount_intent.h`, `src/core/refcount_intent.cpp` |
| BUI/BUD format (deferred data-fork map/unmap) | `include/xfs/bmap_intent.h`, `src/core/bmap_intent.cpp` |
| Intent preflight and lifetime | `src/recovery/log_scan.cpp` |
| Intent completion and orphan CoW cleanup | `src/write/recovery_host.cpp` |
| Formatter | `src/mkfs/geometry.cpp`, `src/mkfs/ag_builder.cpp`, `src/mkfs/formatter.cpp`, `src/mkfs/superblock_builder.cpp` |
| Policy / CLI | `src/write/policy.cpp`, `src/cli/commands.cpp` |
| Unit tests | `tests/unit/rw6c_tests.cpp` |
| Independent oracle | `tests/tools/rw6c_refcount_probe.py` |

The local reference trees are `reference/linux/fs/xfs/` and
`reference/xfsprogs-7.2.0/`. No GPL code is copied. The algorithms were
reimplemented from the on-disk format and the observable state machine in
`libxfs/xfs_refcount.c`, `libxfs/xfs_refcount_btree.c`,
`xfs_refcount_item.c` and `xfs_reflink.c`.

## On-disk model

### Refcount records

A refcountbt leaf record is 12 bytes:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `rc_startblock` | encoded AG block |
| 4 | `rc_blockcount` | length in filesystem blocks |
| 8 | `rc_refcount` | number of owners, or 1 for CoW staging |

The encoded startblock has two domains:

* `startblock < 2^31`: ordinary shared written file data, `refcount >= 2`;
* `startblock | 2^31`: CoW staging, `refcount == 1`.

Records are ordered by the encoded key. Consequently CoW records sort after
all shared records even though their physical blocks may be lower. Physical
ranges in the two domains are disjoint. Every other allocation (metadata,
attributes, unwritten extents, free space) is implied refcount one and has no
record.

Reflink requires every AG to have fewer than `2^31` blocks, because bit 31 is
the CoW flag. The reader, policy and formatter enforce that rule.

### B+tree

Refcountbt uses the v5 short B+tree header (`R3FC`), one 4-byte key per node
entry, 4-byte child pointers and the 12-byte leaf record. The tree is a normal
non-overlapping B+tree (unlike rmapbt):

* parent low key is exactly the first encoded key of the child subtree;
* sibling links are maintained at every level;
* the root may be an empty leaf;
* non-root blocks stay at least half full;
* `agf_refcount_root`, `agf_refcount_level` and `agf_refcount_blocks` describe
  the tree; `agf_refcount_blocks` includes the root;
* `agf_btreeblks` deliberately **excludes** refcountbt, matching Linux.

Tree blocks are allocated from ordinary free space with rmap owner
`OWN_REFC`, never from the AGFL. This avoids recursive refcount allocation
when the refcount tree itself grows. The free-space and rmap edits happen in
the same transaction as the refcount tree edit.

## Writable algorithms

### Logical adjustment

`RefcountEditor::Increase` and `Decrease` implement
`xfs_refcount_adjust(SHARED, ...)`:

1. reject zero-length or out-of-AG ranges;
2. reject overlap with CoW staging;
3. split existing records at both range boundaries;
4. synthesize implied refcount-one segments for holes;
5. increase/decrease the middle;
6. delete records that fall to one;
7. invoke the free-space callback only when a range reaches zero;
8. merge adjacent same-domain records with the same refcount;
9. apply the resulting record set to the B+tree.

A record at `XFS_REFC_REFCOUNT_MAX` is pinned and is never incremented or
decremented past the maximum.

`CowAlloc` inserts an exact staging record. `CowFree` splits at the requested
boundaries, requires the whole range to be covered by staging records, and
removes it. A gap or a range that was never reserved is corruption.

### Native clone

`XfsFilesystem::ReflinkRange` follows `xfs_reflink_remap_range`:

* both paths must be regular files;
* source and destination offsets must be filesystem-block aligned;
* length zero means "to source EOF"; a non-EOF length must be block aligned;
* holes and unwritten source extents become holes in the destination;
* written source extents are shared, never copied;
* a partial final source block is shared only when the clone also ends at the
  destination EOF;
* a destination range that already maps exactly the desired source blocks is
  left alone (idempotent clone);
* overlapping ranges inside the same file are refused;
* the destination range is replaced in rolling transactions of at most 32
  filesystem blocks;
* source and destination both receive `XFS_DIFLAG2_REFLINK` in the same
  transaction as the first shared mapping.

For each written piece the transaction:

1. removes the old destination mapping and releases its owner/refcount;
2. increases the source extent's refcount;
3. adds the destination rmap owner;
4. inserts the destination data-fork extent;
5. writes the destination inode and the source reflink flag;
6. commits log, metadata and flush in the normal transaction order.

### CoW write

A write whose affected written extent is shared uses an out-of-place CoW:

1. allocate private staging blocks with rmap owner `OWN_COW` and a refcountbt
   CoW record, in transaction 1;
2. write the user bytes and preserved partial-block bytes to the staging
   blocks;
3. flush the data device **before** any metadata can point at it;
4. in one atomic remap transaction, remove the old destination mappings and
   release their owners/refcounts, convert each staging owner to the inode,
   remove the CoW records, insert the new private extents, and update the
   inode size/flags/timestamps;
5. log, write metadata and flush in the normal order.

If staging or the data write fails before the remap transaction, the staging
extents are cancelled in a cleanup transaction. If cleanup itself fails, the
records remain for mount-time orphan recovery. Once the remap transaction has
been submitted, staging is never cancelled speculatively: recovery owns the
outcome.

A read that observes the old inode sees the old bytes; a read after the remap
sees the new bytes. The staged data is durable first, so the remap can never
expose stale disk contents.

### Shared truncate, unlink and size changes

`DataForkEditor::SetFreeRemovedBlocks(true)` now releases blocks through
`BlockAllocator::ReleaseData` instead of an unconditional free. Release does:

* remove the inode's rmap owner;
* decrease the refcount;
* free the physical run only when the refcount reaches one (the record is
  deleted and the free-space callback runs).

This is used by `SetFileSize`, `TruncateForFree`, unlink/rmdir replacement and
the CoW remap. A partial final block that is shared is CoW-replaced before it
is zeroed beyond the new EOF, so growing a clone cannot expose bytes that the
other owner did not intend to share.

### Orphan CoW staging

Every writable mount of a reflink filesystem runs the equivalent of
`xfs_reflink_recover_cow` after unlinked-inode recovery. It reads every AG's
refcount tree, and for each COW staging record:

1. removes the `OWN_COW` rmap and refcount record;
2. returns the blocks to free space;
3. commits one transaction per extent.

A crash between the staging transaction and the remap leaves exactly this
state. Active inode mappings never reference CoW staging blocks, so all such
records are orphans at mount time.

## CUI/CUD recovery

CUI and CUD are host-order log items.

CUI format region:

| Offset | Field |
|---|---|
| 0 | `cui_type` (`0x1242`) |
| 2 | `cui_size` (must be 1) |
| 4 | `cui_nextents` |
| 8 | `cui_id` |
| 16 | first extent: `startblock` u64, `len` u32, `flags` u32 |

The low byte of `flags` is the operation:

| Value | Operation |
|---|---|
| 1 | increase shared refcount |
| 2 | decrease shared refcount |
| 3 | allocate CoW staging |
| 4 | free CoW staging |

CUD is a 16-byte region with `cud_cui_id` at offset 8.

Preflight parses and validates every CUI/CUD before the first write. Unresolved
CUI intents are allowed; resolution by `CollectPendingIntents` is type-keyed
and ordered, including pointer-derived ID reuse. Completion runs one normal
transaction per intent:

* increase/decrease use the shared adjustment path (decrease uses
  `OWN_UNKNOWN` only for a range reaching zero, because the original mapping
  owner was already removed);
* allocate-CoW and free-CoW touch only the refcount half; rmap and free-space
  work was committed with the original transaction, matching Linux
  `xfs_refcount_alloc_cow_extent` / `_free_cow_extent`;
* the matching CUD is logged in the same transaction as the refcount buffers;
* a crash before log commit leaves the intent pending; a crash after commit
  leaves the CUD and the new refcount state.

BUI/BUD are the companion deferred data-fork mapping intents.  A reflink clone
and a CoW remap both log the destination mapping through a BUI, often in the
same transaction as the CUI.  The parser also accepts the RUI shared variants
(`MAP_SHARED`, `UNMAP_SHARED`, `CONVERT_SHARED`) on a reflink filesystem; on a
non-reflink filesystem they remain refused before the first write.  Recovery accepts a BUI only with exactly one
map or unmap operation, validates it against the inode and AG geometry, and
completes it in a normal transaction:

* MAP inserts the data-fork extent, accounts `di_nblocks`, and adds the inode
  rmap owner (written or unwritten);
* UNMAP removes the inode rmap owner and the data-fork extent; the paired EFI,
  if present, performs the physical free.  A missing rmap owner is accepted
  when no other owner overlaps, which makes BUI/EFI completion order
  independent;
* a matching BUD is logged in the same transaction as the inode/bmap buffers.

Attribute-fork and realtime BUI extents are refused.  The completion order is
the recovered log item order; BUI map/unmap is idempotent with respect to a
previously completed EFI free.

Mount then runs orphan CoW recovery, which removes any completed allocate-CoW
record that no inode references.

## Formatter

`FormatOptions::reflink` and `--reflink` enable `ro_compat.reflink` and write a
valid empty refcount root into every AG. `--rmapbt --reflink` is the combined
profile. The formatter:

* reserves the refcount root block in every AG;
* increments the superblock free-block accounting for it;
* adds `OWN_REFC` to the formatter rmap tree (when rmapbt is on);
* sets `agf_refcount_root/level/blocks` (level 1, blocks 1);
* leaves `agf_btreeblks` unchanged because refcountbt is not included.

Reflink without rmapbt is legal and supported; the writer falls back to
free-space-only release accounting for shared blocks.

## Policy and refusal

`CheckWritableProfile` keeps the ordinary reflink refusal. The hidden
qualification flag is independent of `read_write`:

* `--rw-experimental` alone: `reflink=1` is refused before any write;
* `--rw-experimental --rw-reflink-qualification`: hidden mutation path;
* `xfswin recover --yes-i-really-mean-it`: recovery-only path, accepts reflink
  to complete intents and orphan staging, then closes the mount;
* read-only mounts are unchanged.

The refusal is an error, never a silent read-only or partial-write mount.

## Testing

Local tests:

```text
xfs_tests.exe --filter=rw6c
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

`tests/unit/rw6c_tests.cpp` covers:

* formatter layout for both reflink profiles and all AGs;
* typed lookup, trimmed interval lookup, corruption and AGF counter rejects;
* logical increase/decrease, holes, split/merge, pinned maximum, CoW domains;
* real B+tree split to level 3 and collapse back to level 1;
* 100-file persistent refcount tree growth (level 2) and collapse;
* clone/holes/unwritten/partial EOF/all-hole/local-fork behavior;
* CoW write, destination isolation, source preservation and cache reads;
* three-way clones, truncate, unlink and last-reference release;
* the ordinary read-write refusal;
* orphan CoW staging cleanup on the next writable mount;
* CUI/CUD structural matrices and lifetime/ID reuse;
* overlapping written-data rmap records with one private CoW block.

The independent oracle is deliberately separate from the C++ reader:

```text
python tests/tools/rw6c_refcount_probe.py <image> [--allow-no-rmapbt] [--json out]
```

It derives the number of written data-fork mappings per physical block from
the forward inobt, inode forks and bmap btrees, then requires every shared
record to match that count exactly. It also feeds the same forward ownership
into the independent RW-6B rmap oracle, extended for `OWN_REFC`, `OWN_COW` and
legal shared-data overlaps.

### Local evidence (not qualification)

The following was observed on Windows before the Linux qualification run. It
is recorded to make the implementation state auditable, not to claim any
Linux gate:

* full unit/integration suite: **167 tests passed**;
* `--make-rw6c-fixture` produced a Windows-formatted, 1 KiB-block
  `reflink=1,rmapbt=1` image with 100 separated shared extents; the independent
  oracle accepted it (104 shared records / 123 shared blocks, refcount tree
  level 2, no CoW);
* `rw6c_windows_mutate.py` completed CoW source isolation, four clones,
  partial-EOF growth, shared truncate and unlink; all content assertions and
  the oracle passed;
* the no-write refusal matrix left both the 4 KiB oracle image and the 1 KiB
  fixture byte-identical;
* the clone/CoW/truncate crash matrix recovered **17/17** injection points on
  both the rmap and reflink-only profiles, with the oracle clean after every
  restart;
* the near-ENOSPC run drained free space to 41 blocks; a four-block CoW write
  was refused with `no space left`, left `/base/cow.bin` byte-identical,
  leaked no CoW record and passed the oracle;
* a simulated Linux reverse pass (Windows-created equivalent data) was read,
  checked against the manifest, mutated again and accepted by the oracle;
* the genuine Linux CUI/BUI/CUD snapshot captured by run `40159734421`
  (`linux-generate-rw6c-cui`, CUI op 1 + BUI map op 1) was completed by
  Windows both directly and through 17 recovery crash points; the resulting
  sharing/CoW/free-block state matched the kernel-recovered oracle.  Linux
  re-verification of the Windows output is still a CI gate.

Genuine Linux-created images, genuine CUI/CUD logs, Linux crash replay and
Linux xfs_repair/mount evidence remain the CI gates below. The local fixture
helper is test support; it is never described as Linux evidence.

## Qualification ledger

This ledger is the only place that may authorize opening the public gate. A
row may be marked **verified** only when the linked CI evidence exists and
passed; local tests and Windows self-consistency are not Linux evidence.

| # | Gate | Local status | Linux/independent evidence |
|---|---|---|---|
| 1 | Formatter emits both legal profiles (`reflink=1,rmapbt=0/1`) accepted by Linux `xfs_repair -n` and mount | formatter/reader unit tests pass | **pending CI** |
| 2 | Typed reader rejects corruption and agrees with the independent oracle | unit matrix, oracle smoke pass | **pending CI on Linux-created images** |
| 3 | Refcount B+tree reaches level >= 2 in a persistent real image and collapses cleanly | unit level-3 editor test and level-2 100-file test pass | **pending Linux verify** |
| 4 | Linux-created reflink fixture round trip, both profiles | none locally (no Linux env) | **pending CI** |
| 5 | Clone semantics: holes, unwritten, partial EOF, all-hole and self rules | unit tests pass | **pending Linux mount/content verify** |
| 6 | CoW write ordering and destination isolation | unit tests pass | **pending Linux/crash verify** |
| 7 | Shared truncate/unlink, three-way clone and last-reference release | unit tests pass | **pending Linux verify** |
| 8 | Genuine Linux CUI without CUD completes on Windows, including its BUI | local completion and 17 crash points against run `40159734421` artifact; kernel oracle matched | **pending Linux verification of the Windows output** |
| 9 | Orphan CoW staging recovery after a crash | unit mount-cleanup test passes | **pending crash CI** |
| 10 | Crash matrix for clone, CoW, truncate/unlink and recovery; every image accepted by Linux | crash points exist; Windows-side replay not yet run | **pending CI** |
| 11 | ENOSPC and no-write refusal matrix leave the image byte-identical or structurally valid | mount refusal unit test | **pending CI** |
| 12 | Cross-OS churn/reverse interoperability and a final code/document audit | none | **pending CI** |

No row currently claims Linux verification. The ordinary gate must remain
closed until rows 1-12 are backed by real evidence and this table is updated
to name the runs/artifacts.

## Known limitations

* Realtime refcountbt (`refcount=1` for the realtime group) is unsupported.
* Refcount intents are completed one CUI per transaction. A 1024-extent CUI
  is covered by the unit matrix, but a CUI larger than one transaction budget
  is refused rather than split with a fresh continuation intent.
* Attribute forks with allocated extents still cannot be freed; BUI
  attribute/realtime mappings are refused.
* A BUI is accepted only with one mapping (the Linux recovery constraint).
* Exchange-range, online repair, quotas, parent pointers and metadir remain
  outside the writable profile.
* The Windows formatter's refcount tree is a single empty root leaf; tree
  growth is exercised by the writer, not by mkfs.
* The independent oracle validates metadata and forward sharing, not file
  content; content is checked by the Linux mount scripts.
