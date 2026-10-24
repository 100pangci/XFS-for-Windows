#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent XFS internal-log recovery oracle (RW-5).

This is a from-scratch implementation of the *read* side of XFS log recovery,
written against the published on-disk format and the kernel's documented
algorithms. It never calls the project's C++ parser: it exists to disagree with
it when one of the two is wrong (the Linux kernel remains the highest oracle).

What it implements:

  * superblock geometry (logstart/logblocks/logsectsize/uuid), the ring of
    512-byte basic blocks and the AG-encoded fsblock -> byte offset rule;
  * xlog_find_zeroed / find_cycle_start / find_verify_cycle /
    find_verify_log_record / find_head / find_tail, including the cycle
    wrap-around cases and torn-tail trimming;
  * record validation (magic/version/len/lsn/uuid/format), the CRC over the
    header (CRC field zeroed) plus cycle-stamped data, and extension header
    blocks for records larger than 32 KiB;
  * xlog_unpack_data cycle restoration;
  * the operation state machine (START/COMMIT/CONTINUE/WAS_CONT/UNMOUNT),
    transaction reassembly by tid, continuation regions and split transaction
    headers;
  * an item inventory (BUF/INODE/ICREATE/EFI/EFD/BUI/BUD/...).

Usage:
  recovery_probe.py <image> [--json]
  recovery_probe.py <image> --compare <xfswin-journal.json>

`--compare` checks the structural equality of the two dumps (record
boundaries/LSNs/cycles/op counts, transaction ids, item types and the
per-item identifiers); it exits non-zero on any difference.
"""

import json
import struct
import sys

# --- on-disk constants (xfs_format.h / xfs_log_format.h) -------------------
XLOG_HEADER_MAGIC_NUM = 0xFEEDBABE
XLOG_VERSION_2 = 2
XLOG_HEADER_CYCLE_SIZE = 32 * 1024
XLOG_MAX_ICLOGS = 8
XLOG_MAX_RECORD_BSIZE = 256 * 1024
XLOG_REC_SHIFT_BLOCKS = XLOG_MAX_RECORD_BSIZE // 512          # 512
XLOG_TOTAL_REC_SHIFT_BLOCKS = (XLOG_MAX_ICLOGS * XLOG_MAX_RECORD_BSIZE) // 512
XLOG_FMT_LINUX_LE = 1
XLOG_BIG_RECORD_BSIZE = 32 * 1024
XFS_BLF_CHUNK = 128
XFS_BLF_INODE_BUF = 1 << 0
XFS_BLF_CANCEL = 1 << 1
XFS_TRANSACTION = 0x69
XFS_LOG = 0xAA

XLOG_START_TRANS = 0x01
XLOG_COMMIT_TRANS = 0x02
XLOG_CONTINUE_TRANS = 0x04
XLOG_WAS_CONT_TRANS = 0x08
XLOG_END_TRANS = 0x10
XLOG_UNMOUNT_TRANS = 0x20

ITEM_TYPES = {
    0x1236: "XFS_LI_EFI",
    0x1237: "XFS_LI_EFD",
    0x1238: "XFS_LI_IUNLINK",
    0x123B: "XFS_LI_INODE",
    0x123C: "XFS_LI_BUF",
    0x123D: "XFS_LI_DQUOT",
    0x123E: "XFS_LI_QUOTAOFF",
    0x123F: "XFS_LI_ICREATE",
    0x1240: "XFS_LI_RUI",
    0x1241: "XFS_LI_RUD",
    0x1242: "XFS_LI_CUI",
    0x1243: "XFS_LI_CUD",
    0x1244: "XFS_LI_BUI",
    0x1245: "XFS_LI_BUD",
    0x1246: "XFS_LI_ATTRI",
    0x1247: "XFS_LI_ATTRD",
}

# --- CRC32c (Castagnoli), polynomial 0x1EDC6F41 reflected -------------------
_CRC32C_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if _c & 1 else _c >> 1
    _CRC32C_TABLE.append(_c)


def crc32c(data, crc=0xFFFFFFFF):
    for byte in data:
        crc = _CRC32C_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc


class LogError(Exception):
    pass


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def le16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def make_lsn(cycle, block):
    return (cycle << 32) | block


def lsn_cmp(a, b):
    if (a >> 32) != (b >> 32):
        return -1 if (a >> 32) < (b >> 32) else 1
    if (a & 0xFFFFFFFF) != (b & 0xFFFFFFFF):
        return -1 if (a & 0xFFFFFFFF) < (b & 0xFFFFFFFF) else 1
    return 0


class Geometry:
    """Superblock fields the log logic needs (offsets from xfs_format.h)."""

    def __init__(self, image):
        if be32(image, 0) != 0x58465342:
            raise LogError("not an XFS superblock")
        self.image = image
        self.block_size = be32(image, 4)
        self.ag_blocks = be32(image, 84)
        self.log_start_fsb = be64(image, 48)
        self.log_blocks_fsb = be32(image, 96)
        self.versionnum = be16(image, 100)
        self.sector_size = be16(image, 102)
        self.block_log = image[120]
        self.ag_blk_log = image[124]
        self.log_sector_size = be16(image, 194) or 512
        self.log_sunit = be32(image, 196)
        self.uuid = bytes(image[32:48])
        self.meta_uuid = bytes(image[248:264]) or self.uuid
        agno = self.log_start_fsb >> self.ag_blk_log
        agbno = self.log_start_fsb & ((1 << self.ag_blk_log) - 1)
        self.log_byte = ((agno * self.ag_blocks) + agbno) << self.block_log
        self.log_bb = (self.log_blocks_fsb * self.block_size) // 512
        if self.log_bb <= 0:
            raise LogError("empty log")
        self.log_v2 = (self.versionnum & 0x0400) != 0
        self.log_bsize = (self.log_sunit if self.log_sunit > XLOG_BIG_RECORD_BSIZE
                          else XLOG_BIG_RECORD_BSIZE)


class LogImage:
    """Reads the log ring from a buffer that starts at log block zero.

    Only the superblock and the log are loaded, so probing a multi-GiB image
    does not fault the whole file into memory each time.  The RUI capture
    script calls this scanner once per device-mapper snapshot.
    """

    def __init__(self, log_data, geo):
        self.log_data = log_data
        self.geo = geo

    def read_block(self, block):
        block %= self.geo.log_bb
        off = block * 512
        return self.log_data[off:off + 512]

    def read_raw(self, block, count):
        off = block * 512
        return self.log_data[off:off + count * 512]

    def cycle(self, block):
        data = self.read_block(block)
        first = be32(data, 0)
        return be32(data, 4) if first == XLOG_HEADER_MAGIC_NUM else first


def parse_header(block):
    return {
        "magic": be32(block, 0) == XLOG_HEADER_MAGIC_NUM,
        "cycle": be32(block, 4),
        "version": be32(block, 8),
        "len": be32(block, 12),
        "lsn": be64(block, 16),
        "tail_lsn": be64(block, 24),
        "crc": struct.unpack_from("<I", block, 32)[0],
        "prev": be32(block, 36),
        "num_ops": be32(block, 40),
        "hsize": be32(block, 320),
        "fmt": be32(block, 300),
        "uuid": bytes(block[304:320]),
    }


def header_blocks(header, log_v2):
    if log_v2 and (header["version"] & XLOG_VERSION_2) and \
            header["hsize"] > XLOG_HEADER_CYCLE_SIZE:
        return (header["hsize"] + XLOG_HEADER_CYCLE_SIZE - 1) // XLOG_HEADER_CYCLE_SIZE
    return 1


def find_cycle_start(img, first_blk, last_blk, cycle):
    end_blk = last_blk
    mid_blk = (first_blk + end_blk) // 2
    while mid_blk != first_blk and mid_blk != end_blk:
        if img.cycle(mid_blk) == cycle:
            end_blk = mid_blk
        else:
            first_blk = mid_blk
        mid_blk = (first_blk + end_blk) // 2
    return end_blk


def find_verify_cycle(img, start, count, stop_cycle):
    for i in range(count):
        if img.cycle(start + i) == stop_cycle:
            return start + i
    return -1


def find_verify_log_record(img, start_blk, last_blk, extra_bblks, geo):
    if last_blk <= start_blk:
        raise LogError("log inconsistent (no previous header)")
    num_blks = last_blk - start_blk
    buf = img.read_raw(start_blk, num_blks)
    found = -1
    for i in range(last_blk - 1, -1, -1):
        if i < start_blk:
            raise LogError("log inconsistent (didn't find previous header)")
        block = buf[(i - start_blk) * 512:(i - start_blk + 1) * 512]
        if be32(block, 0) == XLOG_HEADER_MAGIC_NUM:
            found = i
            break
    if found == -1:
        return 1
    header = parse_header(buf[(found - start_blk) * 512:
                              (found - start_blk + 1) * 512])
    xhdrs = header_blocks(header, geo.log_v2)
    if (last_blk - found + extra_bblks) != (header["len"] + 511) // 512 + xhdrs:
        last_blk = found
    return (0, last_blk)


def find_zeroed(img, geo):
    first_cycle = img.cycle(0)
    if first_cycle == 0:
        return (1, 0)
    last_cycle = img.cycle(geo.log_bb - 1)
    if last_cycle != 0:
        return (0, 0)
    last_blk = geo.log_bb - 1
    last_blk = find_cycle_start(img, 0, last_blk, 0)
    num_scan = min(geo.log_bb, XLOG_TOTAL_REC_SHIFT_BLOCKS)
    if last_blk < num_scan:
        num_scan = last_blk
    start_blk = last_blk - num_scan
    new_blk = find_verify_cycle(img, start_blk, num_scan, 0)
    if new_blk != -1:
        last_blk = new_blk
    rc, last_blk = find_verify_log_record(img, start_blk, last_blk, 0, geo)
    return (1, last_blk)


def find_head(img, geo):
    rc, first_blk = find_zeroed(img, geo)
    if rc == 1:
        return first_blk
    first_half = img.cycle(0)
    last_half = img.cycle(geo.log_bb - 1)
    if first_half == last_half:
        head_blk = geo.log_bb
        stop_on_cycle = (last_half - 1) & 0xFFFFFFFF
    else:
        stop_on_cycle = last_half
        head_blk = find_cycle_start(img, 0, geo.log_bb - 1, last_half)
    num_scan = min(geo.log_bb, XLOG_TOTAL_REC_SHIFT_BLOCKS)
    if head_blk >= num_scan:
        start_blk = head_blk - num_scan
        new_blk = find_verify_cycle(img, start_blk, num_scan, stop_on_cycle)
        if new_blk != -1:
            head_blk = new_blk
    else:
        start_blk = geo.log_bb - (num_scan - head_blk)
        new_blk = find_verify_cycle(img, start_blk, num_scan - head_blk,
                                    (stop_on_cycle - 1) & 0xFFFFFFFF)
        if new_blk != -1:
            head_blk = new_blk
        else:
            new_blk = find_verify_cycle(img, 0, head_blk, stop_on_cycle)
            if new_blk != -1:
                head_blk = new_blk
    num_scan = XLOG_REC_SHIFT_BLOCKS
    if head_blk >= num_scan:
        start_blk = head_blk - num_scan
        rc, head_blk = find_verify_log_record(img, start_blk, head_blk, 0, geo)
    else:
        start_blk = 0
        rc, head_blk = find_verify_log_record(img, start_blk, head_blk, 0, geo)
        if rc == 1:
            wrapped_start = geo.log_bb - (num_scan - head_blk)
            new_blk = geo.log_bb
            rc, new_blk = find_verify_log_record(img, wrapped_start, new_blk,
                                                 head_blk, geo)
            if new_blk != geo.log_bb:
                head_blk = new_blk
    return 0 if head_blk == geo.log_bb else head_blk


def rseek_logrec_hdr(img, head_blk, tail_blk, count):
    log_bb = img.geo.log_bb
    found = 0
    rblk = None
    header = None
    wrapped = False
    end_blk = tail_blk if head_blk > tail_blk else 0
    for i in range(head_blk - 1, end_blk - 1, -1):
        block = img.read_block(i)
        if be32(block, 0) == XLOG_HEADER_MAGIC_NUM:
            rblk, header = i, parse_header(block)
            found += 1
            if found == count:
                return found, rblk, header, wrapped
    if tail_blk >= head_blk and found != count:
        for i in range(log_bb - 1, tail_blk - 1, -1):
            block = img.read_block(i)
            if be32(block, 0) == XLOG_HEADER_MAGIC_NUM:
                wrapped = True
                rblk, header = i, parse_header(block)
                found += 1
                if found == count:
                    return found, rblk, header, wrapped
    return found, rblk, header, wrapped


def seek_logrec_hdr(img, head_blk, tail_blk, count):
    log_bb = img.geo.log_bb
    found = 0
    rblk = None
    header = None
    wrapped = False
    end_blk = head_blk if head_blk > tail_blk else log_bb - 1
    for i in range(tail_blk, end_blk + 1):
        block = img.read_block(i)
        if be32(block, 0) == XLOG_HEADER_MAGIC_NUM:
            rblk, header = i, parse_header(block)
            found += 1
            if found == count:
                return found, rblk, header, wrapped
    if tail_blk > head_blk and found != count:
        for i in range(0, head_blk):
            block = img.read_block(i)
            if be32(block, 0) == XLOG_HEADER_MAGIC_NUM:
                wrapped = True
                rblk, header = i, parse_header(block)
                found += 1
                if found == count:
                    return found, rblk, header, wrapped
    return found, rblk, header, wrapped


def record_crc(header_bytes, data, fixed=328):
    header = bytearray(header_bytes[:fixed])
    struct.pack_into("<I", header, 32, 0)
    crc = crc32c(bytes(header))
    # xlog_cksum(): each xlog_rec_ext_header contributes its 260 covered bytes
    # (cycle word + 64 saved data-cycle words), not the 252 reserved bytes.
    for ext in range(1, len(header_bytes) // 512):
        at = ext * 512
        crc = crc32c(bytes(header_bytes[at:at + 260]), crc)
    crc = crc32c(data, crc)
    return (~crc) & 0xFFFFFFFF


def verify_crc(header_bytes, data):
    stored = struct.unpack_from("<I", header_bytes, 32)[0]
    if stored == 0:
        return True, record_crc(header_bytes, data)
    crc = record_crc(header_bytes, data)
    if crc == stored:
        return True, crc
    other = record_crc(header_bytes, data, fixed=324)
    return other == stored, other


def unpack_cycle_data(header_bytes, data):
    blocks = len(data) // 512
    out = bytearray(data)
    for i in range(blocks):
        if i < 64:
            saved = be32(header_bytes, 44 + i * 4)
        else:
            ext = i // 64
            k = i % 64
            at = ext * 512 + 4 + k * 4
            saved = be32(header_bytes, at) if at + 4 <= len(header_bytes) else 0
        struct.pack_into(">I", out, i * 512, saved)
    return bytes(out)


def read_record(img, blk_no, hblks, h_size, geo):
    """Reads one record (header blocks + data), honouring the physical wrap."""
    log_bb = geo.log_bb
    header_bytes = bytearray()
    if blk_no + hblks <= log_bb:
        header_bytes = bytearray(img.read_raw(blk_no, hblks))
    else:
        split = log_bb - blk_no
        header_bytes = bytearray(img.read_raw(blk_no, split))
        header_bytes += img.read_raw(0, hblks - split)
    header = parse_header(header_bytes[:512])
    bblks = (header["len"] + 511) // 512
    data_start = blk_no + hblks
    if data_start + bblks <= log_bb or data_start >= log_bb:
        data = img.read_raw(data_start % log_bb, bblks)
    else:
        split = log_bb - data_start
        data = img.read_raw(data_start, split) + img.read_raw(0, bblks - split)
    return header, bytes(header_bytes), data


def validate_record(header, block, h_size, geo):
    if not header["magic"]:
        raise LogError("block %d is not a log record header" % block)
    if geo.log_v2:
        if header["version"] != XLOG_VERSION_2:
            raise LogError("bad log record version %d" % header["version"])
    elif header["version"] != 1:
        raise LogError("bad log record version %d" % header["version"])
    if header["len"] <= 0 or header["len"] > h_size:
        raise LogError("bad log record length %d" % header["len"])
    if block > geo.log_bb:
        raise LogError("bad log block %d" % block)


def iterate_records(img, head_blk, tail_blk, geo, crc_only=False):
    """xlog_do_recovery_pass: returns records in tail -> head order. On the
    first bad record the raised LogError carries `first_bad` (its block)."""
    if head_blk == tail_blk:
        return []
    log_bb = geo.log_bb
    first = img.read_block(tail_blk)
    h_size = XLOG_BIG_RECORD_BSIZE
    h = parse_header(first)
    if geo.log_v2:
        h_size = h["hsize"]
        if h["len"] > h_size and h["len"] <= geo.log_bsize and h["num_ops"] == 1:
            h_size = geo.log_bsize
        try:
            validate_record(h, tail_blk, h_size, geo)
        except LogError as exc:
            exc.first_bad = tail_blk
            raise
    hblks = 1
    if geo.log_v2 and h_size > XLOG_HEADER_CYCLE_SIZE and (h["version"] & 2):
        hblks = (h_size + XLOG_HEADER_CYCLE_SIZE - 1) // XLOG_HEADER_CYCLE_SIZE

    records = []

    def process(blk_no):
        header, header_bytes, data = read_record(img, blk_no, hblks, h_size, geo)
        try:
            validate_record(header, blk_no, h_size, geo)
        except LogError as exc:
            exc.first_bad = blk_no
            raise
        ok, _ = verify_crc(header_bytes, data)
        if not ok:
            err = LogError("CRC mismatch at block %d" % blk_no)
            err.first_bad = blk_no
            raise err
        if not crc_only:
            records.append((blk_no, header, header_bytes,
                            unpack_cycle_data(header_bytes, data)))
        return header

    if tail_blk > head_blk:
        blk_no = tail_blk
        while blk_no < log_bb:
            header = process(blk_no)
            blk_no += hblks + (header["len"] + 511) // 512
        blk_no -= log_bb
    else:
        blk_no = tail_blk
    while blk_no < head_blk:
        header = process(blk_no)
        blk_no += hblks + (header["len"] + 511) // 512
    return records


def tail_distance(img, head_blk, tail_blk):
    if head_blk < tail_blk:
        return tail_blk - head_blk
    return tail_blk + (img.geo.log_bb - head_blk)


def verify_tail(img, head_blk, tail_blk, hsize, geo):
    orig_tail = tail_blk
    found, tmp_tail, _, _ = seek_logrec_hdr(img, head_blk, tail_blk, 1)
    if found and tmp_tail != tail_blk:
        tail_blk = tmp_tail
    try:
        iterate_records(img, head_blk, tail_blk, geo, crc_only=True)
        return tail_blk
    except LogError as exc:
        first_bad = getattr(exc, "first_bad", None)
    while True:
        max_distance = (XLOG_MAX_ICLOGS * hsize) // 512
        if first_bad is None or tail_distance(img, head_blk, first_bad) > max_distance:
            raise LogError("log tail verification failed")
        found, tmp_tail, _, _ = seek_logrec_hdr(img, head_blk, first_bad, 2)
        if found < 2:
            raise LogError("log tail verification failed")
        tail_blk = tmp_tail
        try:
            iterate_records(img, head_blk, tail_blk, geo, crc_only=True)
            return tail_blk
        except LogError as exc:
            first_bad = getattr(exc, "first_bad", None)


def verify_head(img, head_blk, tail_blk, geo):
    found, tmp_blk, tmp_header, wrapped = rseek_logrec_hdr(
        img, head_blk, tail_blk, XLOG_MAX_ICLOGS)
    if found == 0:
        raise LogError("no record before the head")
    try:
        iterate_records(img, head_blk, tmp_blk, geo, crc_only=True)
    except LogError as exc:
        first_bad = getattr(exc, "first_bad", None)
        if first_bad is None:
            raise
        found, last_blk, last_header, wrapped = rseek_logrec_hdr(
            img, first_bad, tail_blk, 1)
        if found == 0:
            raise LogError("torn write with no good record before it")
        head_blk = first_bad
        tail_blk = last_header["tail_lsn"] & 0xFFFFFFFF
        if head_blk == tail_blk:
            return head_blk, tail_blk, last_blk, last_header, wrapped
        return (head_blk,
                verify_tail(img, head_blk, tail_blk, last_header["hsize"], geo),
                last_blk, last_header, wrapped)
    return (head_blk, verify_tail(img, head_blk, tail_blk, tmp_header["hsize"], geo),
            tmp_blk, tmp_header, wrapped)


def find_tail(img, geo):
    head_blk = find_head(img, geo)
    if head_blk == 0 and img.cycle(0) == 0:
        return {"state": "empty", "head": 0, "tail": 0, "cycle": 0,
                "prev": 0, "newest_lsn": 0, "tail_lsn": 0}
    found, rhead_blk, rhead, wrapped = rseek_logrec_hdr(img, head_blk, head_blk, 1)
    if found == 0:
        return {"state": "unknown", "head": 0, "tail": 0, "cycle": 0,
                "prev": 0, "newest_lsn": 0, "tail_lsn": 0}
    tail_blk = rhead["tail_lsn"] & 0xFFFFFFFF
    head_cycle = (rhead["cycle"] + (1 if wrapped else 0)) & 0xFFFFFFFF
    state = "dirty"
    hblks = header_blocks(rhead, geo.log_v2)
    after_umount = (rhead_blk + hblks + (rhead["len"] + 511) // 512) % geo.log_bb
    if head_blk == after_umount and rhead["num_ops"] == 1:
        op = img.read_block((rhead_blk + hblks) % geo.log_bb)
        if op[9] & XLOG_UNMOUNT_TRANS:
            state = "clean"
            tail_blk = after_umount
    if state != "clean":
        new_head, new_tail, new_blk, new_header, new_wrapped = verify_head(
            img, head_blk, tail_blk, geo)
        if new_head != head_blk:
            head_blk = new_head
            tail_blk = new_tail
            rhead_blk = new_blk
            head_cycle = (new_header["cycle"] + (1 if new_wrapped else 0)) & 0xFFFFFFFF
            rhead = new_header
            hblks = header_blocks(rhead, geo.log_v2)
            after_umount = (rhead_blk + hblks +
                            (rhead["len"] + 511) // 512) % geo.log_bb
            if head_blk == after_umount and rhead["num_ops"] == 1:
                op = img.read_block((rhead_blk + hblks) % geo.log_bb)
                if op[9] & XLOG_UNMOUNT_TRANS:
                    state = "clean"
                    tail_blk = after_umount
    return {"state": state, "head": head_blk, "tail": tail_blk,
            "cycle": head_cycle, "prev": rhead_blk,
            "newest_lsn": rhead["lsn"], "tail_lsn": rhead["tail_lsn"]}


# --- operation stream / transaction reassembly ------------------------------
class Item:
    def __init__(self):
        self.regions = []
        self.total = 0


class Trans:
    def __init__(self, tid, lsn):
        self.tid = tid
        self.lsn = lsn
        self.items = []
        self.item = None
        self.theader = bytearray(16)
        self.have_item = False


def item_type(item):
    return le16(item.regions[0], 0) if item.regions and len(item.regions[0]) >= 2 else 0


def item_summary(item):
    tid = item_type(item)
    out = {"type": ITEM_TYPES.get(tid, "unknown"), "type_id": tid,
           "regions": len(item.regions)}
    first = item.regions[0] if item.regions else b""
    if tid == 0x123C and len(first) >= 20:
        flags = le16(first, 4)
        out.update({"blkno": le32(first, 8) | (le32(first, 12) << 32),
                    "len": le16(first, 6), "flags": flags,
                    "blft": (flags >> 11) & 0x1F})
    elif tid == 0x123B and len(first) >= 56:
        out.update({"ino": le32(first, 16) | (le32(first, 20) << 32),
                    "fields": le32(first, 4),
                    "blkno": le32(first, 40) | (le32(first, 44) << 32),
                    "boffset": le32(first, 52)})
    elif tid == 0x123F and len(first) >= 28:
        out.update({"ag": be32(first, 4), "agbno": be32(first, 8),
                    "count": be32(first, 12)})
    elif tid in (0x1236, 0x1237, 0x1244, 0x1245) and len(first) >= 16:
        out.update({"id": struct.unpack_from("<Q", first, 8)[0]})
    elif tid in (0x1240, 0x1241) and len(first) >= 16:
        out.update({"id": struct.unpack_from("<Q", first, 8)[0]})
        if tid == 0x1240:
            count = le32(first, 4)
            extents = []
            for i in range(count):
                base = 16 + i * 32
                if base + 32 > len(first):
                    break
                extents.append({
                    "owner": struct.unpack_from("<Q", first, base)[0],
                    "startblock": struct.unpack_from("<Q", first, base + 8)[0],
                    "startoff": struct.unpack_from("<Q", first, base + 16)[0],
                    "len": le32(first, base + 24),
                    "flags": le32(first, base + 28),
                    "op": le32(first, base + 28) & 0xFF,
                })
            out.update({"nextents": count, "extents": extents})
    return out


class Reconstruction:
    def __init__(self):
        self.open = {}
        self.transactions = []
        self.ops = 0

    def process_record(self, header, data):
        end = len(data)
        pos = 0
        num_ops = header["num_ops"]
        while pos < end and num_ops:
            if pos + 12 > end:
                raise LogError("op header overrun")
            tid = be32(data, pos)
            olen = be32(data, pos + 4)
            client = data[pos + 8]
            flags = data[pos + 9]
            pos += 12
            if client not in (XFS_TRANSACTION, XFS_LOG):
                raise LogError("bad op client id 0x%x" % client)
            if pos + olen > end:
                raise LogError("op overrun")
            self.ops += 1
            self.process_op(tid, flags, data[pos:pos + olen], header["lsn"])
            pos += olen
            num_ops -= 1

    def process_op(self, tid, flags, payload, lsn):
        trans = self.open.get(tid)
        if trans is None:
            if not (flags & XLOG_START_TRANS):
                return
            if payload:
                raise LogError("START_TRANS carries data")
            self.open[tid] = Trans(tid, lsn)
            return
        flags &= ~XLOG_END_TRANS
        if flags & XLOG_WAS_CONT_TRANS:
            flags &= ~XLOG_CONTINUE_TRANS
        if flags in (0, XLOG_CONTINUE_TRANS):
            self.add_to_trans(trans, payload)
        elif flags == XLOG_WAS_CONT_TRANS:
            self.add_to_cont_trans(trans, payload)
        elif flags == XLOG_COMMIT_TRANS:
            self.commit(trans)
            del self.open[tid]
        elif flags == XLOG_UNMOUNT_TRANS:
            del self.open[tid]
        else:
            raise LogError("bad op flags 0x%x" % flags)

    def add_to_trans(self, trans, payload):
        if not payload:
            return
        if not trans.have_item:
            if len(payload) < 4 or le32(payload, 0) != 0x5452414E:
                raise LogError("bad transaction header magic")
            if len(payload) > 16:
                raise LogError("bad transaction header length")
            trans.theader[:len(payload)] = payload
            if len(payload) == 16:
                trans.have_item = True
                trans.item = Item()
            return
        if trans.item.total and len(trans.item.regions) == trans.item.total:
            self.close_item(trans)
        if not trans.have_item:
            trans.have_item = True
            trans.item = Item()
        if trans.item.total == 0:
            if len(payload) < 4:
                raise LogError("item region smaller than its format")
            size = le16(payload, 2)
            if size == 0 or size > 257:
                raise LogError("bad region count %d" % size)
            trans.item.total = size
        if len(trans.item.regions) >= trans.item.total:
            raise LogError("item region overflow")
        trans.item.regions.append(payload)

    def close_item(self, trans):
        if not trans.have_item:
            return
        if trans.item.regions:
            trans.items.append(trans.item)
        trans.item = None
        trans.have_item = False

    def add_to_cont_trans(self, trans, payload):
        if not trans.have_item:
            if len(payload) > 16:
                raise LogError("bad transaction header length")
            at = 16 - len(payload)
            trans.theader[at:at + len(payload)] = payload
            trans.have_item = True
            trans.item = Item()
            return
        if not trans.item.regions:
            raise LogError("continuation without a region")
        trans.item.regions[-1] += payload

    def commit(self, trans):
        self.close_item(trans)
        if not trans.items:
            raise LogError("committed transaction with no items")
        # xlog_recover_reorder_trans(): buffer items first, then non-buffer
        # items, then inode buffers, then cancelled buffers.
        buffers, items, inode_buffers, cancelled = [], [], [], []
        for item in trans.items:
            tid = item_type(item)
            if tid == 0x123C:
                flags = le16(item.regions[0], 4) if len(item.regions[0]) >= 6 else 0
                blft = (flags >> 11) & 0x1F
                if flags & XFS_BLF_CANCEL:
                    cancelled.append(item)
                elif blft == 8:  # XFS_BLFT_DINO_BUF
                    inode_buffers.append(item)
                else:
                    buffers.append(item)
                continue
            if tid == 0x123F:  # icreate is a buffer-list item
                buffers.append(item)
                continue
            items.append(item)
        ordered = buffers + items + inode_buffers + cancelled
        self.transactions.append({
            "tid": trans.tid, "lsn": "0x%x" % trans.lsn,
            "items": [item_summary(item) for item in ordered]})


def scan(path):
    with open(path, "rb") as stream:
        sb = stream.read(512)
        if len(sb) < 512:
            raise LogError("short superblock read")
        geo = Geometry(sb)
        stream.seek(geo.log_byte)
        log_data = stream.read(geo.log_bb * 512)
    if len(log_data) != geo.log_bb * 512:
        raise LogError("short log read (%d of %d bytes)" %
                       (len(log_data), geo.log_bb * 512))
    img = LogImage(log_data, geo)
    tail = find_tail(img, geo)
    result = {
        "state": tail["state"],
        "head": {"block": tail["head"], "cycle": tail["cycle"]},
        "tail": {"block": tail["tail"],
                 "cycle": (tail["tail_lsn"] >> 32) if tail["state"] != "clean"
                 else tail["cycle"]},
        "newest_lsn": "0x%x" % tail["newest_lsn"],
        "records": [],
        "transactions": [],
        "unsupported": [],
    }
    if tail["state"] != "dirty" or tail["head"] == tail["tail"]:
        return result
    reconstruction = Reconstruction()
    for rblk, header, header_bytes, data in iterate_records(
            img, tail["head"], tail["tail"], geo):
        result["records"].append({
            "lsn": "0x%x" % header["lsn"], "block": rblk,
            "cycle": header["cycle"], "len": header["len"],
            "ops": header["num_ops"], "prev": header["prev"],
            "tail_lsn": "0x%x" % header["tail_lsn"]})
        reconstruction.process_record(header, data)
    result["transactions"] = reconstruction.transactions
    return result


def compare(actual, expected):
    problems = []

    def check(path, a, b):
        if a != b:
            problems.append("%s: oracle=%r xfswin=%r" % (path, a, b))

    check("state", actual["state"], expected["state"])
    check("head.block", actual["head"]["block"], expected["head"]["block"])
    check("head.cycle", actual["head"]["cycle"], expected["head"]["cycle"])
    check("tail.block", actual["tail"]["block"], expected["tail"]["block"])
    ra, rb = actual["records"], expected["records"]
    check("record count", len(ra), len(rb))
    for i in range(min(len(ra), len(rb))):
        for key in ("lsn", "block", "cycle", "len", "ops", "prev", "tail_lsn"):
            check("record[%d].%s" % (i, key), ra[i][key], rb[i][key])
    ta, tb = actual["transactions"], expected["transactions"]
    check("transaction count", len(ta), len(tb))
    for i in range(min(len(ta), len(tb))):
        check("transaction[%d].tid" % i, ta[i]["tid"], tb[i]["tid"])
        check("transaction[%d].lsn" % i, ta[i]["lsn"], tb[i]["lsn"])
        ia, ib = ta[i]["items"], tb[i]["items"]
        check("transaction[%d].item count" % i, len(ia), len(ib))
        for j in range(min(len(ia), len(ib))):
            for key in ia[j]:
                check("transaction[%d].item[%d].%s" % (i, j, key),
                      ia[j][key], ib[j].get(key))
    return problems


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    # The image path is the first non-option argument (accepting options first
    # keeps shell callers from having to remember the order).
    args = argv[1:]
    skip = set()
    if "--compare" in args:
        skip.add(args.index("--compare") + 1)
    path = next((a for i, a in enumerate(args)
                 if not a.startswith("--") and i not in skip), None)
    if path is None:
        print(__doc__)
        return 2
    mode_json = "--json" in argv
    comparison = None
    if "--compare" in argv:
        comparison = argv[argv.index("--compare") + 1]
    result = scan(path)
    if comparison is not None:
        expected = json.load(open(comparison, "r"))
        problems = compare(result, expected)
        for problem in problems:
            print("DIFF: " + problem)
        if problems:
            print("%d structural difference(s)" % len(problems))
            return 1
        print("recovery_probe: structure matches (%d records, %d transactions)"
              % (len(result["records"]), len(result["transactions"])))
        return 0
    if mode_json:
        json.dump(result, sys.stdout, indent=2)
        print()
        return 0
    print("state: %s" % result["state"])
    print("head: block=%d cycle=%d" % (result["head"]["block"],
                                       result["head"]["cycle"]))
    print("tail: block=%d cycle=%d" % (result["tail"]["block"],
                                       result["tail"]["cycle"]))
    print("records: %d" % len(result["records"]))
    for record in result["records"]:
        print("  lsn=%s block=%d cycle=%d len=%d ops=%d"
              % (record["lsn"], record["block"], record["cycle"],
                 record["len"], record["ops"]))
    print("transactions: %d" % len(result["transactions"]))
    for trans in result["transactions"]:
        print("  tid=%d lsn=%s items=%d"
              % (trans["tid"], trans["lsn"], len(trans["items"])))
        for item in trans["items"]:
            print("    %s %s" % (item["type"],
                                 {k: v for k, v in item.items()
                                  if k not in ("type", "type_id")}))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except LogError as exc:
        print("log error: %s" % exc)
        sys.exit(1)
