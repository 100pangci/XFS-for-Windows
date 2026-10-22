#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent RW-6A oracle for modern XFS inode metadata.

This is a second implementation (Python only, no project code) of the parts
this milestone writes:

  * superblock feature bits (finobt, inobtcount, sparse inodes, bigtime,
    NREXT64);
  * inobt and finobt trees (every level, CRCs, owners, record semantics);
  * sparse hole masks and backing blocks;
  * AGI counters and agi_iblocks/agi_fblocks;
  * superblock icount/ifree/fdblocks cross-accounting;
  * every inode image in a backed sparse chunk (magic, version, CRC, di_ino,
    uuid, bigtime timestamps, NREXT64 counters).

usage:
  rw6_inode_probe.py <image> [--quiet] [--allow-empty-finobt]

Exit code 0 means every checked invariant held.
"""

import argparse
import struct
import sys
import uuid as uuidlib

SB_MAGIC = 0x58465342
AGF_MAGIC = 0x58414746
AGI_MAGIC = 0x58414749
DINODE_MAGIC = 0x494E
BNO_MAGIC = 0x41423342
CNT_MAGIC = 0x41423343
INO_MAGIC = 0x49414233
FINO_MAGIC = 0x46494233

RO_FINOBT = 1 << 0
RO_RMAPBT = 1 << 1
RO_REFLINK = 1 << 2
RO_INOBTCNT = 1 << 3
IN_SPINODES = 1 << 1
IN_BIGTIME = 1 << 3
IN_NREXT64 = 1 << 5

DIFLAG2_BIGTIME = 1 << 3
DIFLAG2_NREXT64 = 1 << 4

INODES_PER_CHUNK = 64
INODES_PER_HOLEMASK_BIT = 4


def make_crc_table():
    poly = 0x82F63B78
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ poly if (crc & 1) else crc >> 1
        table.append(crc)
    return table


CRC_TABLE = make_crc_table()


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc = CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def xfs_crc(buf, offset):
    copy = bytearray(buf)
    copy[offset:offset + 4] = b"\x00\x00\x00\x00"
    return crc32c(bytes(copy))


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def popcount(value):
    return bin(value).count("1")


def fail(message):
    raise AssertionError(message)


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

    def read(self, offset, size):
        if offset < 0 or offset + size > len(self.data):
            fail("read out of bounds: %d+%d" % (offset, size))
        return self.data[offset:offset + size]


class Geometry:
    def __init__(self, img):
        self.sb = img.read(0, 512)
        if u32(self.sb, 0) != SB_MAGIC:
            fail("bad superblock magic")
        self.blocksize = u32(self.sb, 4)
        self.dblocks = u64(self.sb, 8)
        self.uuid = self.sb[32:48]
        self.agblocks = u32(self.sb, 84)
        self.agcount = u32(self.sb, 88)
        self.sectsize = u16(self.sb, 102)
        self.inodesize = u16(self.sb, 104)
        self.inopblock = u16(self.sb, 106)
        self.blocklog = self.sb[120]
        self.inodelog = self.sb[122]
        self.inopblog = self.sb[123]
        self.agblklog = self.sb[124]
        self.icount = u64(self.sb, 128)
        self.ifree = u64(self.sb, 136)
        self.fdblocks = u64(self.sb, 144)
        self.features2 = u32(self.sb, 200)
        self.ro_compat = u32(self.sb, 212)
        self.incompat = u32(self.sb, 216)
        self.spino_align = u32(self.sb, 228)
        self.inoalignmt = u32(self.sb, 180)
        self.meta_uuid = self.sb[248:264]
        if self.meta_uuid == b"\x00" * 16:
            self.meta_uuid = self.uuid
        if self.blocksize != (1 << self.blocklog):
            fail("block size/log mismatch")
        if self.inopblock != self.blocksize // self.inodesize:
            fail("sb_inopblock mismatch")
        if (self.dblocks + self.agblocks - 1) // self.agblocks != self.agcount:
            fail("agcount mismatch")
        self.finobt = bool(self.ro_compat & RO_FINOBT)
        self.inobtcount = bool(self.ro_compat & RO_INOBTCNT)
        self.sparse = bool(self.incompat & IN_SPINODES)
        self.bigtime = bool(self.incompat & IN_BIGTIME)
        self.nrext64 = bool(self.incompat & IN_NREXT64)
        # xfs_repair validates the CRC over the full sector.
        sector = img.read(0, self.sectsize)
        if xfs_crc(sector, 224) != le32(sector, 224):
            fail("superblock CRC mismatch")

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
        off = agino & ((1 << self.inopblog) - 1)
        return (agno * self.agblocks + agbno) * self.blocksize + \
            off * self.inodesize


def read_agf(img, geo, agno):
    agf = img.read(geo.ag_offset(agno) + geo.sectsize, geo.sectsize)
    if u32(agf, 0) != AGF_MAGIC:
        fail("AG%d: bad AGF magic" % agno)
    if xfs_crc(agf, 216) != le32(agf, 216):
        fail("AG%d: bad AGF CRC" % agno)
    return {
        "bno_root": u32(agf, 16),
        "cnt_root": u32(agf, 20),
        "flcount": u32(agf, 48),
        "freeblks": u32(agf, 52),
        "btreeblks": u32(agf, 60),
    }


def read_agi(img, geo, agno):
    agi = img.read(geo.ag_offset(agno) + 2 * geo.sectsize, geo.sectsize)
    if u32(agi, 0) != AGI_MAGIC:
        fail("AG%d: bad AGI magic" % agno)
    if xfs_crc(agi, 312) != le32(agi, 312):
        fail("AG%d: bad AGI CRC" % agno)
    if u32(agi, 8) != agno:
        fail("AG%d: AGI seqno %d" % (agno, u32(agi, 8)))
    return {
        "length": u32(agi, 12),
        "count": u32(agi, 16),
        "root": u32(agi, 20),
        "level": u32(agi, 24),
        "freecount": u32(agi, 28),
        "free_root": u32(agi, 328),
        "free_level": u32(agi, 332),
        "iblocks": u32(agi, 336),
        "fblocks": u32(agi, 340),
    }


def read_btree(img, geo, agno, root, magic, rec_size, key_size, ptr_size,
               parse_rec, name):
    if root >= geo.ag_blocks(agno):
        fail("AG%d: %s root out of range" % (agno, name))
    header = 56
    max_node = (geo.blocksize - header) // (key_size + ptr_size)
    max_leaf = (geo.blocksize - header) // rec_size
    stack = [(root, None)]
    seen = set()
    records = []
    blocks = 0
    while stack:
        agbno, expected = stack.pop()
        if agbno in seen:
            fail("AG%d: %s cycle at %d" % (agno, name, agbno))
        seen.add(agbno)
        if agbno >= geo.ag_blocks(agno):
            fail("AG%d: %s child out of range" % (agno, name))
        buf = img.read(geo.ag_offset(agno) + agbno * geo.blocksize,
                       geo.blocksize)
        if u32(buf, 0) != magic:
            fail("AG%d: %s bad magic at %d" % (agno, name, agbno))
        if xfs_crc(buf, 52) != le32(buf, 52):
            fail("AG%d: %s bad CRC at %d" % (agno, name, agbno))
        if u32(buf, 48) != agno:
            fail("AG%d: %s bad owner at %d" % (agno, name, agbno))
        if u64(buf, 16) != (geo.ag_offset(agno) >> 9) + \
                (agbno << (geo.blocklog - 9)):
            fail("AG%d: %s bad blkno at %d" % (agno, name, agbno))
        level = u16(buf, 4)
        numrecs = u16(buf, 6)
        if level > 16:
            fail("AG%d: %s level too deep" % (agno, name))
        if expected is not None and level != expected:
            fail("AG%d: %s level mismatch at %d" % (agno, name, agbno))
        maxrecs = max_leaf if level == 0 else max_node
        if numrecs > maxrecs:
            fail("AG%d: %s numrecs %d > %d" % (agno, name, numrecs, maxrecs))
        if numrecs == 0 and level != 0:
            fail("AG%d: %s empty interior node" % (agno, name))
        blocks += 1
        if level == 0:
            for i in range(numrecs):
                off = header + i * rec_size
                records.append(parse_rec(buf, off))
        else:
            for i in range(numrecs):
                ptr = u32(buf, header + max_node * key_size + i * ptr_size)
                stack.append((ptr, level - 1))
    return records, blocks


def parse_inobt_rec(geo):
    def parse(buf, off):
        startino = u32(buf, off)
        free = u64(buf, off + 8)
        if geo.sparse:
            holemask = u16(buf, off + 4)
            count = buf[off + 6]
            freecount = buf[off + 7]
        else:
            holemask = 0
            count = INODES_PER_CHUNK
            freecount = u32(buf, off + 4)
        return {
            "startino": startino,
            "holemask": holemask,
            "count": count,
            "freecount": freecount,
            "free": free,
        }
    return parse


def alloc_mask(holemask):
    mask = 0
    for bit in range(16):
        if not (holemask >> bit) & 1:
            mask |= 0xF << (bit * INODES_PER_HOLEMASK_BIT)
    return mask


def parse_bno_rec(buf, off):
    return (u32(buf, off), u32(buf, off + 4))


def decode_timestamp(raw, bigtime):
    if bigtime:
        nsec = raw % 1000000000
        sec = raw // 1000000000 - (1 << 31)
        return sec, nsec
    sec = struct.unpack(">i", struct.pack(">I", (raw >> 32) & 0xFFFFFFFF))[0]
    nsec = raw & 0xFFFFFFFF
    return sec, nsec


def verify_inode(img, geo, ino, free_bit):
    raw = img.read(geo.ino_offset(ino), geo.inodesize)
    if u16(raw, 0) != DINODE_MAGIC:
        fail("inode %d: bad magic" % ino)
    if raw[4] != 3:
        fail("inode %d: version %d" % (ino, raw[4]))
    if xfs_crc(raw, 100) != le32(raw, 100):
        fail("inode %d: bad CRC" % ino)
    if u64(raw, 152) != ino:
        fail("inode %d: di_ino %d" % (ino, u64(raw, 152)))
    if raw[160:176] != geo.meta_uuid:
        fail("inode %d: uuid mismatch" % ino)
    flags2 = u64(raw, 120)
    bigtime = bool(flags2 & DIFLAG2_BIGTIME)
    nrext64 = bool(flags2 & DIFLAG2_NREXT64)
    if bigtime and not geo.bigtime:
        fail("inode %d: BIGTIME without the filesystem feature" % ino)
    if nrext64 and not geo.nrext64:
        fail("inode %d: NREXT64 without the filesystem feature" % ino)
    if bigtime:
        for off in (32, 40, 48, 144):
            sec, nsec = decode_timestamp(u64(raw, off), True)
            if nsec >= 1000000000:
                fail("inode %d: invalid bigtime nanoseconds" % ino)
            if sec < -(1 << 31) or sec > 16299260424:
                fail("inode %d: bigtime seconds out of range: %d" % (ino, sec))
    if nrext64:
        if u16(raw, 80) != 0:
            fail("inode %d: nonzero di_nrext64_pad" % ino)
        nextents = u64(raw, 24)
        anextents = u32(raw, 76)
    else:
        if u64(raw, 24) != 0:
            fail("inode %d: nonzero v3 padding" % ino)
        nextents = u32(raw, 76)
        anextents = u16(raw, 80)
    mode = u16(raw, 2)
    if free_bit and mode != 0:
        fail("inode %d: free inode has mode %o" % (ino, mode))
    if not free_bit and mode == 0:
        fail("inode %d: allocated inode has mode zero" % ino)
    return {
        "ino": ino,
        "mode": mode,
        "flags2": flags2,
        "bigtime": bigtime,
        "nrext64": nrext64,
        "nextents": nextents,
        "anextents": anextents,
    }


def check_image(path, quiet, allow_empty_finobt):
    img = Image(path)
    geo = Geometry(img)
    if not quiet:
        print("features: finobt=%d inobtcount=%d sparse=%d bigtime=%d "
              "nrext64=%d rmapbt=%d reflink=%d" %
              (geo.finobt, geo.inobtcount, geo.sparse, geo.bigtime,
               geo.nrext64, bool(geo.ro_compat & RO_RMAPBT),
               bool(geo.ro_compat & RO_REFLINK)))

    total_count = 0
    total_free = 0
    total_fdblocks = 0
    inode_images = 0
    for agno in range(geo.agcount):
        agf = read_agf(img, geo, agno)
        agi = read_agi(img, geo, agno)
        if agi["length"] != geo.ag_blocks(agno):
            fail("AG%d: AGI length mismatch" % agno)
        total_fdblocks += agf["freeblks"] + agf["flcount"] + agf["btreeblks"]

        if agi["level"] == 0 or agi["level"] > 16:
            fail("AG%d: inobt level %d" % (agno, agi["level"]))
        inobt, inobt_blocks = read_btree(
            img, geo, agno, agi["root"], INO_MAGIC, 16, 4, 4,
            parse_inobt_rec(geo), "inobt")
        by_start = {}
        last_end = 0
        count = 0
        freecount = 0
        for rec in inobt:
            start = rec["startino"]
            if start < last_end:
                fail("AG%d: inobt records overlap/out of order" % agno)
            if start + INODES_PER_CHUNK > geo.ag_blocks(agno) * geo.inopblock:
                fail("AG%d: inobt record out of range" % agno)
            last_end = start + INODES_PER_CHUNK
            mask = alloc_mask(rec["holemask"]) if geo.sparse else ~0 & \
                ((1 << 64) - 1)
            expected_count = popcount(mask)
            if rec["count"] != expected_count:
                fail("AG%d: chunk %d count %d != hole mask %d" %
                     (agno, start, rec["count"], expected_count))
            actual_free = popcount(rec["free"] & mask)
            if rec["freecount"] != actual_free:
                fail("AG%d: chunk %d freecount %d != %d" %
                     (agno, start, rec["freecount"], actual_free))
            if geo.sparse and (rec["holemask"] & ~0xFFFF):
                fail("AG%d: holemask out of range" % agno)
            if rec["startino"] in by_start:
                fail("AG%d: duplicate inobt record" % agno)
            by_start[start] = rec
            count += rec["count"]
            freecount += rec["freecount"]
        if count != agi["count"] or freecount != agi["freecount"]:
            fail("AG%d: AGI count/free %d/%d != inobt %d/%d" %
                 (agno, agi["count"], agi["freecount"], count, freecount))
        total_count += count
        total_free += freecount

        finobt_blocks = 0
        if geo.finobt:
            if agi["free_level"] == 0:
                fail("AG%d: finobt feature without a root" % agno)
            finobt, finobt_blocks = read_btree(
                img, geo, agno, agi["free_root"], FINO_MAGIC, 16, 4, 4,
                parse_inobt_rec(geo), "finobt")
            if not finobt and not allow_empty_finobt:
                # An empty finobt is legal only when every chunk is full.
                if any(r["freecount"] > 0 for r in inobt):
                    fail("AG%d: finobt is empty but the inobt has free inodes"
                         % agno)
            seen_free = set()
            for f in finobt:
                start = f["startino"]
                if start not in by_start:
                    fail("AG%d: finobt record without inobt record" % agno)
                r = by_start[start]
                for key in ("holemask", "count", "freecount", "free"):
                    if f[key] != r[key]:
                        fail("AG%d: finobt %s mismatch at %d" %
                             (agno, key, start))
                if f["freecount"] <= 0:
                    fail("AG%d: finobt tracks a full chunk" % agno)
                seen_free.add(start)
            for start, r in by_start.items():
                if r["freecount"] > 0 and start not in seen_free:
                    fail("AG%d: finobt missing free chunk %d" % (agno, start))
        elif agi["free_level"] != 0:
            fail("AG%d: finobt root set without the feature" % agno)

        if geo.inobtcount:
            if agi["iblocks"] != inobt_blocks:
                fail("AG%d: agi_iblocks %d != reachable %d" %
                     (agno, agi["iblocks"], inobt_blocks))
            if geo.finobt and agi["fblocks"] != finobt_blocks:
                fail("AG%d: agi_fblocks %d != reachable %d" %
                     (agno, agi["fblocks"], finobt_blocks))

        # Every backed inode image has to verify. On a sparse filesystem this
        # skips hole slots; on a normal filesystem all 64 slots are backed.
        for rec in inobt:
            start = rec["startino"]
            backed = alloc_mask(rec["holemask"]) if geo.sparse else \
                (1 << 64) - 1
            for ino_i in range(64):
                if not (backed >> ino_i) & 1:
                    continue
                agino = start + ino_i
                ino = (agno << (geo.agblklog + geo.inopblog)) | agino
                free_bit = bool((rec["free"] >> ino_i) & 1)
                verify_inode(img, geo, ino, free_bit)
                inode_images += 1

        # Sparse backing must not be in the free-space tree. (A full chunk is
        # also a backed chunk, but its blocks cannot be free by definition.)
        if geo.sparse:
            free_extents, _ = read_btree(
                img, geo, agno, agf["bno_root"], BNO_MAGIC, 8, 8, 4,
                parse_bno_rec, "bnobt")
            for rec in inobt:
                start = rec["startino"]
                sagbno = start >> geo.inopblog
                bit = 0
                while bit < 16:
                    if (rec["holemask"] >> bit) & 1:
                        bit += 1
                        continue
                    run_start = bit
                    while bit < 16 and not ((rec["holemask"] >> bit) & 1):
                        bit += 1
                    inode_offset = run_start * INODES_PER_HOLEMASK_BIT
                    inode_count = (bit - run_start) * \
                        INODES_PER_HOLEMASK_BIT
                    if inode_offset % geo.inopblock != 0 or \
                            inode_count % geo.inopblock != 0:
                        fail("AG%d: unaligned sparse backing at %d" %
                             (agno, start))
                    agbno = sagbno + inode_offset // geo.inopblock
                    blocks = inode_count // geo.inopblock
                    for extent_start, extent_len in free_extents:
                        if agbno < extent_start + extent_len and \
                                extent_start < agbno + blocks:
                            fail("AG%d: backed inode block %d is free" %
                                 (agno, agbno))

    if total_count != geo.icount:
        fail("superblock icount %d != sum of AGI %d" %
             (geo.icount, total_count))
    if total_free != geo.ifree:
        fail("superblock ifree %d != sum of AGI %d" %
             (geo.ifree, total_free))
    if total_fdblocks != geo.fdblocks:
        fail("superblock fdblocks %d != AG free counters %d" %
             (geo.fdblocks, total_fdblocks))

    if not quiet:
        print("ok: %d AGs, inodes %d/%d free, fdblocks %d, %d inode images"
              % (geo.agcount, total_free, total_count, total_fdblocks,
                 inode_images))
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--allow-empty-finobt", action="store_true")
    args = parser.parse_args()
    try:
        return check_image(args.image, args.quiet, args.allow_empty_finobt)
    except AssertionError as exc:
        print("rw6 oracle: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
