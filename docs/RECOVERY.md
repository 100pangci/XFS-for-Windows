# Journal recovery (RW-5)

Status: **implemented** for the writable profile (XFS v5, CRC enabled, internal
log, finobt/inobtcount/sparse inodes/bigtime/NREXT64, rmapbt, reflink/CoW,
native xattrs and parent pointers; no quota/metadir/realtime/external log).
This note documents what the Windows recovery engine does, what it refuses, and
how it is verified. It is the recovery counterpart of `JOURNAL.md` (the
writer).

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

The Linux stale encoding is verified from the Linux 6.17 source
(`xfs_buf_item_format_segment`, `xfs_trans_binval`) and from real kernel logs.
QUAL-2 captured three fixtures from a running kernel (7.0.0-1017-azure) with
4,878 `XFS_BLF_CANCEL` items: every item used `blf_type = XFS_LI_BUF`,
`blf_size = 1`, `blf_flags = 0x2`, one `XFS_BUF_FORMAT` region of
`20 + 4 * blf_map_size` bytes and an all-zero map, with
`blf_map_size = ceil(blf_len / 8)` for `blf_len` in 512-byte blocks. The
independent `cancel_probe.py` reader and `xfs_logprint` agree on the items.
The parser uses only the cancel item's `(blkno, len)` geometry and does not
interpret its map. The writer emits the same shape; real Linux mounts and
repairs images it writes (the hosted kernel has no online scrub ioctl, which
the qualification records as a skip).

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

### 4.3 Buffer invalidation (writer side)

Any metadata block that is freed must be invalidated with
`xfs_trans_binval`: the block's own buffer item is replaced by an
`XFS_BLF_CANCEL` item, and a queued write-back for the same buffer is
suppressed (a stale buffer is never written). Without this, an older
`XFS_LI_BUF` still in the recovery window would overwrite the block after a
new owner reused the address with a direct or ordered write.

The writer emits the Linux stale shape described in 4.1 through
`XfsTransaction::InvalidateMetadataBuffer`. The real Linux kernel replays a
Windows-written cancel image and `xfs_repair -n` accepts the result; the
hosted runner kernel has no online scrub ioctl, which the qualification
records as a skip. The free-owner paths call the allocator's `InvalidateMetadata`
before releasing the block. Covered metadata
includes bnobt/cntbt/rmapbt/refcountbt/inobt/finobt/bmbt blocks, directory
data/leaf/node/free/block blocks, attr DA leaf/node blocks, inode chunks
(cancelled one whole cluster at a time) and multi-block remote symlink
buffers. Pure file data, direct-written remote xattr values and CoW staging
have no buffer item and are never cancelled; AGFL entries are free-space
placeholders and are not invalidated either.

Cancelling and reviving in one transaction is legal: if the same
`(offset, length)` is logged again after `InvalidateMetadataBuffer`, the
writer removes the cancel item (the newer item supersedes the older one in
log order) and logs every chunk, so no older item for that buffer can
contribute stale chunks. An ordered buffer (inode cluster) may be revived
only when an `ICREATE` item in the same transaction rebuilds the cluster.

### 4.4 Inode items

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

A *freed* inode (unlink/rmdir/rename-replace: `xfs_difree` zeroed the core,
`mode == 0`) is a legal log item, and the kernel's verifier only applies the
mode/type, extent-count, fork-offset and realtime checks when the mode is
non-zero. Replay accepts it with exactly that scope (magic, version, checksum,
`di_ino` and the UUID must still hold) and never decodes forks for it; the
readers keep refusing a mode-0 inode, it is not a file.

### 4.5 ICREATE

`XFS_LI_ICREATE` rebuilds whole inode clusters from the logged geometry
(`xfs_ialloc_inode_init`): each inode gets `di_magic`, version 3, the logged
generation, `di_next_unlinked = NULLAGINO`, its `di_ino`, the filesystem UUID
and a fresh CRC; `di_lsn` stays zero like the kernel's fresh inodes. ICREATE is
ordered with the buffer items, before inode items, so an inode allocated in the
same transaction is replayed into the initialized cluster.

### 4.6 The end of pass 2

Partial writes in front of the head are overwritten with empty records of the
previous cycle (`xlog_clear_stale_blocks`), step by step and capped at
`XLOG_TOTAL_REC_SHIFT` blocks, so a later crash cannot mistake them for data.

Superblock counters and LSN (both after the replay, before the completion
transactions):

* lazy counters (`sb_icount`, `sb_ifree`, `sb_fdblocks`) are not authoritative
  after a crash. They are rebuilt from the AGI/AGF headers and written to the
  superblock sector, exactly like `xfs_initialize_perag_data()` does after log
  mount - the completion transactions read them back, and a value that is too
  small makes freeing an inode chunk fail;
* closing the log stamps the superblock's LSN with the LSN of the unmount
  record (the kernel's covering write does the same). Without it every replay
  of kernel-written metadata would leave blocks whose LSN is *ahead of the
  superblock*, which `xfswin check` (like a repair scan) treats as corruption.

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
| `XFS_LI_BUF` | yes | bitmap chunks, cancellation, inode-buffer special case; recognises `IAB3` inobt and `FIB3` finobt with the shared btree LSN/CRC offsets |
| `XFS_LI_INODE` | yes | core + data/attr fork regions, fields mask; legacy and bigtime timestamps, NREXT64 counters |
| `XFS_LI_ICREATE` | yes | cluster rebuild; full and sparse (`ialloc_min_blks`) chunk lengths |
| `XFS_LI_EFI` / `XFS_LI_EFD` | yes | deferred extent free completion + done item |
| `XFS_LI_BUI` / `XFS_LI_BUD` | yes (RW-6C) | deferred data-fork map/unmap completion; one mapping per BUI |
| `XFS_LI_RUI/RUD` | yes (RW-6B) | rmap intent completion, including surviving RUI |
| `XFS_LI_CUI/CUD` | yes (RW-6C recovery-only) | refcount/CoW intent completion; see [REFLINK.md](REFLINK.md) |
| `XFS_LI_ATTRI/ATTRD` | yes (RW-6D) | attribute set/replace/remove intent completion; see [XATTR.md](XATTR.md) |
| `DQUOT`, `QUOTAOFF`, `IUNLINK` | **refused** | quota items are outside the writable profile |

The table is evidence-driven: the Linux dirty-log operation matrix (section 8)
records which item types the supported operations actually produce
(`dirty-logs/inventory.txt` and `dirty-logs/*.journal.json`, generated by
`make_dirty_logs_linux.sh` from the independent probe). If a new Linux
operation introduces an item type outside this table, recovery refuses rather
than guesses.

Expected operation -> item matrix for the writable profile (from the kernel
sources; CI confirms it against real logs):

| Linux operation | log items |
|---|---|
| timestamp | `INODE` |
| extend / write | `INODE`, `BUF` (inode cluster, AGF/AGFL, bnobt/cntbt, superblock) |
| truncate / unlink | `INODE`, `EFI`/`EFD`, `BUF` (AGF/AGFL, btrees, superblock), directory blocks |
| create / mkdir / rmdir | `ICREATE` (new chunk), `INODE`, `BUF` (AGI, inobt, directory blocks, superblock) |
| rename (all forms) / hardlink | `INODE` (both parents, moved inode, replaced target), `BUF` (directory blocks) |
| symlink | `ICREATE` when a chunk is allocated, `INODE` (local target), `BUF` (remote symlink block) |
| setxattr / removexattr | `ATTRI`/`ATTRD`, `INODE`, `BUF` (attr leaf/node, inode cluster, free-space btrees, AGF/AGFL, superblock); remote value blocks are written outside the log |

The Windows writer uses one atomic transaction for an attribute update (inode,
BMBT/DA/remote-fork metadata and free-space buffers) and pre-flushes remote
value data, so its own logs contain `INODE`/`BUF`, not `ATTRI`/`ATTRD`. It
still parses and completes Linux's attribute intents.

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
  hardlink, symlink (inline/remote). Each fixture is a copy of the loop block
  device taken right after the operation's fsync and before the unmount, in the
  same process, while a stream of unforced noise writes keeps the CIL busy. The
  device is not frozen on purpose: a freeze quiesces the log, writes the
  metadata back and moves the tail past the operation, leaving only covering
  records in the window; right after the fsync nothing is in flight, so the
  copy sees every completed write whole and a torn newest record is what
  recovery trims. The expected post-operation namespace/content is recorded as
  a manifest before the capture, and the generator replays a copy of every
  snapshot with the Linux kernel and requires that replay to reach the
  manifest: the fixture exists only if Linux itself recovers to the recorded
  state. Windows must reproduce it and Linux must accept it.
* **C - recovery itself crashes**: `windows-recovery-crash-matrix` kills
  `xfswin` inside recovery at every `recovery_*` point and requires the restart
  to converge (`linux-verify-recovery-crash` re-checks the result).
* **D - independent parser**: `tests/tools/recovery_probe.py` reimplements the
  read side (CRC, cycle restoration, head/tail, op chain, commit detection,
  item inventory) without calling the C++ code; the Windows recovery job runs
  the differential (`xfswin journal --json` vs the probe) before and after
  recovery. The kernel's own `xfs_logprint -t` output is captured for every
  Linux fixture and compared against the same dump
  (`tests/tools/compare_logprint.py`): record chain (LSN cycle + block),
  head/tail/state and the committed transaction ids must agree. (`-t` prints no
  operation lines, so the operation-count checks apply only to default-mode
  output.)

## 9. Behaviour

* `xfswin recover <image> --yes-i-really-mean-it`: explicit recovery; ends with
  a clean-unmount record and prints records/transactions/items/buffers
  (skipped by LSN), intents completed, unlinked inodes processed and the final
  log state. `--verbose` adds icreate/inode/byte counters. A log that needs no
  replay still goes through the completion phases and is closed: the scanned
  position is adopted instead of classifying the log a second time (a torn
  record behind the newest good one reads as clean to the scanner but not to
  the record-latest classifier), and a log whose newest good record already is
  the unmount record is left untouched.
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

## 10. RW-6A modern inode layouts

The RW-5 engine already treated metadata by magic, not by the RW-5 feature
list, so the extension is deliberate and small:

* **finobt buffers** are `FIB3` short-form blocks. They use the same LSN
  (`bb_lsn` + 24) and CRC (`bb_crc` + 52) offsets as the inobt, the same owner
  (AG number) and the same UUID. Replay recognition, cancellation, the
  on-disk-LSN skip rule and the write verifier therefore apply unchanged.
* **AGI replay** copies the whole buffer; `agi_iblocks`/`agi_fblocks` and the
  finobt root/level are preserved exactly as logged.
* **ICREATE** accepts both the full chunk length and the cluster-sized sparse
  length, requires `count == length << sb_inopblog`, and rebuilds only the
  captured clusters. The sparse hole slots are not part of the ICREATE.
* **inode items** carry the host-order `xfs_log_dinode`. Legacy timestamps are
  two 32-bit values, bigtime is one 64-bit nanoseconds value; NREXT64 uses
  `di_big_nextents`/`di_big_anextents` and zero `di_nrext64_pad`. Recovery
  converts them exactly like `xfs_log_dinode_to_disk`, re-checksums the inode
  and runs the full inode verifier, including the feature-flag consistency
  checks.
* The independent RW-6A oracle (`tests/tools/rw6_inode_probe.py`) is run on
  every Windows-recovered image in addition to `xfswin check`; Linux then
  mounts it read-only (proving the log is clean) and runs `xfs_repair -n`.

## 11. Not in this milestone

Not replayed and refused when encountered: external logs, realtime devices,
quota items, ACL translation, metadir, zoned filesystems, and exchange-range
XMI/XMD operations (the feature bit itself is accepted).  RUI/RUD, BUI/BUD,
CUI/CUD and ATTRI/ATTRD have dedicated completion passes; CUI/CUD are accepted
only on reflink filesystems and are completed by both the ordinary writable
mount/recovery path and the standalone `recover` command. A reflink clone/CoW
can log a BUI and CUI in the same transaction. ATTRI/ATTRD completion includes
the private parent-pointer PPTR set/remove/replace operations; a child inode is
opened by inode + generation, and a missing old record is idempotent during
recovery. Attribute intents are completed by both paths as well (see
[XATTR.md](XATTR.md) and [PARENT_POINTERS.md](PARENT_POINTERS.md)). The writable
feature profile remains allow-listed.
