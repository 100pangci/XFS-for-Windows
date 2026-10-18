# Journal recovery (RW-5)

Status: **implemented** for the writable profile (XFS v5, CRC enabled, internal
log, no finobt/rmapbt/reflink/sparse inodes/bigtime/inobtcount/quota/metadir).
This note documents what the Windows recovery engine does, what it refuses, and
how it is verified. It is the recovery counterpart of `JOURNAL.md` (the writer).

```
Windows crash / Linux crash
        |
        v
  dirty XFS internal log
        |
        v
  xfswin recover  (or a read-write mount)
        |
        +-> scan + validate the whole log          (read-only)
        +-> reconstruct every committed transaction
        +-> inventory every item type; refuse if anything is unsupported
        +-> pass1: buffer cancellation table
        +-> pass2: metadata replay (direct buffer writes, LSN rules)
        +-> flush
        +-> finish deferred intents (EFI -> free extents + EFD)
        +-> process AGI unlinked lists (truncate + free zero-link inodes)
        +-> flush
        +-> clean-unmount record (standalone `recover`) / writable log (mount)
```

The hard rule: **nothing is written before the whole log has been parsed,
validated, reconstructed and proven replayable by this build.** A log with an
unsupported item, a corrupt transaction, a broken continuation chain or an
invalid geometry is refused with the record LSN, transaction id and item type;
the image is left untouched.

## 1. Layering

| file | role | kernel counterpart |
|---|---|---|
| `include/xfs/recovery.h` | public scan/replay API, item classification | `xfs_log_recover.h` |
| `src/recovery/log_scan.cpp` | head/tail discovery, record validation, cycle restoration, op stream, transaction reconstruction, item inventory | `xlog_find_head`, `xlog_find_tail`, `xlog_find_zeroed`, `xlog_verify_head/tail`, `xlog_do_recovery_pass`, `xlog_recover_process_data` |
| `src/recovery/replay.cpp` | pass1/pass2, cancellation, buffer/inode/icreate replay, LSN/CRC stamping, stale-block clearing | `xlog_recover_buf_commit_pass1/2`, `xlog_recover_inode_commit_pass2`, `xlog_recover_icreate_commit_pass2`, `xlog_clear_stale_blocks` |
| `src/write/recovery_host.cpp` | filesystem orchestration: re-read superblock, adopt log state, complete intents, AGI unlinked lists | `xlog_recover`, `xlog_recover_finish`, `xlog_recover_process_iunlinks` |
| `src/core/log_checker.cpp` | still only classifies the newest record for writer positioning | `xlog_check_unmount_rec` |

`CheckLogClean`/`ReadLogPosition` are **not** the recovery parser. They remain
the small "is the newest record an unmount record" classifier used when the
writer needs a position; the mount path and `xfswin recover` use the recovery
engine.

## 2. Head/tail discovery and record validation

Ported from the kernel, including the cases the old scanner did not implement:

* `xlog_find_zeroed` + `xlog_find_cycle_start` (binary search, then a
  verification scan) locate the first zeroed block of a partially written log;
* `xlog_find_head` finds the write head: same-cycle case (`last_half_cycle - 1`
  holes), differing-cycle case (first occurrence of `last_half_cycle`), the
  two-part scan around the physical end, and `xlog_find_verify_log_record` to
  step back over a partial record. The scan distances use the kernel's
  `XLOG_REC_SHIFT` (512 basic blocks) and `XLOG_TOTAL_REC_SHIFT` (4096);
* the cycle comparison is the kernel's 32-bit wrap arithmetic, not a naive
  `max(cycle, block)`;
* `xlog_find_tail` reads the newest record, takes `h_tail_lsn` as the replay
  tail, and detects a clean unmount (`h_num_logops == 1`, op flag
  `XLOG_UNMOUNT_TRANS`, head immediately after the record);
* `xlog_verify_head` CRC-verifies up to `XLOG_MAX_ICLOGS` records at the head
  and **trims a torn tail** (bad CRC within the maximum in-flight window) to
  the last good record; `xlog_verify_tail` pushes the tail forward when a CRC
  failure is within that window, exactly like the kernel. Corruption outside
  that window is not treated as a torn write and refuses;
* a block with `h_cycle == 0` (kernel-cleared stale space) is not a record;
* records are validated for magic, version (logv2), `h_len` (multiple of 512,
  within `h_size`), `h_lsn`/`h_tail_lsn`, `h_prev_block`, `h_num_logops`,
  `h_fmt` (`XLOG_FMT_LINUX_LE`), UUID, and the CRC. The CRC covers the header
  (328 bytes, CRC field zeroed) or the historical 324-byte coverage plus the
  record data **as written** (cycle stamps in place), matching `xlog_cksum`.
  A zero stored CRC is accepted (mkfs history);
* records that wrap the physical end (header and/or data) are read in pieces,
  like `xlog_do_recovery_pass`; extension header blocks are used for records
  larger than 32 KiB, and cycle data beyond the first 64 blocks is read from
  `xlog_rec_ext_header` blocks.

`xlog_unpack_data` restores the first word of every data block from the cycle
data array before operations are parsed.

## 3. Operation stream and transactions

The op state machine is the kernel's:

* `XLOG_START_TRANS` opens a transaction keyed by `oh_tid`; the transaction LSN
  is the LSN of the record that carried the START op (that is the
  `current_lsn` all LSN comparisons use);
* `XLOG_COMMIT_TRANS` closes it; **only committed transactions are replayed**
  ("no commit = the transaction never happened");
* `XLOG_CONTINUE_TRANS` / `XLOG_WAS_CONT_TRANS` reassemble an op split across
  records; the transaction header itself may be split (`add_to_cont_trans`
  copies the remainder at `sizeof(header) - len`);
* `XLOG_UNMOUNT_TRANS` closes the record without replaying anything.

A transaction is kept as a **list** of items, each with its format header and
payload regions (`ri_buf[]` in the kernel). Each item's region count comes from
the first 32 bits of its format (`ilf_size`/`blf_size`, maximum 257).
Transactions are reordered like `xlog_recover_reorder_trans`: `BUF` items
(except inode buffers and cancellations), then `ICREATE`, then all other items,
then inode buffers, then cancelled buffers.

## 4. Replay

### 4.1 Buffer cancellation (pass 1)

Every `XFS_LI_BUF` with `XFS_BLF_CANCEL` is collected in a table keyed by
`(blkno, len)` with a refcount (`xlog_add_buffer_cancelled`). In pass 2 a
buffer item matching a cancel entry is skipped; a cancel item consumes one
reference, so a block reused after its last cancellation is replayed again
(`xlog_put_buffer_cancelled`). Cancellation matches the full `(blkno, len)`
pair, never an offset guess.

### 4.2 Buffer replay (pass 2)

`XFS_LI_BUF` items carry a `blf_data_map` bitmap of 128-byte dirty chunks; only
those chunks are in the log, one region per contiguous run. Replay copies
exactly the logged bytes (the bitmap, not the whole buffer; unlogged areas keep
their on-disk contents), verifies the post-patch metadata type, and the buffer
is written back with the recovery LSN stamped and the CRC recomputed.

Inode buffers (`XFS_BLF_INODE_BUF`) replay only `di_next_unlinked` from the
logged regions (never the whole inode) and recompute the per-inode CRC, like
`xlog_recover_do_inode_buffer`.

The metadata LSN rule is `xlog_recover_get_buf_lsn`: the on-disk LSN is
extracted from the buffer's current magic (superblock, AGF/AGI/AGFL, short and
long btree blocks, directory blocks, symlink blocks), including the UUID
check. If `lsn >= current_lsn` the item is skipped (and the buffer verified);
if the magic is unknown or belongs to another filesystem instance (`-1`) the
item is replayed. Buffers are held in a small cache and drained when the
recovery LSN changes, exactly like the kernel's per-commit
`xfs_buf_delwri_submit`: items of one checkpoint see each other's changes while
the on-disk LSN rule stays meaningful. LSN and CRC are stamped at flush time
(`xfs_agf_write_verify` and friends), not at patch time.

### 4.3 Inode items

`XFS_LI_INODE` describes the inode **cluster buffer** plus the inode byte offset
(`ilf_blkno`/`ilf_len`/`ilf_boffset`), not a single inode block. The logged
`xfs_log_dinode` is converted field by field into the on-disk core
(`xfs_log_dinode_to_disk`), which stamps `di_lsn = current_lsn`, then the
logged fork regions are applied according to `ilf_fields`:

* `XFS_ILOG_CORE` only touches the core;
* `XFS_ILOG_DDATA/DEXT` copy the data-fork region (local data / extent list);
* `XFS_ILOG_DBROOT` converts the logged btree root (`xfs_bmbt_to_bmdr`);
* `XFS_ILOG_ADATA/AEXT/ABROOT` do the same for the attribute fork.

Unlogged bytes keep their on-disk values. The inode CRC is recomputed over the
whole inode and the result is verified (`xfs_dinode_verify` equivalent). The
LSN rule is strictly greater (`di_lsn > current_lsn` skips), so checkpoints
sharing a start LSN are all applied.

### 4.4 ICREATE

`XFS_LI_ICREATE` rebuilds whole inode clusters from the logged geometry
(`xfs_ialloc_inode_init`): each inode gets `di_magic`, version 3, the logged
generation, `di_next_unlinked = NULLAGINO`, its `di_ino`, the filesystem UUID
and a fresh CRC; `di_lsn` stays zero like the kernel's fresh inodes. ICREATE is
ordered with the buffer items, before inode items, so an inode allocated in the
same transaction is replayed into the initialized cluster.

### 4.5 The end of pass 2

Partial writes in front of the head are overwritten with empty records of the
previous cycle (`xlog_clear_stale_blocks`), step by step and capped at
`XLOG_TOTAL_REC_SHIFT` blocks, so a later crash cannot mistake them for data.

## 5. Completion phases (normal transactions)

These run after the metadata replay has been flushed, through the normal
transaction writer, exactly because the kernel also finishes them that way.

### 5.1 Intents

`XFS_LI_EFI` (extent free intent) is validated (every extent must be in range)
and completed in one transaction: the extents are freed through the block
allocator, and an `XFS_LI_EFD` done item with the same id is logged. A crash
before the completion leaves the intent pending; replaying again finds the EFI
and the (later) EFD and does **not** free twice. `XFS_LI_BUI/BUD` (bmap update
intents) have no completion pass in this build: a log that still *needs* a BUI
completed is refused during the scan, before any metadata write.

While intents are unresolved, every recovery transaction keeps `h_tail_lsn`
pinned to the start of the replayed window, so a crash during completion
re-replays the whole window (idempotent because of the EFI/EFD pairing), like
the kernel's AIL pinning.

### 5.2 AGI unlinked lists

After replay (and on every read-write mount, even of a clean log - a crash
after the unlink transaction was flushed leaves a legal, clean log with
zero-link inodes on the lists), every AGI bucket is walked:

* a zero-link, allocated inode is truncated (`xfs_inactive`) and freed
  (`xfs_ifree`), which also removes it from the list;
* a bucket that cannot be walked (unreadable inode, inode with links) is
  cleared in a transaction (`xlog_recover_clear_agi_bucket`) and only a repair
  scan can recover what remains;
* a zero-link inode is a legal crash state, never reported as corruption.

## 6. Crash safety (recovery is restartable)

* The log stays authoritative until replay has completely finished.
* Replay writes are idempotent: the metadata LSN rule, the transaction commit
  state, the intent/done pairing and the AGI unlinked lists all converge.
* The tail stays pinned while intents are pending; the clean-unmount record is
  only written after every completion phase has been flushed.
* Any failure leaves the log dirty; nothing pretends to be clean.
* `tests/unit/recovery_tests.cpp` cuts the device at every recovery write and
  requires the restarted recovery to converge to the crash-free image; CI adds
  process-level crash points (`recovery_*`) verified on Linux.

## 7. Item whitelist

| item | replay | notes |
|---|---|---|
| `XFS_LI_BUF` | yes | bitmap chunks, cancellation, inode-buffer special case |
| `XFS_LI_INODE` | yes | core + data/attr fork regions, fields mask |
| `XFS_LI_ICREATE` | yes | cluster rebuild |
| `XFS_LI_EFI` / `XFS_LI_EFD` | yes | deferred extent free completion + done item |
| `XFS_LI_BUI` / `XFS_LI_BUD` | replay only | a *surviving* BUI refuses (no bmap completion yet) |
| `XFS_LI_RUI/RUD`, `CUI/CUD`, `ATTRI/ATTRD`, `DQUOT`, `QUOTAOFF`, `IUNLINK` | **refused** | rmap/reflink/attr/quota are outside the writable profile |

The table is evidence-driven: the Linux dirty-log operation matrix (section 8)
records which item types the supported operations actually produce
(`dirty-logs/*.logprint.txt`, `xfswin journal`). If a new Linux operation
introduces an item type outside this table, recovery refuses rather than
guesses.

## 8. Verification

Two oracles are kept; neither side proves itself:

* **A - Linux replays the dirty image** (`linux-verify-rw*-crash`, existing):
  the kernel's own recovery is the semantic oracle for the writer.
* **B - Windows replays the dirty image, Linux verifies the result**
  (`windows-recover-windows-crash` -> `linux-verify-windows-recovery`): every
  RW-1..RW-4 crash image is recovered by `xfswin`; Linux must mount it without
  replaying anything, `xfs_repair -n` must be silent and the log must be clean.
* **B2 - Linux writes the dirty log, Windows replays it**
  (`linux-generate-dirty-logs` -> `windows-recover-linux-logs` ->
  `linux-verify-cross-recovery`): the operation matrix is timestamp, extend,
  truncate, create, unlink, mkdir, rmdir, rename (same/cross/replace),
  hardlink, symlink (inline/remote). Each fixture is a device-mapper snapshot
  taken while I/O is frozen (a real crash-consistent block-device image, not a
  copy of a live file), and the expected post-operation namespace/content is
  recorded as a manifest before the capture. Windows must reproduce it and
  Linux must accept it.
* **C - recovery itself crashes**: `windows-recovery-crash-matrix` kills
  `xfswin` inside recovery at every `recovery_*` point and requires the restart
  to converge (`linux-verify-recovery-crash` re-checks the result).
* **D - independent parser**: `tests/tools/recovery_probe.py` reimplements the
  read side (CRC, cycle restoration, head/tail, op chain, commit detection,
  item inventory) without calling the C++ code; the Windows recovery job runs
  the differential (`xfswin journal --json` vs the probe) before and after
  recovery. `xfs_logprint -t` output for every Linux fixture is captured for
  the same structural comparison (`dirty-logs/*.logprint.txt`).

## 9. Behaviour

* `xfswin recover <image> --yes-i-really-mean-it`: explicit recovery; ends with
  a clean-unmount record and prints records/transactions/items/buffers
  (skipped by LSN), intents completed, unlinked inodes processed and the final
  log state. `--verbose` adds icreate/inode/byte counters.
* `xfswin check`: read-only. A dirty log is reported as
  `journal: dirty - recovery required (recoverable transactions=N, items=M,
  unsupported items=...)`; it never writes.
* `xfswin mount <image> X:` (read-only): a dirty log still refuses; use
  `--force-unsafe-ro` to get the pre-recovery view (clearly warned).
* `xfswin mount <image> X: --rw-experimental`: a dirty log is recovered
  automatically before the volume becomes visible; any recovery error fails the
  mount.
* `xfswin journal <image> [--json]`: structural dump of the log (records,
  transactions, items, unsupported inventory) - the differential/debug tool.

## 10. Not in this milestone

Not replayed and refused when encountered: external logs, realtime devices,
reflink/refcount, rmap (including writable rmapbt), quota, attribute intents,
ACL translation, parent pointers, metadir, zoned filesystems. The writable
feature profile is unchanged.
