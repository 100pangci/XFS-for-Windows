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
| `blf_size` | item size in 32-bit words |
| `blf_flags` | buffer type in bits 11..15 (`XFS_BLFT_*`: AGF/AGI/AGFL/btree/dir/inode/sb...) |
| `blf_len` | buffer length in 512-byte units |
| `blf_blkno` | daddr of the buffer |
| `blf_map_size` | used words of the dirty bitmap |
| `blf_data_map[]` | one bit per 128-byte chunk: 1 = that chunk is logged |

followed by the logged 128-byte chunks, in bitmap order. Replay copies each
logged chunk into the buffer at its offset, then writes the buffer with the
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
  can walk backwards; the very first record after mkfs points at the mkfs
  record (or 0xffffffff if none - to be confirmed against tested output).

## 6. What RW-1 must implement

```text
src/write/log_record.cpp    record assembly: header(s), ops, cycle data, CRC
src/write/log_state.cpp     read the log: newest record, cycle, head/tail,
                            previous block, "is clean"
src/write/transaction.cpp   in-memory transaction: log items, reservations,
                            commit -> write record, then write metadata
src/write/log_item.cpp      inode item and buffer item formatting
src/write/superblock_counters.cpp  (already needed by RW-2/3)
```

Commit ordering (ordered data, then metadata, then the log tail):

1. reserve log space and build the item list;
2. stamp each dirty metadata buffer with the transaction LSN;
3. write the record (header + data) to the log ring, then flush;
4. write the metadata buffers, then flush;
5. only then consider the transaction durable; update the in-memory tail.

Crash points `before_log_write`, `mid_log_record`, `after_log_commit`,
`before_metadata_write`, `mid_metadata_write`, `after_metadata_write`,
`before_device_flush`, `after_device_flush` already exist and are wired into
the data path; RW-1 wires them into this sequence.

The first mutation is deliberately small: **update `mtime`/`ctime` of one
inode** (the write path already knows how to do the data part). That exercises
one inode item, one log record, one metadata buffer, the LSN rules and the
clean-unmount record - without the allocator.

## 7. Open questions to settle with Linux before landing RW-1

These need an experiment (write a candidate log, then mount/replay on Linux and
compare), not more reading:

1. Exact `h_tail_lsn`/`h_prev_block` values for the first record written after
   a clean unmount (including the cycle bump when the write wraps the ring).
2. Whether `h_size` in our records must equal the log's maximum record size
   and how many header blocks a 64 KiB-minimum log expects.
3. The minimal inode buffer (`ilf_blkno/ilf_len/ilf_boffset`) that the inode
   buffer verifier accepts (1 BB vs. a full inode cluster).
4. Whether a checkpoint record needs `th_type = XFS_TRANS_CHECKPOINT` and
   `th_num_items`, and how the commit op must be terminated (zero-length op).
5. The exact CRC coverage (`XLOG_REC_SIZE` vs. the extended header blocks) and
   the cycle-stamp positions for records larger than 32 KiB.

## 8. Test strategy

- Unit: record assembly and parsing round-trip (our own reader), cycle data
  restore, CRC.
- Linux replay: with `XFSWIN_CRASH_AT` at every point of one transaction,
  terminate the process, then on Linux require a `mount` (which replays) and
  `xfs_repair -n` to succeed; the file must show either the old or the new
  mtime.
- Differential: dump the log with `xfs_logprint -t` before/after and compare
  structure with a Linux-created equivalent transaction.
- Only after that: RW-2 (allocator) and beyond.
