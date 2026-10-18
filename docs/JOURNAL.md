# Journal (log) notes - research for RW-1

Status: **research note**. RW-0 (see `DESIGN.md`) writes file data only and
never touches the log. RW-1 introduces metadata transactions, so it needs a
real XFS log writer whose output Linux replays natively. This document records
what was verified against the kernel (`fs/xfs`, kept under `reference/linux/`)
and `xfsprogs` (`reference/xfsprogs-7.2.0/`), plus the plan and the open
questions. It is a specification summary, not copied code.

## 1. Ground rules

- Emit the standard format. No custom records, no "clean log" shortcuts, no
  writing metadata without a committed log record.
- Recovery only trusts a record if it can find it (cycle numbers), validate it
  (CRC/format/uuid) and find a commit operation for the transaction it
  contains. A transaction is applied as a whole or not at all.
- A clean unmount *is* a log record (one op, `XLOG_UNMOUNT_TRANS`). It is the
  only way to tell Linux "no replay needed".
- Every metadata buffer written back for a transaction carries that
  transaction's LSN in its on-disk LSN field (`bb_lsn`, `di_lsn`, `agi_lsn`,
  ...). Recovery uses these fields to avoid replaying an older change over a
  newer buffer.

## 2. The log is a ring of 512-byte blocks

The internal log is `sb_logblocks` filesystem blocks starting at
`sb_logstart`; in log space it is a ring of 512-byte blocks ("basic blocks",
BB). A record occupies:

```
[ 512-byte header block ]  [ optional extension header blocks ]
[ h_len bytes of record data ]              (h_len is a multiple of 512)
```

Everything after the header block(s) is "data" and is addressed in BB.

### Cycle numbers

Every 512-byte block of written log carries a cycle number in its first four
bytes, which is how recovery finds the head/tail of the ring:

- a record header block starts with `XLOG_HEADER_MAGIC_NUM` (0xFEEDBABE), and
  the cycle is the next 32-bit word (`h_cycle`);
- every data block starts with the record's cycle number (indented BE32).

The original first word of each data block is preserved in the record header:
`h_cycle_data[64]` holds one word per data block for the first 32 KiB of data;
larger records add `xlog_rec_ext_header` blocks (each holding another 64
words) and `h_size` grows accordingly. On read, recovery puts the saved words
back before parsing operations.

### `xlog_rec_header` (512 bytes, big-endian fields unless noted)

| offset | field | notes |
|---|---|---|
| 0 | `h_magicno` | 0xFEEDBABE |
| 4 | `h_cycle` | write cycle of this record |
| 8 | `h_version` | 2 for log v2 |
| 12 | `h_len` | data bytes after the header block(s); 512-byte multiple |
| 16 | `h_lsn` | `(cycle << 32) | block of this header` |
| 24 | `h_tail_lsn` | LSN of the oldest record still needed |
| 32 | `h_crc` | **le32** CRC32c over the header block(s) |
| 36 | `h_prev_block` | block number of the previous record |
| 40 | `h_num_logops` | number of operations in this record |
| 44 | `h_cycle_data[64]` | saved first words of the first 64 data blocks |
| 300 | `h_fmt` | `XLOG_FMT_LINUX_LE` (1) |
| 304 | `h_fs_uuid` | must equal `sb_uuid` |
| 320 | `h_size` | iclog size (log record max size) |
| 324 | `h_pad0` | |

The compiler-dependent tail (`h_reserved`) means the CRC is computed over
`XLOG_REC_SIZE` bytes (offsetofend of `h_pad0`/`h_size`); Linux accepts either
of the two historical sizes on little-endian hosts. Our writer should use the
non-i386 layout (`h_size` included) and zero the rest.

## 3. Operations inside a record

Each operation is a 12-byte header followed by `oh_len` bytes of data, padded
to a 4-byte boundary:

| offset | field | notes |
|---|---|---|
| 0 | `oh_tid` | transaction id (same for all ops of one transaction) |
| 4 | `oh_len` | data bytes of this op (0 for a commit op) |
| 8 | `oh_clientid` | 0x69 (`XFS_TRANSACTION`) for transactions, 0xaa (`XFS_LOG`) for the unmount record |
| 9 | `oh_flags` | see below |
| 10 | `oh_res2` | 0 |

Flags: `XLOG_START_TRANS` (0x01), `XLOG_COMMIT_TRANS` (0x02),
`XLOG_CONTINUE_TRANS` (0x04), `XLOG_WAS_CONT_TRANS` (0x08),
`XLOG_END_TRANS` (0x10), `XLOG_UNMOUNT_TRANS` (0x20).

Recovery's state machine (verified in `xfs_log_recover.c`):

1. an op with `XLOG_START_TRANS` opens a transaction; its data must be a
   `xfs_trans_header` (`th_magic` = `XFS_TRANS_HEADER_MAGIC` 0x5452414e, "TRAN",
   `th_num_items` = number of items that follow);
2. ops with flags 0 append item data to the open transaction;
3. an op with `XLOG_COMMIT_TRANS` commits and replays it (data length 0);
4. `XLOG_UNMOUNT_TRANS` is skipped (that record is the clean-unmount marker);
5. any other sequence is a corrupt log.

`xfs_trans_header` is written in **host byte order** (little-endian on x86),
as are the item format headers below. The payloads they describe are on-disk
big-endian structures. Modern kernels write `th_type = XFS_TRANS_CHECKPOINT`
(40); that is the CIL/checkpoint format, which is what our writer should emit
(it is the only type a CIL-enabled log accepts).

## 4. Item formats

An item is identified by the first 16 bits of its data, read in host byte
order: `XFS_LI_INODE` (0x123b) or `XFS_LI_BUF` (0x123c).

### Inode item (`xfs_inode_log_format`, host endian)

| field | meaning |
|---|---|
| `ilf_type` | `XFS_LI_INODE` |
| `ilf_size` | size of the formatted item in 32-bit words (header + logged regions) |
| `ilf_fields` | `XFS_ILOG_*`: `_CORE` (0x001), `_DDATA`, `_DEXT`, `_DBROOT`, `_ADATA`, ... |
| `ilf_asize`, `ilf_dsize` | byte sizes of the logged attr/data regions |
| `ilf_ino` | inode number |
| `ilf_blkno` | daddr (512-byte units) of the inode buffer |
| `ilf_len` | inode buffer length in 512-byte units |
| `ilf_boffset` | byte offset of the inode inside that buffer |

followed by a `struct xfs_log_dinode` payload (host endian, same field order
as the on-disk inode core) and, for `XFS_ILOG_DEXT/DATA/DBROOT`, the logged
data-fork region. Replay reads the inode buffer at `ilf_blkno/ilf_len`,
converts the log dinode with `xfs_log_dinode_to_disk()`, and **sets
`di_lsn` to the LSN of the transaction being replayed**. It skips the item if
the on-disk inode already has an LSN that is newer. Therefore:

- the logged dinode must contain the complete desired core state (not a diff),
- the on-disk inode must be written with the same LSN as the log record.

For RW-1's timestamp update, `ilf_fields = XFS_ILOG_CORE` is enough: the
timestamps live in the core.

### Buffer item (`xfs_buf_log_format`, host endian)

| field | meaning |
|---|---|
| `blf_type` | `XFS_LI_BUF` |
| `blf_size` | number of regions: the format header plus one per contiguous run of `blf_data_map` (the kernel's `xfs_buf_item_format_segment`; recovery's `xlog_recover_do_reg_buffer` consumes exactly one region per run, so the two must match) |
| `blf_flags` | buffer type in bits 11..15 (`XFS_BLFT_*`: AGF/AGI/AGFL/btree/dir/inode/sb...) |
| `blf_len` | buffer length in 512-byte units |
| `blf_blkno` | daddr of the buffer |
| `blf_map_size` | used words of the dirty bitmap |
| `blf_data_map[]` | one bit per 128-byte chunk: 1 = that chunk is logged |

followed by the logged 128-byte chunks, in bitmap order, one log op per
contiguous run (never one op for several runs). Replay copies each logged
region into the buffer at its offset, then writes the buffer with the
transaction's LSN (the buffer verifiers stamp the LSN fields). The base
metadata block header (magic/CRC/LSN) must be part of the logged chunks
whenever the header changes - in practice the whole first chunk is logged for
every metadata buffer.

## 5. Clean and dirty state

`xlog_find_tail()` decides whether recovery is required (`xfs_log_recover.c`):

1. find the head: scan the ring's cycles and locate the start of the newest
   record (`xlog_find_head`);
2. read the newest record; the tail is `BLOCK_LSN(h_tail_lsn)` of that record;
3. **clean** only if the newest record has exactly one op (`h_num_logops == 1`),
   that op has `XLOG_UNMOUNT_TRANS`, and the head block equals the end of that
   record (`rhead_blk + hblks + BTOBB(h_len)`). Then the tail is set to the end
   of the unmount record;
4. otherwise everything from tail to head is replayed, and the head is
   CRC-verified so torn writes are trimmed.

Consequences for the writer:

- A transaction record makes the log dirty. Recovery then replays from the
  record's `h_tail_lsn` to the head. For a single writer that only appends
  records, `h_tail_lsn` should point at the **first record of the batch being
  written** (the tail after this commit); after a clean unmount that is the
  new record itself.
- The clean unmount record must be written at the current head, with the
  current cycle, one op, client 0xaa, and a payload of
  `xfs_unmount_log_format { magic = 0x556e }`. Its position must be the head
  afterwards, which is automatic when it is the last thing written.
- `h_prev_block` must point at the previous record's header block so recovery
  can walk backwards; the first record after mkfs links to the mkfs record,
  which itself uses 0xffffffff (no predecessor) - both confirmed against
  formatter, writer and kernel output.

## 6. What RW-1 implements

```text
include/xfs/log_format.h    on-disk record/op/transaction/item constants
include/xfs/log_record.h    LogOp/LogRecordParams + BuildLogRecord (used by
src/write/log_record.cpp    the writer *and* mkfs's clean record)
include/xfs/transaction.h   log items + the metadata buffers they protect
src/write/transaction.cpp   commit: record -> flush -> metadata -> flush
src/write/inode_write.cpp   SetFileTimes: one inode item, one record
src/core/log_checker.cpp    ReadLogPosition: the state a clean mount leaves
                            behind (cycle, next block, prev link, tail LSN)
```

Commit ordering (ordered data, then metadata, then the log tail):

1. begin: take the write position (scan the log for a clean one the first time),
   assign the transaction id and the LSN, and wrap to block 0 with a new cycle
   early if the reserved space does not fit before the end of the ring (records
   are never split across the end);
2. build the item list, stamp every metadata buffer with the LSN;
3. write the record header block, then the data blocks, then flush - the record
   must be durable before the metadata it describes;
4. write the metadata buffers, then flush;
5. advance the write position, mark the log dirty, invalidate the caches.

Every step has an injection point (`before_log_write`, `mid_log_record`,
`after_log_commit`, `before_metadata_write`, `mid_metadata_write`,
`after_metadata_write`) plus the data-phase and flush points that already
existed. `WriteCleanUnmountRecord` closes the log at the current head with one
unmount op; the CLI and the WinFsp unmount path both call it, so the next mount
(Linux or this build) finds a clean log.

The tail LSN of a record is the record's own LSN: every commit writes and
flushes its metadata before the next transaction starts, so nothing older than
the newest record can still need replaying.

## 7. Open questions, answered

1. **`h_tail_lsn`/`h_prev_block` of the first record after a clean unmount.**
   `xlog_set_state` sets `l_prev_block` to the found record's header block and
   the tail to `(cycle, after_umount_blk)`. We do the same: the next record's
   `h_prev_block` is the unmount record's block, its tail is its own LSN, and a
   wrap to block 0 bumps the cycle (checked by `log_position_of_a_fresh_fixture...`
   and the crash matrix).
2. **`h_size` and header blocks.** For a non-striped internal log the kernel
   uses `XLOG_BIG_RECORD_BSIZE` (32 KiB) as the iclog size, which is exactly
   one header block (`xlog_logrec_hblks`). We write 32768 and never produce a
   record that needs extension headers (rejected above 32 KiB of data).
3. **Minimal inode buffer.** `xfs_inode_buf_verify` validates
   `XFS_BB_TO_FSB(b_length) * inopblock` inodes; for one 512-byte block that is
   zero inodes, so a 1-BB buffer is accepted and recovery checks the magic at
   `ilf_boffset` itself. The item uses `blkno = inode offset / 512`, `len = 1`,
   `boffset = 0`, and replay applies it to a 512-byte buffer.
4. **Checkpoint header.** `th_type = XFS_TRANS_CHECKPOINT` (40), `th_magic`
   'TRAN', `th_tid` = the op tids, `th_num_items` = the number of item regions
   (recovery ignores it, but it is what the CIL writes); the commit op is a
   zero-length op flagged `XLOG_COMMIT_TRANS`, and the start op is a
   zero-length op flagged `XLOG_START_TRANS` (recovery asserts its length is 0).
5. **CRC coverage and cycle stamps.** `xlog_cksum` covers the header (with the
   CRC field zeroed) plus the record data *as written*, i.e. with the cycle
   words already stamped in (`xlog_recover_check_crc` runs in the CRCPASS
   before `xlog_unpack_data` restores them). Extension headers only apply above
   32 KiB, which this writer refuses to produce. Our records match the kernel
   byte for byte; `tests/tools/log_probe.py` recomputes all of it from the
   image, independently of the C++ implementation.

Two more facts learned while implementing:

- **mkfs's clean record has a zero CRC.** The kernel tolerates that
  (`xlog_recover_check_crc` only rejects a mismatch when the stored CRC is set)
  and recovery never reaches it because our records always point the tail at
  themselves - but the formatter now writes the real CRC through the same
  record builder as the writer.
- **The unmount payload's magic is written in host byte order** by the kernel
  (a plain `XLOG_UNMOUNT_TYPE` assignment to a `__be16`). Nothing reads it
  (recovery only looks at the flag), but we match the kernel so log dumps are
  identical.
- **Recovering a torn record.** The head is walked back when a CRC check fails
  (`xlog_find_head`/`xlog_verify_tail` with `first_bad`) and the tail is pushed
  forward when the failure is within `XLOG_MAX_ICLOGS * hsize` of the head.
  That is what makes a crash between "header written" and "data written" leave
  the old state.
- **An inode item names the inode *cluster*, not the inode.** `xfs_imap`
  computes the cluster (8 KiB scaled by inode size on v3 filesystems, floored
  to whole blocks) and `xfs_inode_item_format` logs `blkno` (a 512-byte daddr),
  `len` (the cluster in 512-byte units) and `boffset` (the inode's byte offset
  inside the cluster). Replay reads that buffer with the inode buffer verifier,
  applies the logged core at `boffset`, recomputes the CRC and writes the whole
  buffer back - and then validates the result with `xfs_dinode_verify`. A
  one-block buffer at the inode's own address parses fine but is not what the
  kernel describes; the crash-replay job proved the difference.
- **The kernel marks stale log space with empty records.** While finding the
  head it stamps blocks with `h_cycle = 0` / `h_len = 0` headers ("cleared"
  space); `xlog_get_cycle` and `xlog_valid_rec_header` treat those as
  non-records, so any reader of a log the kernel has touched must ignore them
  (our parser does, or it reports thousands of bogus torn records).
- **The unmount record's op length differs by writer.** mkfs.xfs writes
  `oh_len = 8` with `xfs_unmount_log_format` in host order; the kernel's own
  unmount record uses `oh_len = 0` with the same 8 bytes trailing. Recovery
  only looks at the flag, and the clean/unmount decision is `h_num_logops == 1`
  plus that flag, so both shapes mount identically.

## 8. Test strategy

- Unit (`tests/unit/journal_tests.cpp`): record layout and header fields, op
  chain round-trip, cycle stamping, CRC, parameter rejection, the clean-log
  write position, the clean unmount record closing the log, `SetFileTimes`
  changing only the core fields it owns, and write-ahead ordering observed
  through a recording device (data -> log header -> log data -> flush -> inode
  -> flush).
- Independent oracle (`tests/tools/log_probe.py`): parses an image's log from
  the kernel specification: magic/version/cycle/uuid, `h_lsn` block, `h_len`
  multiple of 512, cycle stamping, the CRC, the op chain, the item payloads and
  whether the newest complete record is an unmount record.
- Crash windows (Windows: `tests/scripts/verify_crash_windows.ps1` +
  `tests/tools/rw_crash_check.py`): one `pwrite` per injection point, then
  require the pre-replay image to be consistent - the log intact except at most
  one torn newest record, the inode never torn, and if the inode changed, a
  complete record with the same LSN and timestamps must describe it.
- Linux replay (`tests/scripts/verify_crash_linux.sh` +
  `tests/tools/rw_replay_check.py`, CI job `linux-verify-crash`): mount each
  crash image (the kernel replays our record), require the old timestamps for
  the points before the record is durable and the *logged* new ones after it,
  require the data to be old or new, require a clean log after the unmount and
  `xfs_repair -n` to stay silent.
- Differential dump: `tests/tools/compare_logprint.py` compares the kernel's
  `xfs_logprint -t` output against `xfswin journal --json` (record chain with
  LSN cycle + block, head/tail/state, committed transaction ids) on the
  Linux-written dirty-log matrix; CI job `windows-recover-linux-logs` runs it
  for every fixture before replaying it (RW-5, `docs/RECOVERY.md` section 8).
- Next: RW-2 (allocator, growth, unwritten extents) and RW-3 (create/unlink).
