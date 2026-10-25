# Reverse mapping ledger (RW-6B)

Status: **qualified ordinary experimental writes** (`rmapbt=1,reflink=0`
only). Code revision `5a41fca` passed all **65 jobs** in
[run 40107607956](https://github.com/100pangci/XFS-for-Windows/actions/runs/40107607956),
using ordinary `--rw-experimental`, including real-image structural/refusal
gates. Reflink/refcount/CoW and xattr modification remain refused. This is
experimental image-file support, not production/raw-device write support or
qualification of every possible kernel RUI producer; sampling limits remain
explicit below.

## 1. Baseline and sources

PR [#6](https://github.com/100pangci/XFS-for-Windows/pull/6) merged on
2026-10-22, with all reported checks successful; merge commit
`18c54d48bc216b070398c0245625c0b26bb8e63d`, RW-6A HEAD
`ff65d2c0668dc07dfa49a5514b57465f9148453d`. `rw-6b` starts at that merge.

The specification sources are the **local** `reference/linux/fs/xfs/` and
`reference/xfsprogs-7.2.0/` trees (gitignored, not build dependencies). No GPL
implementation is copied. To identify the consulted snapshot even without a
kernel checkout revision, SHA256 of the local files is:

| file | SHA256 |
|---|---|
| Linux `libxfs/xfs_rmap.c` | `fed3589937c7a8139e292692f83e5568dd7fcec8a8266b0ba7d80a2a1308fc04` |
| Linux `libxfs/xfs_rmap_btree.c` | `3356314bcf3f3a7e42611764ea53d19b4407a829d3890a3bb0c20a7ccda408e6` |
| Linux `xfs_rmap_item.c` | `f9843badcba490e73954059f6ffa13e947823fb31379922bbe6b7768c11b9f95` |
| xfsprogs `libxfs/xfs_rmap.c` | `928b83e388451ab9cbabcac33229aa789fe87e6ee9c28f74c4d1461b03a7c5ed` |

Source map:

* `libxfs/xfs_format.h`, `xfs_rmap.h`, `xfs_rmap_btree.h`: layouts/packing.
* `libxfs/xfs_rmap.c`: record validation, lookup, map/unmap/convert,
  direct allocator updates, deferred operations, EFI wildcard-owner semantics.
* `libxfs/xfs_rmap_btree.c`: key comparison, high keys, AGFL block lifecycle,
  root and block counters, verifiers, max/min records.
* `libxfs/xfs_btree.c`: overlapping key propagation, split/shift/join/root
  changes (`xfs_btree_get_leaf_keys`, `xfs_btree_get_node_keys`).
* `libxfs/xfs_alloc.c`: `xfs_alloc_fix_freelist`, get/put freelist,
  `xfs_free_ag_extent`, allocation finish, minimum freelist calculation.
* `libxfs/xfs_ialloc.c`, `xfs_ialloc_btree.c`, `xfs_bmap_btree.c`:
  inode backing, inobt/finobt and bmbt ownership.
* `xfs_rmap_item.[ch]`, `libxfs/xfs_log_format.h`, `xfs_defer.c`,
  `xfs_log_recover.c`: intent formats, commit/cancellation and completion.
* xfsprogs `repair/rmap.c` (`rmaps_are_mergeable`, forward observations),
  `repair/scan.c` (`scan_rmapbt`), `db/agf.c`, `db/btblock.c`,
  `logprint/log_redo.c`: independent interpretation/diagnostics.

Final oracles are Linux kernel mount/replay, `xfs_repair -n`, `xfs_db`, and
`xfs_logprint`. Unit fixtures do not substitute for Linux-created fixtures.

## 2. On-disk block, record and keys

rmapbt requires v5 CRC metadata. Per-AG root/height live in the AGF. A normal
block has magic `RMB3` (`0x524d4233`) and the 56-byte short v5 btree header:

| offset | field | encoding |
|---|---|---|
| 0 | magic | be32 |
| 4 / 6 | level / numrecs | be16 / be16 |
| 8 / 12 | left / right sibling | be32 AG block, null = `0xffffffff` |
| 16 | block address | be64 **512-byte daddr**, not fsblock |
| 24 | LSN | be64 |
| 32 | UUID | 16 bytes, metadata UUID |
| 48 | header owner | be32 **AG number**, not record owner |
| 52 | CRC32c | le32, checksum of whole filesystem block |

A leaf record is **24 bytes**: startblock be32 at +0, blockcount be32 at +4,
owner be64 at +8, packed offset be64 at +16. Start/length are AG-relative
filesystem blocks. Length is nonzero and cannot cross the AG boundary.

The packed offset uses bit 63 = attr fork, 62 = bmbt block, 61 = unwritten,
54..60 = reserved zero, 0..53 = logical filesystem-block offset. Host-side
flags are separate: attr=1, bmbt=2, unwritten=4. Never confuse these with
the log's operation flags.

Each key is **20 packed bytes**, not a padded C++ struct: startblock be32,
owner be64, packed offset be64. Compare unsigned lexicographically by
`(startblock, owner, offset with UNWRITTEN masked out)`. Attr and bmbt bits
participate in ordering; unwritten is a record attribute, not part of the
key. Equal key records differing only in unwritten state are not distinct.

An internal entry has an **interleaved low/high key pair** (40 bytes), then
the separate pointer array. For zero-based entry i:

```text
low key  = 56 + i*40
high key = 56 + i*40 + 20
pointer  = 56 + max_node_records*40 + i*4
max_leaf = floor((blocksize - 56) / 24)
max_node = floor((blocksize - 56) / 44)
min_nonroot = floor(max_records / 2)
```

Low = first record/subtree key. Record high = start+length-1; add length-1
to logical offset only for inode fork extents, **not** metadata or bmbt
records; clear unwritten in both keys. Subtree high = **maximum of every
child/record high key**, not necessarily the last one's high key. This
matters for nested/overlapping extents on read-only reflink filesystems.
All ancestor high keys can change on update, not merely the leftmost key.

Full traversal must verify CRC/identity, exact child levels, bounded counts,
valid pointers, both sibling directions at every level, global record order,
child low/high equality, cycle/multiply-referenced blocks and AGF reachable
block count. A walk can detect missing reachable blocks and a count mismatch;
finding an orphan outside the root requires the independent ownership scan.

## 3. Owners and forward metadata

Special owners are unsigned 64-bit encodings of negative values; high bit
distinguishes them from inode numbers. `OWN_NULL=-1` (skip/growfs) and
`OWN_UNKNOWN=-2` (EFI recovery wildcard) are **API sentinels**, never normal
on-disk record owners. `OWN_MIN=-10` is a guard, not an allocated owner.
The kernel's simple record check includes that endpoint; xfs_repair's
`scan_rmapbt` requires `owner > OWN_MIN`. The reader uses the real owners:

| value | owner | expected forward source |
|---|---|---|
| -3 | `OWN_FS` | static AG header area (SB/AGF/AGI/AGFL sectors) |
| -4 | `OWN_LOG` | internal log extent |
| -5 | `OWN_AG` | bnobt + cntbt + rmapbt blocks **including roots**, active AGFL entries |
| -6 | `OWN_INOBT` | inobt + finobt blocks, including roots |
| -7 | `OWN_INODES` | real inode-chunk backing blocks, including free inode slots |
| -8 | `OWN_REFC` | refcountbt (reflink, not writable in RW-6B) |
| -9 | `OWN_COW` | CoW staging (not writable in RW-6B) |

Special owners have offset=0, flags=0. Inode owners must pass geometry
validation (`xfs_verify_ino`), not just be nonzero. The first static header
record starts at 0 and spans `XFS_AGFL_BLOCK(mp)+1` filesystem blocks;
header sectors can share a filesystem block.

| allocation | record owner / offset / flags |
|---|---|
| regular file data | inode / logical block / unwritten when appropriate |
| directory data/block/leaf/node/free | directory inode / **data fork logical block** / written |
| remote symlink | symlink inode / data fork logical block / written |
| data fork bmbt block | inode / **0** / BMBT |
| attr fork bmbt block | inode / **0** / ATTR+BMBT |
| attr fork allocated extent | inode / attr logical block / ATTR |
| local data/dir/symlink/attr | no separate ownership (inside `OWN_INODES` backing) |

Directory block type is not a special rmap owner. Their mappings and remote
symlinks pass through bmap; the owner is the inode fork. Attr mutation is out
of scope, but preserved attr extents and bmbt blocks still belong in checks.
Hardlinks do not duplicate ownership: scan allocated inodes, not paths.

For sparse inode chunks, expand the **inverse holemask** into real backing
ranges (`xfs_ialloc_ag_alloc`, sparse free path). Holes get no rmap; adjacent
backing of different chunks can merge under `OWN_INODES`. Allocating/freeing
an inode within an existing chunk does not change that chunk's ownership.

## 4. AGFL state machine and recursion

```text
bnobt/cntbt free extent (no owner)
  -- refill: remove free range; map OWN_AG --> active AGFL
active AGFL
  -- get_freelist(btreeblk=1) --> bnobt / cntbt / rmapbt block
  <-- put_freelist(btreeblk=1) -- tree shrink
       (OWN_AG stays unchanged in both directions)
active AGFL
  -- shrink: dequeue; deferred extent free with OWN_AG --> ordinary free
       (remove OWN_AG when actually freed)
```

`xfs_rmapbt_alloc_block` only pops the AGFL and increments rmap_blocks;
`xfs_rmapbt_free_block` decrements it and pushes the block back. Neither
recursively calls rmap_map/unmap. This is the solution to self-accounting,
not a recursive call through a generic block allocator.

Before editing an rmapbt, Linux refreshes the freelist
(`xfs_rmap_finish_init_cursor`). `xfs_alloc_min_freelist` includes the bno,
cnt and rmap worst-case height-growth needs; rmap contributes
`2*min(rmap_level+1, max_rmap_levels)-2`. Refilling itself changes the free
trees and adds OWN_AG records, so reservation must exist **before** entry,
not only when a leaf split runs out of blocks. Busy extents prevent reuse
while prior frees remain unsafe (`xfs_extent_busy_reuse/insert`); this is
not an additional on-disk owner.

AGF fields (`xfs_format.h`, `db/agf.c`): rmap_root +24, rmap_level +36,
rmap_blocks +80, btreeblks +60. Height includes leaf level; root block level
is height-1. rmap_blocks counts **all reachable rmap blocks including root**.

```text
agf_btreeblks = (bnobt blocks-1) + (cntbt blocks-1) + (rmapbt blocks-1)
sb_fdblocks = sum(agf_freeblks + agf_flcount + agf_btreeblks)
```

inobt/finobt/bmbt blocks and inode backing are not in btreeblks. RW-2's
allocator currently recomputes btreeblks from bno/cnt only: that calculation
**must change before any rmap filesystem becomes writable**. Linux's in-core
rmap reservation accounting is separate from these on-disk identities.

## 5. Logical mutation and API correspondence

Separate a typed record editor from physical tree edits and from free-space
allocation. Do not make the allocator hand-edit raw rmap nodes.

| intended API | Linux reference |
|---|---|
| Lookup / LookupLE / LookupGE | `xfs_btree_lookup`, `xfs_rmap_lookup_eq/le` |
| Iterate / range query | `xfs_rmap_query_all/range` |
| predecessor / successor | `xfs_btree_decrement/increment` |
| exact Insert / Delete / Update | `xfs_rmap_insert/delete/update` |
| MapExtent | `xfs_rmap_map` |
| UnmapExtent | `xfs_rmap_unmap` |
| ConvertExtent | `xfs_rmap_convert` |
| AllocateMetadata / FreeMetadata | `xfs_rmap_alloc/free` |

Map merge requires physical adjacency, equal owner, **all three flags equal**,
and logical adjacency except for special-owner/bmbt records (their offset is
zero). Check XFS_RMAP_LEN_MAX (u32 max), not bmbt's 21-bit length limit.
Map can insert isolated, merge left, merge right, or delete right and enlarge
left. No overlap is legal in the RW-6B non-reflink profile.

Unmap requires a covering record with matching ownership/state/logical range.
Exact removes it; prefix advances physical start and, for inode fork data,
logical offset; suffix trims length; middle produces two residual records.
Sentinels are explicit exceptional operations, not records. EFI UNKNOWN
unmap accepts either a fully covering record or **no overlap at all**; partial
coverage must refuse (`xfs_rmap_unmap`), not silently skip.

Convert only applies to inode **data extents**, not attr/bmbt/metadata.
`xfs_rmap_convert` takes the **old** unwritten state and selects the opposite
state; `__xfs_rmap_finish_intent(CONVERT)` passes `!unwritten`, because the
intent describes the **new** state. Its filling/contiguity matrix has exact,
prefix, suffix and middle (three-record) cases, with eligible written-state
neighbours merged. Changing the bmap flag without converting rmap is corrupt.

`xfs_rmap_map_raw` is not unconditional exact insert: it selects shared map
for ordinary written data and nonshared map for other records. There is no
`xfs_rmap_unmap_raw` in this consulted kernel snapshot; do not invent one
based on an assumed upstream API.

Read-only overlap handling must retain the Linux overlapping-key layout even
with reflink RW refused. Sharing is only valid for ordinary written data
fork records on a reflink filesystem, never metadata, attr, bmbt or unwritten.
Duplicate ownership is not legitimate sharing.

## 6. Transaction plan and existing integration audit

Windows may log forward map, free trees/counters and rmap buffers in **one**
transaction rather than emulate Linux's deferred lock ordering. No metadata
write occurs during build. Commit is log+commit -> durable flush -> metadata
writeback -> flush. The writer keeps the kernel's fixed 32 KiB record size and
splits a larger transaction into multiple records with `CONTINUE`/`WAS_CONT`
item fragments and one `COMMIT`, exactly the kernel's rolling-transaction
mechanism. It does not change `h_size` in the middle of a Linux-created log.
The total transaction budget remains `XLOG_MAX_RECORD_BSIZE` (256 KiB); a
transaction that would exceed it is refused before writing, and an atomic
namespace change is never split across transactions. The typed reader and
both Python probes also accept genuine Linux records with
`xlog_rec_ext_header` blocks (up to 256 KiB). RUI completion adds the RUD before the
allocator's final budget check, so an oversized completion is refused rather
than half-written. The completion log size for genuine Linux RUIs with
different `rui_nextents` is still to be measured in CI (the writer logs the
serialized size of every completion transaction there); until that evidence
exists this section is an implementation statement, not an acceptance result.

Linux directly updates metadata allocation owners in allocator context via
`xfs_rmap_alloc/free`. File fork map/unmap/convert and selected metadata work
use deferred intents (`xfs_rmap_*_extent`, `xfs_defer_finish_one`). Deferred
work is sorted by AG, retaining age within an AG. File extent allocations can
skip the allocator's owner update because bmap schedules the rmap later;
there must be exactly one ownership update, never both.

Required Windows integration sites identified in the RW-0..6A code:

| file / path | ownership work still required |
|---|---|
| `src/write/alloc.cpp` | common per-AG rmap state, owner-aware allocation/free, AGFL refill/drain, include rmap nonroots in counters, reserve before structural edits |
| `src/write/bmap.cpp` `InsertExtent/RemoveRange` | map/unmap inode extents, not record-normalization edits; BMBT flags for `AllocBmbtBlock/FreeBmbtBlock` |
| `src/write/file_write.cpp` | unwritten allocation transaction and written conversion transaction both update rmap; in-place overwrite does not remap |
| `src/write/ialloc.cpp` | `OWN_INOBT` for both trees; `OWN_INODES` for actual full/sparse backing allocation/free |
| `src/write/dir_write.cpp` | inode-owned logical ranges across every directory format conversion/allocation/free |
| `src/write/namespace.cpp` | remote symlink creation, last-link truncate/free, replace lifecycle, shared transaction/editor state |
| `src/write/recovery_host.cpp` | RUI completion, EFI unknown-owner unmap, unlinked-inode paths using the same ledger |
| `src/recovery/log_scan.cpp`, `replay.cpp` | typed RUI/RUD preflight and ordered lifetime; RMB3 buffer verifier/LSN protection |

All editors in one transaction must see the same staged AGF/AGFL/rmap buffers.
An independent read from disk while another editor holds staged changes can
lose ownership updates. Structural split/merge changes must propagate both
low and high keys, maintain siblings, allocate from AGFL and update root,
height, rmap_blocks and btreeblks in that same transaction.

## 7. RUI/RUD format and lifetime

`XFS_LI_RUI=0x1240`, `XFS_LI_RUD=0x1241`. One host-order region each (the
existing scanner accepts Linux LE). RUI is 16 + n*32 bytes:

```text
+0 type u16, +2 size u16 (=1), +4 nextents u32, +8 id u64
extent: +0 owner u64, +8 startblock u64, +16 startoff u64,
        +24 len u32, +28 flags u32
```

startblock is an **encoded fsblock**, not a linear disk block. Validate one-AG
physical range, inode/special owner, file offset and count without overflow.
RUD is **16 bytes**, not an extent array: type u16, size u16, pad u32, RUI id
u64. The stale comment about `rud_extents` in the source is not the layout.

Operation low byte: 1 MAP, 2 MAP_SHARED, 3 UNMAP, 4 UNMAP_SHARED, 5 CONVERT,
6 CONVERT_SHARED, 7 ALLOC, 8 FREE. High flags: attr bit31, bmbt bit30,
unwritten bit29; bits 8..28 reserved. Current producer/recovery reconstructs
fork and state, but **does not set/reconstruct bmbt** in rmap intents; bmbt
allocation uses the direct allocator owner path. A parser must recognize the
defined bit, but cannot silently treat a bmbt intent as a data extent.
Realtime RUI/RUD have distinct types and remain unsupported.

Only committed transactions enter the recovered intent list. Process item
lifetimes in replay order, keyed by **intent type and id**, not a global set
of all done IDs (IDs are pointer-derived and can be reused). Matching RUD
releases an earlier active RUI; an uncommitted RUD releases nothing.

Linux `xlog_recover_release_intent` cancels earlier active matches if present;
an unmatched RUD is legal when its RUI is outside the replay tail. Genuine
kernel fixture `40084457645` establishes this boundary (the kernel recovers
it cleanly). The scanner therefore accepts a structurally valid unmatched
RUD, without cancelling later reused IDs. Malformed RUDs and unsupported
shared RUIs still refuse before any replay write.

Surviving RUI completion is `xfs_rmap_recover_work` -> validate every extent
-> reconstruct operations -> `xlog_recover_finish_intent` ->
`xfs_defer_finish_one` -> `xfs_rmap_finish_one` -> capture/commit.
**RUD belongs in the same committed transaction as the rmap buffers.** Never
write the rmap update durably and then commit RUD in another transaction.
Idempotence comes from write-ahead logging and pairing, not accepting a
double-map/double-unmap as normal. A restart before commit retries the intent;
after commit it replays buffers and sees RUD, so does not execute it twice.
The original tail stays pinned until every intent is resolved. Log wrap must
not overwrite a pinned recovery window (existing early-wrap path needs audit).

Preflight must inventory **all** operations before pass2, including already
paired shared operations. MAP_SHARED/UNMAP_SHARED/CONVERT_SHARED refuse
before the first metadata write, even on a falsely non-reflink superblock.
Likewise refuse unsupported semantics, unknown flags, malformed lengths or
unfinishable completion plans before starting replay. Never ignore RUI and
clean the log.

## 8. Buffer recovery and LSN

RMB3 uses ordinary `XFS_BLFT_BTREE_BUF`, short header LSN +24 / CRC +52.
`xlog_recover_get_buf_lsn` recognizes rmapbt with the other short btrees;
metadata UUID must match before its LSN is trusted. On-disk LSN >= replay
LSN skips the old item and verifies the existing buffer. Otherwise patch the
logged bitmap regions, verify identity/geometry/records, stamp recovery LSN
and recompute CRC at flush. The raw logged CRC is not an authority for the
post-replay block. The verifier must work before AGF initialization; whole
tree cross-checks run on the planned/recovered view, not a half-replayed AGF.

Existing RW-5 does **not** yet classify RMB3 for replay. Adding the magic to
the switch alone is insufficient: a complete preflight must validate the
staged replay view and completion feasibility before writes.

## 9. Implementation snapshot before external acceptance

Implemented on `rw-6b`; the ordinary writable gate was opened after the
qualification in section 12:

* `include/xfs/rmap.h` / `src/core/rmap.cpp`: typed `RmapKey`/`RmapRecord`/
  `RmapTree`; exact/LE/GE/predecessor/successor lookups; traversal with CRC,
  UUID, dblkno, header owner, level/numrecs, sibling, global record order,
  both child keys, cycle and AGF `rmap_blocks` checks; record bounds, inode
  ownership, flags/offset packing and static-owner checks. Cross-AG inode
  owners are accepted, as Linux does.
* `include/xfs/rmap_editor.h` / `src/core/rmap_editor.cpp`: independent writer
  abstraction. Logical `MapExtent`/`UnmapExtent`/`ConvertExtent` implement the
  merge, exact/prefix/suffix/middle and OWN_UNKNOWN rules; every record edit
  goes through `InsertExact`/`DeleteExact`/`UpdateExact`. The physical tree
  implements leaf/node split, merge and borrow, sibling maintenance, root
  growth, root collapse (Linux `xfs_btree_kill_root` semantics: the child
  becomes the AGF root and the old root block returns to the AGFL), overlapping
  low/high-key propagation, and publishes only after a full in-memory
  structural verification.
* `src/write/alloc.cpp`: owner-aware allocation/free/convert (`AllocateOwned`,
  `AllocateFromAgOwned`, `AllocateFromAgRangeOwned`, `FreeOwned`,
  `FreeUnknown`, `ConvertOwned`, `RmapMap/Unmap/Convert` for intent
  completion). AGFL refill records OWN_AG after the blocks are on the ring, so
  an rmap split cannot recurse into an empty freelist; drain removes OWN_AG
  only when a block actually returns to free space. `agf_btreeblks` now
  includes rmap non-root blocks and the AGF rmap root/level/blocks fields are
  maintained in the same transaction. `sb_fdblocks` uses the full
  freeblks+flcount+btreeblks identity.
* All audited ownership sites are wired: file data (written/unwritten/convert),
  bmbt blocks (data/attr flag model), inode chunks (`OWN_INODES`, sparse holes
  excluded), inobt/finobt (`OWN_INOBT`), directories, remote symlinks, AGFL
  and the free-space/rmap trees (`OWN_AG`), EFI `OWN_UNKNOWN` completion.
* `mkfs::FormatOptions::rmapbt` and `xfswin format --rmapbt` create a
  consistent rmapbt=1,reflink=0 image (single root leaf with canonical
  FS/LOG/AG/INOBT/INODES records). This exists so unit/CI fixtures do not need
  hand-built ownership; it is not the primary milestone.
* RUI/RUD: `include/xfs/rmap_intent.h` validates counts, sizes, encoded
  fsblocks, AG bounds, owners, offsets, flags and overflow. Shared
  MAP/UNMAP/CONVERT are refused before replay. RUI is an intent and RUD a done
  item in the ordered `(type,id)` lifetime table. Surviving RUI completion runs
  the rmap update and logs the RUD in **one** transaction. CONVERT uses the
  source-confirmed rule that the intent describes the *new* state and
  `xfs_rmap_convert` receives the old state.
* `RMB3` is classified for physical buffer replay; metadata LSN/UUID/CRC
  stamping uses the short-btree layout. The recovery-only mount path permits
  the rmapbt=1,reflink=0 profile for `xfswin recover` while ordinary
  read-write mounts remain refused.
* Pinned-tail wrap: a completion transaction that would wrap is refused when
  the pinned recovery window reaches block 0/reserved space, instead of
  overwriting unrecovered records.
* `xfswin check` uses the typed reader; `xfswin rmap <image> [--ag N]` prints
  verified records. `tests/unit/rw6b_tests.cpp` covers the merge/unmap/convert
  matrices, RUI/RUD preflight and lifetime, allocator ownership round trips,
  and a real in-memory tree that reaches level 3 (before=1, peak=3,
  after-delete=2) through split/grow/merge/collapse.
* `tests/tools/rw6b_rmap_probe.py` independently derives ownership from the
  forward structures and checks the AGF btreeblks identity. It now runs on
  Windows-formatted rmap images and is wired as a CI gate; a Linux-created
  fixture job is also present.

The early implementation snapshot above did not authorize writable rmapbt.
The subsequent Linux-created fixture, mutation, genuine surviving-RUI,
crash, ENOSPC and repeated interoperability evidence is recorded below.
The RW-6B gate was opened with `rmapbt=1,reflink=0`; run `40107607956`
passed using ordinary opt-in, not the former qualification switch. RW-6C
subsequently added `rmapbt=1,reflink=1` to the same ordinary gate.

## 10. Independent ownership proof and acceptance gates

Expected ownership must be derived from SB log placement, AG headers,
reachable bnobt/cntbt/inobt/finobt/rmapbt blocks, active AGFL ring,
inobt real backing masks and **allocated inode** data/attr forks and bmbt
blocks. Do not infer any expected owner from rmap records. Normalize adjacent
observations using the owner/flags/logical rules, then compare coverage and
records. Also reject expected duplicates, free-space conflicts, missing and
unexplained ownership. Do not rebuild rmapbt to mask writer bugs.

Acceptance evidence (unchecked rows are not claimed as completed):

- [x] typed reader, key/record validation, lookup directions, CLI dump
- [x] independent Python forward-derived ownership oracle on a real
      Linux-created, multi-level rmapbt fixture
- [x] Windows-formatted rmapbt=1,reflink=0 image accepted by Linux
      `xfs_repair -n`, mount and a writable Linux pass
- [x] RmapEditor logical and physical writer (unit matrix, real height 3)
- [x] owner-aware writer at every audited allocation/free/conversion site
- [x] RUI/RUD parser, shared-operation refusal, ordered lifetime, atomic RUD
- [x] RMB3 buffer replay classification and recovery-only rmap profile
- [x] pinned-tail wrap refusal when the recovery window reaches block 0
- [x] real leaf/internal split, merge, borrow, root growth/collapse on a
       Linux-created fixture (`windows-rw6b-tree` / `linux-verify-rw6b-tree`,
       run `40107607956`, grown and collapsed images verified on Linux)
- [x] Linux-created `rmapbt=1,reflink=0` + modern inode profile round trip
- [x] Linux -> Windows -> Linux -> Windows mutations and byte/namespace checks
- [x] genuine Linux dirty logs with committed surviving RUI (not synthetic-only)
- [x] writer crash matrix, both Linux and Windows replay oracles
      (330 cases, ordinary-gate Windows and Linux replay in `40107607956`)
- [x] recovery crash/restart matrix, no double-map/unmap
- [x] shared-intent refusal with full-image before/after hashes
      (three shared operations injected into a genuine kernel log; full-image
      hash equality passes locally and in `40107607956`)
- [x] malformed tree/record/ownership matrix on real fixtures, no writes
      (seven CRC-correct/CRC fault cases on a Linux-created image; full-image
      hash equality passes locally and in `40107607956`)
- [x] Linux mount + `xfs_repair -n` + xfs_db/logprint evidence and all regressions
      (ordinary-gate run `40107607956`, all 65 jobs successful)

Kernel-log sampling limit: the genuine samples establish UNMAP (runs
`40087979448`, `40091917917`) and CONVERT (`40107607956`),
not every possible RUI producer. The MAP/UNMAP/CONVERT/ALLOC/FREE parser/editor
matrix is unit coverage, **not** five-family kernel-log acceptance. Additional
genuine MAP samples and online-repair ALLOC/FREE samples remain unclaimed.
In the consulted kernel, ALLOC/FREE intent producers are online repair and CoW;
CoW/refcount owners remain refused, and online repair is not an advertised
ordinary writer operation. This qualification does not claim online repair or
reflink/CoW interoperability.

### Initial external evidence

The `linux-generate-rw6b-rmap` CI job created a real 2 GiB
`rmapbt=1,reflink=0` filesystem with the modern inode profile and populated it
with written/unwritten/fragmented/sparse files, bmbt-format inodes,
short/block/leaf/node directories, hard links, inline/remote symlinks, inode
churn and sparse chunks. `xfs_repair -n` accepted it. Its rmapbt reached height
2 (AG0 11 blocks, AG1 1, AG2 32, AG3 39; AG0 `agf_btreeblks = 10`). The
independent forward-derived oracle then matched every record:

```
AG 0: 1351/1351, AG 1: 87/87, AG 2: 5163/5163, AG 3: 4186/4186
missing 0, unexpected 0, overlap 0, owner errors 0
```

Windows subsequently read the same image:
`xfswin check` traversed every AG's typed tree cleanly, `xfswin rmap --ag=0`
dumped the records, `rw6b_rmap_probe.py` again reported exact counts, and the
reader differential matched sample file hashes and namespace entries.

The Windows-formatted `rmapbt=1,reflink=0` image also passed the independent
oracle, then `xfs_repair -n`, a read-only mount, and a writable Linux pass
(including `xfs_repair -n` on the resulting image).

This initial result proved the reader/oracle and formatter writes only.
Windows mutation, genuine surviving RUI, crash and repeated interoperability
were subsequently accepted in the runs recorded in section 11.

Only after these gates may `CheckWritableProfile` allow rmapbt. Reflink,
refcountbt, CoW, realtime, external log, quota/xattr/parent mutation, metadir
and zoned XFS remain outside this milestone. Formatter support is lower
priority than Linux-created correctness and is not a prerequisite.

## 11. Acceptance follow-up (2026-10-24)

Baseline acceptance completed in `40091917917` (63/63 success), before the
gate-opening commit. The ordinary-gate revision repeats the full suite and
adds explicit real-image structural/refusal assertions. No ownership oracle
or repair threshold was relaxed, and no rmap tree was rebuilt.

| evidence | result / reference |
|---|---|
| Linux-created fixture, Windows mutation, Linux repair/mount/oracle | passed in runs `40079819711` and `40081605233`; ordinary-gate Windows mutation rerun also passes locally |
| Writer crash matrix | 330 Windows cases plus Linux replay/repair/oracle passed in `40079819711` and `40084457645`; all 330 ordinary-gate local Windows cases also pass, archives and `cases.json` retained |
| Genuine surviving RUI | `40081605233`, artifact `rw6b-linux-rui`; kernel-recovered copy passes the oracle |
| Original RUI, Windows recovery after `f82c969` | local `check`, clean journal and forward oracle pass; 5 intents completed |
| Original RUI, recovery crash/restart after `f82c969` | local 11 injected crashes, 0 failures; separate genuine CI samples subsequently pass both Linux verification jobs |
| New RUI from `40084457645` | unmatched RUD exposed the legal replay-tail boundary described in section 7; corrected local recovery completes 15 intents and passes check/oracle; separately captured genuine UNMAP samples pass both Linux verification jobs in `40087979448` and `40091917917` |
| CI-captured RUI from `40087979448` | Windows recovery and all 11 recovery restarts pass; both Linux RUI verification jobs pass |
| ENOSPC | ordinary-gate local Windows pass: 255 x 1 MiB, next allocation refused empty, 238 disk free blocks; full Linux continuation passes in `40091917917` |
| Reverse interoperability | ordinary-gate Windows local rerun passes; complete Windows reverse and Linux final continuation pass in `40087979448` |

### AGF longest: completion, not BUF replay

The original failure was `AG0 bnobt longest 4015 != AGF longest 4016`.
Stopping at `recovery_after_metadata_flush` isolates physical log replay from
deferred intent completion. On the original RUI image from `40081605233`:

| AG0 field | after BUF/INODE replay | Linux recovery | Windows completion, fixed |
|---|---:|---:|---:|
| freeblks | 8035 | 8035 | 8034 |
| btreeblks | 103 | 103 | 104 |
| longest | 4016 | 4016 | 4015 |
| largest bnobt extent | 45136 + 4016 | 45136 + 4016 | 45137 + 4015 |
| flcount | 8 | 8 | 8 |

Thus the replayed free-space state already matches the kernel. Completing a
middle-extent CONVERT can insert two additional records into a full rmap leaf.
The Windows editor splits where the Linux btree can redistribute to a sibling;
the physical layouts need not be identical. Windows consumes active AGFL block
16319 for that split, then reserves block 45136 to replenish the ring. Block
45136 is an **active AGFL entry**, not a leaked or missed free block. The
forward oracle accounts independently for the new tree and the active ring.
AG1 and AG3 likewise consume an AGFL block for a split.

`AgState::RefillAgflRaw` updated the free records and freeblks but left its
cached longest unchanged. RUI completion can refill without a later file-data
Allocate/Free call to recompute that maximum. `f82c969` refreshes longest in
the refill path itself. Its regression test fails before the fix and passes
after it, both with and without rmapbt; it derives the maximum from bnobt and
also checks conservation of freeblks+flcount+btreeblks. Replay LSN and buffer
cancellation rules were not changed. Linux repair/mount acceptance remains
required for the resulting, legitimately different tree layout.

### Other acceptance blockers uncovered

* `xfs_bmap` reports both offsets and lengths in **512-byte basic blocks**
  regardless of filesystem block size (`xfs_bmap(8)`). The literal-dot regex
  correction alone did not fix the 8x discrepancy on 4 KiB images. The ENOSPC
  verifier now compares allocated coverage in bytes, includes unwritten
  preallocation with `-p`, and excludes holes/delalloc. The expected allocation
  sizes and failed-allocation emptiness checks are unchanged.
* Linux can punch out an empty middle data block while retaining a leaf1
  directory. Its bests slot is `NULLDATAOFF` (`0xffff`). Enumeration now skips
  only a slot whose forward mapping is actually a hole, and rejects live leaf
  pointers to such a slot or a mapped block hidden by a null slot. The unit
  regression covers both the valid sparse layout and corrupt references. The
  original Linux `/linux-after/grow` fixture has db0 and db2, with db1 absent;
  Windows now lists its 100 remaining entries instead of reporting corruption.
* Disk `sb_fdblocks` and Linux read-only `statfs.f_bfree` are not the same
  quantity. `xfs_alloc_set_aside` withholds `8 * agcount` blocks;
  `xfs_freecounter_unavailable` additionally withholds non-root bnobt/cntbt
  blocks (`agf_btreeblks - (agf_rmap_blocks - 1)`). The ENOSPC verifier now
  requires exact equality of the manifest, disk SB counter and summed AGF
  contributions, then independently requires exact mounted accounting with
  these deductions. In `40084457645`, `238 - 8*4 - 0 = 206`. No tolerance or
  ignored accounting discrepancy is introduced.
* The final expectation generator omitted the `n` prefix from the 100 moved
  `/win2/grow/n0400.bin` through `n0499.bin` paths. The fixture creator and
  Windows state checks already agree on these names. Correcting the generated
  paths preserves every size/hash/nlink check; all 100 corrected expectations
  were matched individually against the original reverse image locally.

### Full run `40087979448`

62 of 63 jobs passed. In particular both Windows RUI jobs and both Linux RUI
verification jobs pass, including all 11 restarted images. Mutation, 330-case
writer crash (both replay implementations), reverse/final, WinFsp mount, raw
device and RW-0..RW-6A regressions also pass. This is not yet permission to
open the ordinary writable gate: the remaining job is Linux ENOSPC continuation.

That job now passes repair, ownership oracle, exact allocated coverage,
SB/AGF accounting, read-only mount and exact statfs accounting. Its writable
copy freed 32 MiB, but the following 16 MiB `dd` still hit ENOSPC. Linux
`xfs_default_resblks` reserves up to 8192 filesystem blocks (32 MiB at 4 KiB),
in addition to per-AG reservations; freeing a fixed 32 MiB does not establish
that 16 MiB is available to file data. The continuation now frees already
verified 1 MiB allocation files in batches until kernel statfs reports 32 MiB
available, logs each batch, and still requires the full 16 MiB write, namespace
operations, final repair and oracle. It neither disables kernel reserves nor
reduces the write size. Failure to make enough space after releasing every
successful allocation remains a failure. Linux confirmation subsequently
passed in `40091917917` and in the final ordinary-gate run (section 12).

### Baseline completed: `40091917917`

All **63 jobs succeeded**. The Linux ENOSPC continuation freed 32 MiB and
reported only 75 allocatable blocks; after freeing 64 MiB it reported 8267
blocks, wrote all 16 MiB, and passed final repair and the forward oracle.
Both genuine-RUI recovery jobs and both Linux RUI verification jobs passed,
as did mutation/reverse/final, 330-case writer crash, RW-0..RW-6A, WinFsp and
raw-device verification. This completed before publishing the ordinary gate.

The gate revision additionally requires:

* seven malformed metadata/ownership refusals on the real Linux fixture:
  bad CRC, zero length, invalid owner, out-of-AG extent, parent high key,
  sibling cycle, and a valid-but-wrong owner for a real file. A positive
  ordinary allocation control prevents mistaking profile refusal for metadata
  refusal. Every negative operation has equal full-image before/after SHA256;
* MAP_SHARED/UNMAP_SHARED/CONVERT_SHARED refusal after injecting only the
  operation into an existing committed kernel RUI and preserving record CRCs
  and cycle stamps. These are rejection tests, not synthetic-RUI acceptance;
* a separate Linux-created 512 MiB/4 KiB fixture for actual internal rmap
  splits and collapse, through ordinary file allocations and unlink. It does
  not replace the populated 2 GiB/4 KiB fixture. Both the grown and collapsed
  images go back to Linux for repair/mount/oracle. Tree edit counters are
  diagnostic only; the forward oracle remains independent.

The local structural harness (Windows-formatted, **not** Linux acceptance)
initially used 1 KiB blocks and measured height `1 -> 3 -> 2`, 64 leaf splits, 4 internal splits, 62 leaf
merges, 4 internal merges, 474 leaf borrows, 16 internal borrows, 2 root grows
and 1 collapse. Returning to height 1 is not required: scattered blocks
returned to the active AGFL still carry OWN_AG and can legitimately need two
levels. The real-image CI assertions require every operation class and a
strict height decrease, not an assumed identical physical layout.

Run `40096839943` rejected the first companion fixture at mkfs's minimum
directory size check (`-n size=1024`); no Windows/Linux structural acceptance
is claimed from that run. The companion now uses Linux-supported 4 KiB
blocks/directories and 5000 alternating allocation pairs (10,000 data rmap
records), retaining internal split/merge/borrow and root transition assertions.

### Embedded bmbt root traversal

The larger 4 KiB structural pass exposed a reader boundary near 2646 extents:
after bmdr grows above its leaf children, `LoadDataForkExtents` called
`WalkBtree` separately for each embedded-root child. Sibling state reset for
each call, so the second internal child's legitimate left link was reported
as corruption. The unchanged failing image passed the independent ownership
oracle; its writer-produced bmbt links were valid.

`WalkBtreeForest` now shares cycle/duplicate detection and per-level sibling
state across all embedded-root children, in both extent loading and inode
block accounting. It checks both sibling directions, leaf links and right-edge
termination, and validates every child's level and root low key. No verifier
was disabled. The regression accepts the connected multi-child bmbt and
rejects broken forward/backward links, a nonterminating edge and repeated
root pointers. The original failing image then passes `check` without any
image modification; the full structural and Linux rerun subsequently passed
in section 12.

### Large shrink and a full AGFL

The 10,000-record pass subsequently completed growth and removal of the
alternating-owner B file, but the final A unlink returned `AGFL is full during
an rmap edit`. Shrink can return more tree blocks than the AGFL ring holds;
recursively draining OWN_AG inside a running rmap edit is not safe.

Overflow tree returns now stay transaction-local with unchanged OWN_AG
ownership. Before Commit publishes any rmap/header buffer or computes the
counter delta, it drains active AGFL entries to ordinary free space and puts
the overflow blocks onto the ring, iterating through any further rmap merges
until there are no deferred blocks. Both free trees and rmap are edited
incrementally. The drained entry is popped **before** its ownership unmap so
an unmap-induced split cannot allocate the very block being drained.

The regression grows more rmap blocks than the ring capacity, frees 3000
nonmergeable allocations, and checks exact fdblocks conservation and exact
OWN_AG coverage derived from reachable bno/cnt/rmap tree blocks plus active
AGFL slots. The original failed unlink image also recovers its one unlinked
inode, then passes check and the independent forward oracle with this fix.
The fresh structural pass and full Linux/crash CI subsequently confirmed it
in section 12.

## 12. Final ordinary-gate qualification (2026-10-25)

Qualified code: **`5a41fca11ce53fa2bea618584a84eef93b17df52`**.
[Full run `40107607956`](https://github.com/100pangci/XFS-for-Windows/actions/runs/40107607956):
**65/65 jobs successful**, no failed or skipped jobs.

Mutation, ENOSPC, reverse and 330-case writer crash use **only ordinary
`--rw-experimental`**, not `--rw-rmapbt-qualification`. Genuine surviving-RUI
completion and all 11 recovery crash/restart cases pass both Windows and Linux
verification. WinFsp mount, raw-device verification, RW-0..RW-6A regressions,
Linux reverse/final, negative metadata/ownership and shared-RUI refusals all pass.

The Linux-created 512 MiB/4 KiB structural fixture reached height
**`1 -> 3 -> 1`** through real ordinary file APIs:

| operation | actual count |
|---|---:|
| leaf split / merge | 120 / 120 |
| internal split / merge | 1 / 1 |
| leaf / internal borrow | 2918 / 30 |
| root grow / collapse | 2 / 2 |

Both grown and collapsed images pass Linux `xfs_repair -n`, read-only mount,
a writable Linux pass and the independent forward oracle. AG0 ownership
matches exactly: 10203 records in the grown image, 113 in the collapsed image,
with zero missing/unexpected/overlapping/owner errors. The active AGFL is
included in expected ownership; retained OWN_AG blocks are not treated as leaks.

Artifacts in that run (IDs retained for audit):

| artifact | ID | evidence |
|---|---:|---|
| `rw6b-linux-rmap` | 12565232761 | populated Linux fixture, manifest, layout/repair, companion fixture |
| `rw6b-linux-rui` | 12565696180 | genuine `rui.img`, kernel-recovered image, manifest, logprint, repair |
| `rw6b-windows-recovered-rui` | 12565149809 | Windows recovery and three shared-operation no-write hash reports |
| `rw6b-rui-crash` | 12565985392 | all 11 restarted RUI images |
| `rw6b-crash` | 12565734122 | all 330 writer crash archives and `cases.json` |
| `rw6b-enospc-image` | 12566315223 | image plus `enospc.json`: 255 successes, refused empty target, 238 disk free blocks |
| `rw6b-tree` | 12565489559 | grown/collapsed images and `tree.json` operation counts |
| `rw6b-metadata-refusals` | 12565722635 | seven refused metadata/ownership faults, equal before/after full-image hashes |
| `rw6b-windows-mutated` | 12565738603 | Windows mutation image and namespace/content expectations |
| `rw6b-linux-mutated` | 12565946484 | Linux mutation of the Windows image |
| `rw6b-windows-round2` | 12565878338 | ordinary-gate reverse image and round2 expectations |
| `rw6b-final-evidence` | 12566341325 | final Linux repair, layout and logprint evidence |

These results authorize the ordinary **experimental image-file** writable
profile only for `rmapbt=1,reflink=0`. They do not authorize reflink/refcount,
CoW, xattr mutation, raw-device writable mount, online repair, or claim genuine
kernel samples for every RUI operation family (section 10).

### Documentation-only rerun and host unmount contention

Run `40112173153` (documentation revision `5443130`, identical qualified C++)
passed 64/65 jobs. The remaining Linux writer-crash job stopped at a normal
`umount` with `target is busy` (exit 32), before that image's offline repair;
it did not report a repair/oracle failure. This is recorded rather than
counted as a passing 330-image job.

The crash verifier now logs the busy error and available `fuser` holder
diagnostics, and retries **only busy unmounts**, at most three times. Every
attempt is a normal synchronous unmount; persistent busy or other errors
remain fatal. No successful-case lazy/forced unmount, missing-image skip,
repair/oracle relaxation, or reduced case count is introduced. The exact
330/330 post-unmount repair/clean-log/ownership gates remain mandatory.
