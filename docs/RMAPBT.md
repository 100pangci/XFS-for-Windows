# Reverse mapping ledger (RW-6B)

Status: **in progress, not writable support**. The rmapbt/reflink writable
gates remain closed. This document separates source-verified format/semantics
from implementation and interoperability evidence. A reader or a structurally
valid tree is not proof that the writer maintains ownership.

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

Important source distinction: Linux `xlog_recover_release_intent` does not
report an unmatched RUD as corruption; it cancels matches if present. The
RW-6B requested policy is stricter (orphan RUD refuses). Keep that policy
explicit; do not describe it as Linux's exact rule. Real Linux fixtures must
establish whether this conservative refusal excludes otherwise legal logs.

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

## 9. Current implementation status (not RW-6B completion)

Implemented on `rw-6b`, with the ordinary writable gate still closed:

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

Still required before the gate may open: genuine Linux-created rmap fixtures
with Linux-originated mutations, surviving-RUI capture and completion evidence,
the writer and recovery crash matrices, ENOSPC/log-reservation stress,
reverse interoperability across repeated Linux/Windows passes, and real
`xfs_repair -n`/`xfs_db`/`xfs_logprint` acceptance. The current unit tree is
real but synthetic; it is not a substitute for those gates.
`CheckWritableProfile` still refuses rmapbt for ordinary read-write mounts.
This note must not be read as a claim that RW-6B is complete.

## 10. Independent ownership proof and acceptance gates

Expected ownership must be derived from SB log placement, AG headers,
reachable bnobt/cntbt/inobt/finobt/rmapbt blocks, active AGFL ring,
inobt real backing masks and **allocated inode** data/attr forks and bmbt
blocks. Do not infer any expected owner from rmap records. Normalize adjacent
observations using the owner/flags/logical rules, then compare coverage and
records. Also reject expected duplicates, free-space conflicts, missing and
unexplained ownership. Do not rebuild rmapbt to mask writer bugs.

Pending acceptance evidence (none implied by this research note):

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
- [ ] real leaf/internal split, merge, borrow, root growth/collapse on a
      Linux-created fixture (the unit tree is not acceptance evidence)
- [ ] Linux-created `rmapbt=1,reflink=0` + modern inode profile round trip
- [ ] Linux -> Windows -> Linux -> Windows mutations and byte/namespace checks
- [ ] genuine Linux dirty logs with committed surviving RUI (not synthetic-only)
- [ ] MAP/UNMAP/CONVERT/ALLOC/FREE completion against the Linux fixture
- [ ] writer crash matrix, both Linux and Windows replay oracles
- [ ] recovery crash/restart matrix, no double-map/unmap
- [ ] shared-intent refusal with full-image before/after hashes
- [ ] malformed tree/record/ownership matrix on real fixtures, no writes
- [ ] Linux mount + `xfs_repair -n` + xfs_db/logprint evidence and all regressions

### Current external evidence (not yet a completed gate)

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

This proves the reader/oracle and formatter writes against a Linux-created
multi-level overlapping tree. It does **not** yet prove Windows mutation of a
Linux fixture, writer crash recovery, genuine surviving RUI recovery, or
repeated reverse interoperability; those remain unchecked below.

Only after these gates may `CheckWritableProfile` allow rmapbt. Reflink,
refcountbt, CoW, realtime, external log, quota/xattr/parent mutation, metadir
and zoned XFS remain outside this milestone. Formatter support is lower
priority than Linux-created correctness and is not a prerequisite.
