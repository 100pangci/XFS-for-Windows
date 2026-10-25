#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent RW-6B reverse-mapping oracle.

This program does not import or invoke any C++ code. It reads the raw image,
walks the forward metadata (AG headers, free-space/inode btrees, allocated
inode data/attr forks and bmap btrees), derives expected owners, then checks
the on-disk rmapbt against that derivation.

usage: rw6b_rmap_probe.py <image> [--json OUT] [--quiet]
       [--allow-no-rmapbt]

Exit 0 means every checked invariant held. A filesystem whose rmapbt is off is
rejected unless --allow-no-rmapbt is supplied.
"""

import argparse
import json
import struct
import sys

SB_MAGIC = 0x58465342
AGF_MAGIC = 0x58414746
AGI_MAGIC = 0x58414749
AGFL_MAGIC = 0x5841464c
RMAP_MAGIC = 0x524D4233  # RMB3
BNO_MAGIC = 0x41423342
CNT_MAGIC = 0x41423343
INO_MAGIC = 0x49414233
FINO_MAGIC = 0x46494233
BMBT_MAGIC = 0x424D4133  # BMA3
DINODE_MAGIC = 0x494E

RO_RMAPBT = 1 << 1
RO_REFLINK = 1 << 2
IN_SPINODES = 1 << 1
IN_BIGTIME = 1 << 3
IN_NREXT64 = 1 << 5

OWN_NULL = (1 << 64) - 1
OWN_UNKNOWN = (1 << 64) - 2
OWN_FS = (1 << 64) - 3
OWN_LOG = (1 << 64) - 4
OWN_AG = (1 << 64) - 5
OWN_INOBT = (1 << 64) - 6
OWN_INODES = (1 << 64) - 7
OWN_REFC = (1 << 64) - 8
OWN_COW = (1 << 64) - 9
OWN_MIN = (1 << 64) - 10

ATTR = 1
BMBT = 2
UNWRITTEN = 4
OFF_MASK = (1 << 54) - 1
OFF_ATTR = 1 << 63
OFF_BMBT = 1 << 62
OFF_UNWRITTEN = 1 << 61


def fail(message):
    raise AssertionError(message)


def crc_table():
    out = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        out.append(c)
    return out


CRC = crc_table()


def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = CRC[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def xfs_crc(buf, off):
    b = bytearray(buf)
    b[off:off + 4] = b"\0\0\0\0"
    return crc32c(bytes(b))


def u8(buf, off):
    return buf[off]


def u16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def popcount(v):
    return bin(v).count("1")


def checked_crc(buf, crc_off, what):
    if xfs_crc(buf, crc_off) != le32(buf, crc_off):
        fail("%s: bad CRC" % what)


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

    def read(self, off, size):
        if off < 0 or size < 0 or off + size > len(self.data):
            fail("read outside image: %d+%d" % (off, size))
        return self.data[off:off + size]


class Geometry:
    def __init__(self, img):
        sb = img.read(0, 512)
        if u32(sb, 0) != SB_MAGIC:
            fail("bad superblock magic")
        self.sb = sb
        self.blocksize = u32(sb, 4)
        self.dblocks = u64(sb, 8)
        self.uuid = sb[32:48]
        self.logstart = u64(sb, 48)
        self.rootino = u64(sb, 56)
        self.agblocks = u32(sb, 84)
        self.agcount = u32(sb, 88)
        self.logblocks = u32(sb, 96)
        self.sectsize = u16(sb, 102)
        self.inodesize = u16(sb, 104)
        self.inopblock = u16(sb, 106)
        self.blocklog = sb[120]
        self.inopblog = sb[123]
        self.agblklog = sb[124]
        self.ro_compat = u32(sb, 212)
        self.incompat = u32(sb, 216)
        self.meta_uuid = sb[248:264]
        if self.meta_uuid == b"\0" * 16:
            self.meta_uuid = self.uuid
        if self.blocksize != (1 << self.blocklog):
            fail("superblock blocklog mismatch")
        if self.inopblock != self.blocksize // self.inodesize:
            fail("superblock inopblock mismatch")
        if (self.agcount == 0 or
                (self.dblocks + self.agblocks - 1) // self.agblocks !=
                self.agcount):
            fail("superblock AG geometry mismatch")
        if self.sectsize < 512 or self.sectsize > self.blocksize:
            fail("unsupported sector/block geometry")
        checked_crc(img.read(0, self.sectsize), 224, "superblock")
        self.rmapbt = bool(self.ro_compat & RO_RMAPBT)
        self.reflink = bool(self.ro_compat & RO_REFLINK)
        self.sparse = bool(self.incompat & IN_SPINODES)

    def ag_offset(self, agno):
        return agno * self.agblocks * self.blocksize

    def ag_blocks(self, agno):
        first = agno * self.agblocks
        return min(self.agblocks, self.dblocks - first)

    def fsb_offset(self, fsb):
        agno = fsb >> self.agblklog
        agbno = fsb & ((1 << self.agblklog) - 1)
        return (agno * self.agblocks + agbno) * self.blocksize

    def ino_offset(self, ino):
        agno = ino >> (self.agblklog + self.inopblog)
        agino = ino & ((1 << (self.agblklog + self.inopblog)) - 1)
        agbno = (agino >> self.inopblog) & ((1 << self.agblklog) - 1)
        slot = agino & ((1 << self.inopblog) - 1)
        return ((agno * self.agblocks + agbno) * self.blocksize +
                slot * self.inodesize)


def read_agf(img, geo, agno):
    buf = img.read(geo.ag_offset(agno) + geo.sectsize, geo.sectsize)
    if u32(buf, 0) != AGF_MAGIC or u32(buf, 8) != agno:
        fail("AG%d: bad AGF" % agno)
    checked_crc(buf, 216, "AG%d AGF" % agno)
    return {
        "bno_root": u32(buf, 16), "cnt_root": u32(buf, 20),
        "rmap_root": u32(buf, 24), "bno_level": u32(buf, 28),
        "cnt_level": u32(buf, 32), "rmap_level": u32(buf, 36),
        "flfirst": u32(buf, 40), "fllast": u32(buf, 44),
        "flcount": u32(buf, 48), "freeblks": u32(buf, 52),
        "longest": u32(buf, 56), "btreeblks": u32(buf, 60),
        "rmap_blocks": u32(buf, 80), "refcount_blocks": u32(buf, 84),
        "refcount_root": u32(buf, 88), "refcount_level": u32(buf, 92),
    }


def read_agi(img, geo, agno):
    buf = img.read(geo.ag_offset(agno) + 2 * geo.sectsize, geo.sectsize)
    if u32(buf, 0) != AGI_MAGIC or u32(buf, 8) != agno:
        fail("AG%d: bad AGI" % agno)
    checked_crc(buf, 312, "AG%d AGI" % agno)
    return {
        "count": u32(buf, 16), "root": u32(buf, 20),
        "level": u32(buf, 24), "freecount": u32(buf, 28),
        "free_root": u32(buf, 328), "free_level": u32(buf, 332),
        "iblocks": u32(buf, 336), "fblocks": u32(buf, 340),
    }


def read_agfl_entries(img, geo, agno, agf):
    off = geo.ag_offset(agno) + 3 * geo.sectsize
    buf = img.read(off, geo.sectsize)
    if u32(buf, 0) != AGFL_MAGIC or u32(buf, 4) != agno:
        fail("AG%d: bad AGFL" % agno)
    checked_crc(buf, 32, "AG%d AGFL" % agno)
    size = (geo.sectsize - 36) // 4
    if agf["flcount"] > size:
        fail("AG%d: AGFL count out of range" % agno)
    out = []
    for i in range(agf["flcount"]):
        slot = (agf["flfirst"] + i) % size
        bno = u32(buf, 36 + slot * 4)
        if bno >= geo.ag_blocks(agno):
            fail("AG%d: AGFL entry out of range" % agno)
        out.append(bno)
    return out


def read_btree(img, geo, agno, root, level, magic, key_size, ptr_size,
               rec_size, name):
    """Walk a non-overlapping short btree; returns (records, block list)."""
    if root >= geo.ag_blocks(agno) or level < 1 or level > 16:
        fail("AG%d: %s root/level invalid" % (agno, name))
    header = 56
    max_node = (geo.blocksize - header) // (key_size + ptr_size)
    max_leaf = (geo.blocksize - header) // rec_size
    records = []
    blocks = []
    seen = set()

    def visit(agbno, expected):
        if agbno in seen or agbno >= geo.ag_blocks(agno):
            fail("AG%d: %s pointer/cycle %d" % (agno, name, agbno))
        seen.add(agbno)
        buf = img.read(geo.ag_offset(agno) + agbno * geo.blocksize,
                       geo.blocksize)
        if u32(buf, 0) != magic:
            fail("AG%d: %s bad magic at %d" % (agno, name, agbno))
        checked_crc(buf, 52, "AG%d %s block %d" % (agno, name, agbno))
        if u32(buf, 48) != agno or buf[32:48] != geo.meta_uuid:
            fail("AG%d: %s header owner/uuid at %d" % (agno, name, agbno))
        want_blkno = (geo.ag_offset(agno) >> 9) + \
            (agbno << (geo.blocklog - 9))
        if u64(buf, 16) != want_blkno:
            fail("AG%d: %s blkno at %d" % (agno, name, agbno))
        lvl = u16(buf, 4)
        num = u16(buf, 6)
        if lvl != expected:
            fail("AG%d: %s level at %d" % (agno, name, agbno))
        maxr = max_leaf if lvl == 0 else max_node
        empty_root = lvl == 0 and agbno == root and num == 0
        if (num == 0 and not empty_root) or num > maxr:
            fail("AG%d: %s numrecs at %d" % (agno, name, agbno))
        if not empty_root and agbno != root and num < maxr // 2:
            fail("AG%d: %s underfull block at %d" % (agno, name, agbno))
        blocks.append(agbno)
        if lvl == 0:
            for i in range(num):
                records.append(buf[header + i * rec_size:
                                   header + (i + 1) * rec_size])
        else:
            ptrbase = header + max_node * key_size
            for i in range(num):
                visit(u32(buf, ptrbase + i * ptr_size), lvl - 1)

    visit(root, level - 1)
    return records, blocks


def parse_inobt_rec(geo, raw):
    start = u32(raw, 0)
    free = u64(raw, 8)
    if geo.sparse:
        holemask, count, freecount = u16(raw, 4), raw[6], raw[7]
    else:
        holemask, count, freecount = 0, 64, u32(raw, 4)
    mask = backing_mask(holemask)
    if count != popcount(mask) or freecount != popcount(free & mask):
        fail("inobt record %d count/free mismatch" % start)
    return {"startino": start, "holemask": holemask, "free": free,
            "count": count, "freecount": freecount}


def backing_mask(holemask):
    mask = 0
    for i in range(16):
        if not (holemask >> i) & 1:
            mask |= 0xF << (i * 4)
    return mask


def unpack_offset(packed, key=False):
    if packed & ~(OFF_MASK | OFF_ATTR | OFF_BMBT | OFF_UNWRITTEN):
        fail("rmap offset has reserved bits: 0x%x" % packed)
    if key and packed & OFF_UNWRITTEN:
        fail("unwritten bit in an rmap key")
    flags = 0
    if packed & OFF_ATTR:
        flags |= ATTR
    if packed & OFF_BMBT:
        flags |= BMBT
    if not key and packed & OFF_UNWRITTEN:
        flags |= UNWRITTEN
    return packed & OFF_MASK, flags


def key_tuple(start, owner, offset, flags):
    packed = (offset & OFF_MASK)
    if flags & ATTR:
        packed |= OFF_ATTR
    if flags & BMBT:
        packed |= OFF_BMBT
    return (start, owner, packed)


def high_key(rec):
    start, count, owner, off, flags = rec
    adj = count - 1
    hs = start + adj
    ho = off
    if not (owner & (1 << 63)) and not (flags & BMBT):
        ho += adj
    return (hs, owner, key_tuple(0, 0, ho, flags)[2])


def load_rmap(img, geo, agno, agf):
    if not geo.rmapbt:
        return None
    root, level = agf["rmap_root"], agf["rmap_level"]
    if root >= geo.ag_blocks(agno) or level < 1 or level > 16:
        fail("AG%d: rmap root/level invalid" % agno)
    header = 56
    max_leaf = (geo.blocksize - header) // 24
    max_node = (geo.blocksize - header) // 44
    records = []
    blocks = []
    seen = set()
    last_level = {}
    last_record_key = [None]

    def validate(rec):
        start, count, owner, off, flags = rec
        agfl_block = (3 * geo.sectsize) // geo.blocksize
        if count == 0:
            fail("AG%d: zero-length rmap record" % agno)
        if start <= agfl_block:
            if owner != OWN_FS or count != agfl_block + 1:
                fail("AG%d: malformed static AG record" % agno)
        elif start >= geo.ag_blocks(agno) or count > geo.ag_blocks(agno) - start:
            fail("AG%d: rmap extent outside AG" % agno)
        if owner & (1 << 63):
            if owner < OWN_MIN or owner > OWN_FS:
                fail("AG%d: invalid special owner 0x%x" % (agno, owner))
            if off != 0 or flags != 0:
                fail("AG%d: special owner flags/offset" % agno)
        else:
            # The owner may live in a different AG than this rmap record:
            # cross-AG file data and bmbt blocks are legal.
            owner_agno = owner >> (geo.agblklog + geo.inopblog)
            agino = owner & ((1 << (geo.agblklog + geo.inopblog)) - 1)
            agbno = agino >> geo.inopblog
            if owner_agno >= geo.agcount or agbno <= agfl_block or \
                    agbno >= geo.ag_blocks(owner_agno):
                fail("AG%d: invalid inode owner %d" % (agno, owner))
            if flags & BMBT:
                if off != 0 or flags & UNWRITTEN:
                    fail("AG%d: malformed bmbt rmap record" % agno)
            else:
                if flags & UNWRITTEN and flags & ATTR:
                    fail("AG%d: unwritten attr rmap record" % agno)
                if off + count > (1 << 54) + (1 << 21) - 1:
                    fail("AG%d: rmap logical range overflow" % agno)

    def visit(agbno, expected):
        if agbno in seen or agbno >= geo.ag_blocks(agno):
            fail("AG%d: rmap pointer/cycle %d" % (agno, agbno))
        seen.add(agbno)
        buf = img.read(geo.ag_offset(agno) + agbno * geo.blocksize,
                       geo.blocksize)
        if u32(buf, 0) != RMAP_MAGIC:
            fail("AG%d: rmap bad magic at %d" % (agno, agbno))
        checked_crc(buf, 52, "AG%d rmap block %d" % (agno, agbno))
        if u32(buf, 48) != agno or buf[32:48] != geo.meta_uuid:
            fail("AG%d: rmap header owner/uuid at %d" % (agno, agbno))
        want = (geo.ag_offset(agno) >> 9) + (agbno << (geo.blocklog - 9))
        if u64(buf, 16) != want:
            fail("AG%d: rmap blkno at %d" % (agno, agbno))
        lvl, num = u16(buf, 4), u16(buf, 6)
        if lvl != expected:
            fail("AG%d: rmap level at %d" % (agno, agbno))
        maxr = max_leaf if lvl == 0 else max_node
        if num == 0 or num > maxr:
            fail("AG%d: rmap numrecs at %d" % (agno, agbno))
        if agbno != root and num < maxr // 2:
            fail("AG%d: underfull rmap block at %d" % (agno, agbno))
        left, right = u32(buf, 8), u32(buf, 12)
        if lvl in last_level:
            prev, prev_right = last_level[lvl]
            if left != prev or prev_right != agbno:
                fail("AG%d: rmap sibling links at %d" % (agno, agbno))
        elif left != 0xFFFFFFFF:
            fail("AG%d: first rmap block has left sibling" % agno)
        last_level[lvl] = (agbno, right)
        blocks.append(agbno)
        if lvl == 0:
            low_out = high_out = None
            for i in range(num):
                raw = buf[header + i * 24:header + (i + 1) * 24]
                start, count = u32(raw, 0), u32(raw, 4)
                owner, off = u64(raw, 8), u64(raw, 16)
                off, flags = unpack_offset(off)
                rec = (start, count, owner, off, flags)
                validate(rec)
                k = key_tuple(start, owner, off, flags)
                if last_record_key[0] is not None and \
                        k <= last_record_key[0]:
                    fail("AG%d: rmap records not strictly ordered" % agno)
                last_record_key[0] = k
                records.append(rec)
                hk = high_key(rec)
                if low_out is None:
                    low_out, high_out = (start, owner, k[2]), hk
                elif hk > high_out:
                    high_out = hk
            return (low_out, high_out)
        ptrbase = header + max_node * 40
        result_low = result_high = None
        prev_low = None
        for i in range(num):
            lo_raw = buf[header + i * 40:header + i * 40 + 20]
            hi_raw = buf[header + i * 40 + 20:header + i * 40 + 40]
            lo_off, lo_flags = unpack_offset(u64(lo_raw, 12), key=True)
            hi_off, hi_flags = unpack_offset(u64(hi_raw, 12), key=True)
            low = key_tuple(u32(lo_raw, 0), u64(lo_raw, 4), lo_off, lo_flags)
            high = key_tuple(u32(hi_raw, 0), u64(hi_raw, 4), hi_off, hi_flags)
            if low > high or (prev_low is not None and low < prev_low):
                fail("AG%d: rmap node key order at %d" % (agno, agbno))
            prev_low = low
            child_low, child_high = visit(u32(buf, ptrbase + i * 4), lvl - 1)
            if child_low != low or child_high != high:
                fail("AG%d: rmap parent/child key mismatch at %d" %
                     (agno, agbno))
            if result_low is None:
                result_low, result_high = low, high
            elif child_high > result_high:
                result_high = child_high
        return (result_low, result_high)

    visit(root, level - 1)
    for lvl, (agbno, right) in last_level.items():
        if right != 0xFFFFFFFF:
            fail("AG%d: rightmost rmap block has right sibling" % agno)
    if len(blocks) != agf["rmap_blocks"]:
        fail("AG%d: AGF rmap_blocks %d != reachable %d" %
             (agno, agf["rmap_blocks"], len(blocks)))
    return records, blocks


def add_expected(owners, start, length, owner, offset=0, flags=0):
    if length <= 0:
        fail("zero-length expected ownership: start=%d owner=%d off=%d" %
             (start, owner, offset))
    if start < 0 or owner & (1 << 63):
        pass
    if offset < 0 or offset + length > (1 << 54) + (1 << 21) - 1:
        fail("expected logical extent out of range")
    owners.append((start, length, owner, offset, flags))


def add_blocks(owners, blocks, owner):
    for b in blocks:
        add_expected(owners, b, 1, owner)


def decode_bmap(raw):
    l0, l1 = u64(raw, 0), u64(raw, 8)
    return (((l0 >> 9) & ((1 << 54) - 1)),
            (((l0 & 0x1FF) << 43) | (l1 >> 21)),
            (l1 & 0x1FFFFF), bool(l0 >> 63))


def parse_bmbt_root(img, geo, root_fsb, ino, fork, owners, target_agno,
                    data_blocks):
    """Traverse a bmbt whose root block number is known."""
    buf = img.read(geo.fsb_offset(root_fsb), geo.blocksize)
    if u32(buf, 0) != BMBT_MAGIC or u64(buf, 56) != ino:
        fail("inode %d: bad bmbt root" % ino)
    checked_crc(buf, 64, "inode %d bmbt root" % ino)
    level = u16(buf, 4)
    maxr = (geo.blocksize - 72) // 16

    def visit(fsb, expected):
        agno2 = fsb >> geo.agblklog
        agbno = fsb & ((1 << geo.agblklog) - 1)
        if agno2 >= geo.agcount or agbno >= geo.ag_blocks(agno2):
            fail("inode %d: bmbt pointer out of range" % ino)
        b = img.read(geo.fsb_offset(fsb), geo.blocksize)
        if u32(b, 0) != BMBT_MAGIC or u64(b, 56) != ino:
            fail("inode %d: bad bmbt block" % ino)
        checked_crc(b, 64, "inode %d bmbt" % ino)
        if b[40:56] != geo.meta_uuid or \
                u64(b, 24) != (geo.fsb_offset(fsb) >> 9):
            fail("inode %d: bmbt identity" % ino)
        lvl, num = u16(b, 4), u16(b, 6)
        if lvl != expected or num == 0 or num > maxr:
            fail("inode %d: bmbt level/count" % ino)
        if agno2 == target_agno:
            data_blocks.append(fsb)
            add_expected(owners, agbno, 1, ino, 0,
                         BMBT | (ATTR if fork == "attr" else 0))
        if lvl == 0:
            for i in range(num):
                so, sb, count, uw = decode_bmap(b[72 + i * 16:88 + i * 16])
                ag = sb >> geo.agblklog
                ab = sb & ((1 << geo.agblklog) - 1)
                if count == 0 or ag >= geo.agcount or ab >= geo.ag_blocks(ag) \
                        or count > geo.ag_blocks(ag) - ab:
                    fail("inode %d: invalid mapped extent" % ino)
                if ag == target_agno:
                    add_expected(owners, ab, count, ino, so,
                                 (ATTR if fork == "attr" else 0) |
                                 (UNWRITTEN if uw else 0))
        else:
            ptrbase = 72 + maxr * 8
            for i in range(num):
                visit(u64(b, ptrbase + i * 8), lvl - 1)

    visit(root_fsb, level)


def parse_inode_fork(img, geo, raw, ino, owners, data_blocks, target_agno):
    forkoff = raw[82]
    # di_forkoff counts 8-byte units of literal area after the 176-byte core:
    # XFS_DFORK_APTR = dinode + 176 + (di_forkoff << 3).
    data_end = 176 + forkoff * 8 if forkoff else geo.inodesize
    if data_end < 176 or data_end > geo.inodesize:
        fail("inode %d: invalid forkoff" % ino)
    for fork, fmt, begin, end in (
            ("data", raw[5], 176, data_end),
            ("attr", raw[83], data_end, geo.inodesize)):
        if end <= begin:
            continue
        if fmt == 1:
            continue
        if fmt == 2:
            flags2 = u64(raw, 120)
            if flags2 & (1 << 4):
                nrext = u64(raw, 24) if fork == "data" else u32(raw, 76)
            else:
                nrext = u32(raw, 76) if fork == "data" else u16(raw, 80)
            if nrext == 0:
                continue
            if begin + nrext * 16 > end:
                fail("inode %d: %s extent list overflows fork" % (ino, fork))
            for i in range(nrext):
                so, sb, count, uw = decode_bmap(
                    raw[begin + i * 16:begin + (i + 1) * 16])
                ag = sb >> geo.agblklog
                ab = sb & ((1 << geo.agblklog) - 1)
                if count == 0 or ag >= geo.agcount or \
                        ab >= geo.ag_blocks(ag) or count > geo.ag_blocks(ag) - ab:
                    fail("inode %d: invalid %s extent" % (ino, fork))
                if ag == target_agno:
                    add_expected(owners, ab, count, ino, so,
                                 (ATTR if fork == "attr" else 0) |
                                 (UNWRITTEN if uw else 0))
        elif fmt == 3:
            level, num = u16(raw, begin), u16(raw, begin + 2)
            maxr = (end - begin - 4) // 16
            if level < 1 or num == 0 or num > maxr:
                fail("inode %d: bad in-fork bmdr" % ino)
            keybase = begin + 4
            ptrbase = begin + 4 + maxr * 8
            for i in range(num):
                root_fsb = u64(raw, ptrbase + i * 8)
                parse_bmbt_root(img, geo, root_fsb, ino, fork, owners,
                                target_agno, data_blocks)
        else:
            fail("inode %d: unsupported %s fork format %d" % (ino, fork, fmt))


def scan_inode_owners(img, geo, target_agno, owners, data_blocks):
    """Scan allocated inodes in every AG, retaining only ownership that lands
    in target_agno (an inode in AG0 may own data/bmbt blocks in AG1)."""
    target_inodes = 0
    for scan_agno in range(geo.agcount):
        agi = read_agi(img, geo, scan_agno)
        raw_recs, _ = read_btree(
            img, geo, scan_agno, agi["root"], agi["level"], INO_MAGIC,
            4, 4, 16, "inobt")
        for rr in raw_recs:
            rec = parse_inobt_rec(geo, rr)
            mask = backing_mask(rec["holemask"])
            for slot in range(64):
                if not (mask >> slot) & 1 or (rec["free"] >> slot) & 1:
                    continue
                agino = rec["startino"] + slot
                ino = (scan_agno << (geo.agblklog + geo.inopblog)) | agino
                raw = img.read(geo.ino_offset(ino), geo.inodesize)
                if u16(raw, 0) != DINODE_MAGIC or raw[4] != 3 or \
                        u64(raw, 152) != ino or raw[160:176] != geo.meta_uuid:
                    fail("inode %d: bad allocated inode image" % ino)
                checked_crc(raw, 100, "inode %d" % ino)
                if u16(raw, 2) == 0:
                    fail("inode %d: allocated inode has mode zero" % ino)
                parse_inode_fork(img, geo, raw, ino, owners, data_blocks,
                                 target_agno)
                if scan_agno == target_agno:
                    target_inodes += 1
    return target_inodes


def normalize(records):
    out = []
    for r in sorted(records, key=lambda x: (x[0], x[2], x[3], x[4])):
        if out:
            p = out[-1]
            contiguous = p[0] + p[1] == r[0]
            special = bool(p[2] & (1 << 63))
            bmbt = bool(p[4] & BMBT)
            logical = p[3] + p[1] == r[3]
            if (contiguous and p[2] == r[2] and p[4] == r[4] and
                    (special or bmbt or logical)):
                out[-1] = (p[0], p[1] + r[1], p[2], p[3], p[4])
                continue
        out.append(r)
    return out


def intersect(a_start, a_len, b_start, b_len):
    end = min(a_start + a_len, b_start + b_len)
    return max(a_start, b_start), max(0, end - max(a_start, b_start))


def check_ag(img, geo, agno, refcount_blocks=None, cow_blocks=None):
    """Derive ownership from the forward metadata and compare it to rmapbt.

    `refcount_blocks` / `cow_blocks` are optional per-AG additions used by the
    RW-6C oracle: refcountbt blocks are owned by OWN_REFC, and active CoW
    staging ranges by OWN_COW.  Reflink allows ordinary written data-fork
    records to overlap physically; those overlaps are legal.
    """
    agf = read_agf(img, geo, agno)
    agi = read_agi(img, geo, agno)
    owners = []
    errors = {"missing": [], "unexpected": [], "overlap_errors": [],
              "owner_errors": [], "free_space_conflicts": []}

    agfl_block = (3 * geo.sectsize) // geo.blocksize
    add_expected(owners, 0, agfl_block + 1, OWN_FS)
    agfl = read_agfl_entries(img, geo, agno, agf)
    add_blocks(owners, agfl, OWN_AG)

    logstart_ag = geo.logstart >> geo.agblklog
    if logstart_ag == agno:
        lstart = geo.logstart & ((1 << geo.agblklog) - 1)
        add_expected(owners, lstart, geo.logblocks, OWN_LOG)

    bno_recs, bno_blocks = read_btree(
        img, geo, agno, agf["bno_root"], agf["bno_level"], BNO_MAGIC,
        8, 4, 8, "bnobt")
    cnt_recs, cnt_blocks = read_btree(
        img, geo, agno, agf["cnt_root"], agf["cnt_level"], CNT_MAGIC,
        8, 4, 8, "cntbt")
    add_blocks(owners, bno_blocks + cnt_blocks, OWN_AG)
    free = [(u32(r, 0), u32(r, 4)) for r in bno_recs]

    inobt_recs, inobt_blocks = read_btree(
        img, geo, agno, agi["root"], agi["level"], INO_MAGIC, 4, 4, 16,
        "inobt")
    inobt = [parse_inobt_rec(geo, r) for r in inobt_recs]
    add_blocks(owners, inobt_blocks, OWN_INOBT)
    finobt_blocks = []
    if geo.ro_compat & 1:
        _, finobt_blocks = read_btree(
            img, geo, agno, agi["free_root"], agi["free_level"], FINO_MAGIC,
            4, 4, 16, "finobt")
        add_blocks(owners, finobt_blocks, OWN_INOBT)

    # Inode chunk backing: only real (non-hole) slots.
    backed_blocks = set()
    for rec in inobt:
        mask = backing_mask(rec["holemask"])
        for slot in range(64):
            if not (mask >> slot) & 1:
                continue
            agbno = (rec["startino"] + slot) >> geo.inopblog
            if agbno >= geo.ag_blocks(agno):
                fail("AG%d: inode chunk block out of range" % agno)
            backed_blocks.add(agbno)
    add_blocks(owners, sorted(backed_blocks), OWN_INODES)

    data_blocks = []
    inode_count = scan_inode_owners(img, geo, agno, owners, data_blocks)

    if geo.reflink:
        if refcount_blocks is None:
            fail("AG%d: reflink oracle did not supply refcount tree blocks" %
                 agno)
        add_blocks(owners, refcount_blocks, OWN_REFC)
        for block in cow_blocks or []:
            add_expected(owners, block, 1, OWN_COW)

    rmap = load_rmap(img, geo, agno, agf)
    if rmap is None:
        return {"ag": agno, "rmapbt": False, "owners": owners}, errors
    actual, rmap_blocks = rmap
    add_blocks(owners, rmap_blocks, OWN_AG)

    expected = normalize(owners)
    actual_norm = normalize(actual)

    # Overlap detection on actual physical coverage.  rmapbt=1,reflink=0 has
    # no legal duplication.  With reflink, two ordinary written data-fork
    # records (normal inode owner, no attr/bmbt/unwritten flags) may overlap
    # because both inodes share the same physical blocks.
    for i in range(1, len(actual_norm)):
        p, c = actual_norm[i - 1], actual_norm[i]
        if p[0] + p[1] > c[0]:
            if geo.reflink and p[4] == 0 and c[4] == 0 and \
                    not (p[2] & (1 << 63)) and not (c[2] & (1 << 63)):
                continue
            errors["overlap_errors"].append(
                {"first": list(p), "second": list(c)})

    eset = {tuple(r) for r in expected}
    aset = {tuple(r) for r in actual_norm}
    errors["missing"] = [list(r) for r in sorted(eset - aset)]
    errors["unexpected"] = [list(r) for r in sorted(aset - eset)]

    # Free extents must never overlap a claimed owner.
    for fs, fl in free:
        for start, length, owner, off, flags in expected:
            a, n = intersect(fs, fl, start, length)
            if n:
                errors["free_space_conflicts"].append(
                    {"free": [fs, fl], "owner": [start, length, owner,
                                                 off, flags]})

    # AGF counters that this oracle can derive.
    if len(rmap_blocks) != agf["rmap_blocks"]:
        errors["owner_errors"].append(
            "AGF rmap_blocks %d != %d" % (agf["rmap_blocks"],
                                          len(rmap_blocks)))
    btree_counted = (len(bno_blocks) - 1) + (len(cnt_blocks) - 1) + \
        (len(rmap_blocks) - 1)
    if btree_counted != agf["btreeblks"]:
        errors["owner_errors"].append(
            "AGF btreeblks %d != %d" % (agf["btreeblks"], btree_counted))
    if geo.incompat & IN_SPINODES:
        if agi["iblocks"] != len(inobt_blocks):
            errors["owner_errors"].append("AGI iblocks mismatch")
        if geo.ro_compat & 1 and agi["fblocks"] != len(finobt_blocks):
            errors["owner_errors"].append("AGI fblocks mismatch")

    return {
        "ag": agno,
        "rmapbt": True,
        "root": agf["rmap_root"],
        "level": agf["rmap_level"],
        "blocks": len(rmap_blocks),
        "rmap_records": len(actual_norm),
        "expected_records": len(expected),
        "allocated_inodes": inode_count,
        "bmbt_blocks": len(data_blocks),
        "fdblocks_contribution": agf["freeblks"] + agf["flcount"] +
        agf["btreeblks"],
        "icount": agi["count"],
        "ifree": agi["freecount"],
        "missing": errors["missing"],
        "unexpected": errors["unexpected"],
        "overlap_errors": errors["overlap_errors"],
        "owner_errors": errors["owner_errors"],
        "free_space_conflicts": errors["free_space_conflicts"],
    }, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--json")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--allow-no-rmapbt", action="store_true")
    args = ap.parse_args()
    img = Image(args.image)
    geo = Geometry(img)
    if not geo.rmapbt and not args.allow_no_rmapbt:
        fail("filesystem does not have rmapbt")
    if geo.reflink:
        fail("reflink filesystems are outside this oracle's writable profile")
    results = []
    failed = False
    try:
        for agno in range(geo.agcount):
            result, errors = check_ag(img, geo, agno)
            results.append(result)
            if any(errors.values()):
                failed = True
            if not args.quiet:
                print("AG %d: rmap records %d, expected %d, missing %d, "
                      "unexpected %d, overlap %d, owner errors %d" %
                      (agno, result.get("rmap_records", 0),
                       result.get("expected_records", 0),
                       len(result.get("missing", [])),
                       len(result.get("unexpected", [])),
                       len(result.get("overlap_errors", [])),
                       len(result.get("owner_errors", []))))
        if geo.rmapbt:
            total_fdblocks = sum(r["fdblocks_contribution"] for r in results)
            total_icount = sum(r["icount"] for r in results)
            total_ifree = sum(r["ifree"] for r in results)
            sb_fdblocks = u64(geo.sb, 144)
            sb_icount = u64(geo.sb, 128)
            sb_ifree = u64(geo.sb, 136)
            if not (total_fdblocks == sb_fdblocks and
                    total_icount == sb_icount and total_ifree == sb_ifree):
                failed = True
                results[0].setdefault("owner_errors", []).append(
                    "superblock counters %d/%d/%d != AG sums %d/%d/%d" %
                    (sb_icount, sb_ifree, sb_fdblocks, total_icount,
                     total_ifree, total_fdblocks))
        summary = {"image": args.image, "agcount": geo.agcount,
                   "rmapbt": geo.rmapbt, "ags": results,
                   "ok": not failed}
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(summary, f, indent=2, sort_keys=True)
                f.write("\n")
        if failed:
            if not args.quiet:
                for ag in results:
                    for key in ("missing", "unexpected", "overlap_errors",
                                "owner_errors", "free_space_conflicts"):
                        for item in ag.get(key, [])[:20]:
                            print("AG%d %s: %r" % (ag["ag"], key, item),
                                  file=sys.stderr)
            return 1
    except AssertionError as e:
        print("rw6b rmap oracle: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
