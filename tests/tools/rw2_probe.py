#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent structural oracle for XFS images written by RW-2.

This tool re-implements the checks xfs_repair performs for the structures RW-2
touches (superblock counters, AGF, AGFL, bnobt, cntbt, inode data fork) from the
public on-disk format. It does not use the C++ parser, so a shared bug cannot
hide: CI runs it on every Windows-written image and then also mounts the image
with a real Linux kernel.

Usage:
  rw2_probe.py <image>                    structural check of the whole image
  rw2_probe.py <image> --inode N          also dump inode N's extent map
  rw2_probe.py <image> --inode N --cat F  write the file's data to F
  rw2_probe.py <image> --inode N --expect-size S
  rw2_probe.py <image> --inode N --old <old.img> --expect-sizes S1,S2
      --expect-content old|new|prefix
"""
import argparse
import struct
import sys

AGF_MAGIC = 0x58414746
AGI_MAGIC = 0x58414749
AGFL_MAGIC = 0x5841464C
SB_MAGIC = 0x58465342
BNO_MAGIC = 0x41423342
CNT_MAGIC = 0x41423343
BMAP_MAGIC = 0x424D4133
DINODE_MAGIC = 0x494E
NULL_AGBLOCK = 0xFFFFFFFF
NULL_FSB = 0xFFFFFFFFFFFFFFFF


def make_crc_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
        table.append(crc)
    return table


CRC_TABLE = make_crc_table()


def crc32c(data, crc=0xFFFFFFFF):
    for b in data:
        crc = (crc >> 8) ^ CRC_TABLE[(crc ^ b) & 0xFF]
    return crc ^ 0xFFFFFFFF


def xfs_crc(buf: bytes, crc_offset: int) -> int:
    return crc32c(buf[:crc_offset] + b"\0\0\0\0" + buf[crc_offset + 4:])


def u16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


class Image:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = self.f.seek(0, 2)

    def read(self, offset, size):
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise SystemExit(
                f"{self.path}: short read at {offset} ({len(data)} != {size})")
        return data


class Geometry:
    def __init__(self, img):
        sb = img.read(0, 512)
        if u32(sb, 0) != SB_MAGIC:
            raise SystemExit("bad superblock magic")
        if xfs_crc(sb, 224) != le32(sb, 224):
            raise SystemExit("bad superblock CRC")
        self.sb = sb
        self.blocksize = u32(sb, 4)
        self.dblocks = u64(sb, 8)
        self.rootino = u64(sb, 56)
        self.agblocks = u32(sb, 84)
        self.agcount = u32(sb, 88)
        self.versionnum = u16(sb, 100)
        self.sectsize = u16(sb, 102)
        self.inodesize = u16(sb, 104)
        self.blocklog = sb[120]
        self.inodelog = sb[122]
        self.inopblog = sb[123]
        self.agblklog = sb[124]
        self.icount = u64(sb, 128)
        self.ifree = u64(sb, 136)
        self.fdblocks = u64(sb, 144)
        self.features_incompat = u32(sb, 216)
        self.lsn = u64(sb, 240)
        self.uuid = bytes(sb[32:48])
        # Without the meta-uuid feature the metadata (and every inode) uses the
        # filesystem UUID; sb_meta_uuid is zero.
        self.meta_uuid = (bytes(sb[248:264])
                          if self.features_incompat & 4 else self.uuid)

    def ag_blocks(self, agno):
        first = agno * self.agblocks
        if first >= self.dblocks:
            return 0
        return min(self.agblocks, self.dblocks - first)

    def ag_offset(self, agno):
        return (agno * self.agblocks) << self.blocklog

    def fsb_offset(self, fsb):
        agno = fsb >> self.agblklog
        agbno = fsb & ((1 << self.agblklog) - 1)
        return ((agno * self.agblocks + agbno) << self.blocklog)

    def inode_offset(self, ino):
        agno = ino >> (self.agblklog + self.inopblog)
        agino = ino & ((1 << (self.agblklog + self.inopblog)) - 1)
        agbno = (agino >> self.inopblog) & ((1 << self.agblklog) - 1)
        offset = agino & ((1 << self.inopblog) - 1)
        fsb = (agno << self.agblklog) | agbno
        return self.fsb_offset(fsb) + offset * self.inodesize


def decode_bmbt_rec(l0, l1):
    unwritten = (l0 >> 63) != 0
    startoff = (l0 >> 9) & ((1 << 54) - 1)
    startblock = ((l0 & 0x1FF) << 43) | (l1 >> 21)
    blockcount = l1 & 0x1FFFFF
    return startoff, startblock, blockcount, unwritten


def read_short_btree(img, geo, agno, root, magic, key_is_len,
                     check_records=True):
    """Walks a bnobt/cntbt and returns (records, block_count, levels)."""
    blocksize = geo.blocksize
    max_node = (blocksize - 56) // 12
    seen = set()
    records = []
    stack = [(root, None)]
    blocks = 0
    levels = 0
    while stack:
        agbno, expected_level = stack.pop()
        if not (0 <= agbno < geo.ag_blocks(agno)):
            raise SystemExit(f"AG{agno}: btree pointer {agbno} out of range")
        if agbno in seen:
            raise SystemExit(f"AG{agno}: btree cycle at {agbno}")
        seen.add(agbno)
        blocks += 1
        buf = img.read(geo.ag_offset(agno) + (agbno << geo.blocklog), blocksize)
        if u32(buf, 0) != magic:
            raise SystemExit(f"AG{agno}: bad btree magic at {agbno}")
        if xfs_crc(buf, 52) != le32(buf, 52):
            raise SystemExit(f"AG{agno}: bad btree CRC at {agbno}")
        if u32(buf, 48) != agno:
            raise SystemExit(f"AG{agno}: btree owner mismatch at {agbno}")
        level = u16(buf, 4)
        numrecs = u16(buf, 6)
        levels = max(levels, level + 1)
        if expected_level is not None and level != expected_level:
            raise SystemExit(f"AG{agno}: btree level mismatch at {agbno}")
        maxrecs = (blocksize - 56) // (8 if level == 0 else 12)
        if numrecs > maxrecs:
            raise SystemExit(f"AG{agno}: btree numrecs {numrecs} > {maxrecs}")
        if level > 0:
            if numrecs == 0:
                raise SystemExit(f"AG{agno}: empty interior node")
            for i in range(numrecs):
                stack.append((u32(buf, 56 + max_node * 8 + i * 4), level - 1))
        else:
            if not check_records:
                continue
            prev = None
            for i in range(numrecs):
                start = u32(buf, 56 + i * 8)
                length = u32(buf, 56 + i * 8 + 4)
                if length == 0:
                    raise SystemExit(f"AG{agno}: zero-length free extent")
                if start + length > geo.ag_blocks(agno):
                    raise SystemExit(f"AG{agno}: free extent out of range")
                key = (length, start) if key_is_len else (start, length)
                if prev is not None and key <= prev:
                    raise SystemExit(f"AG{agno}: free records out of order")
                prev = key
                records.append((start, length))
            rightsib = u32(buf, 12)
            if rightsib != NULL_AGBLOCK:
                if not (0 <= rightsib < geo.ag_blocks(agno)):
                    raise SystemExit(f"AG{agno}: bad right sibling")
    return records, blocks, levels


def read_agfl(img, geo, agno, agf):
    flfirst = u32(agf, 40)
    fllast = u32(agf, 44)
    flcount = u32(agf, 48)
    sector = img.read(geo.ag_offset(agno) + 3 * geo.sectsize, geo.sectsize)
    if u32(sector, 0) != AGFL_MAGIC:
        raise SystemExit(f"AG{agno}: bad AGFL magic")
    if xfs_crc(sector, 32) != le32(sector, 32):
        raise SystemExit(f"AG{agno}: bad AGFL CRC")
    size = (geo.sectsize - 36) // 4
    entries = []
    if flcount:
        if flfirst >= size or fllast >= size:
            raise SystemExit(f"AG{agno}: bad AGFL ring index")
        i = flfirst
        while True:
            bno = u32(sector, 36 + i * 4)
            if bno == NULL_AGBLOCK or bno >= geo.ag_blocks(agno):
                raise SystemExit(f"AG{agno}: bad AGFL entry {bno}")
            entries.append(bno)
            if i == fllast:
                break
            i = (i + 1) % size
    if len(entries) != flcount:
        raise SystemExit(
            f"AG{agno}: AGFL walk found {len(entries)} entries, flcount {flcount}")
    return entries


def check_image(img, verbose=True, blocks_only=False):
    geo = Geometry(img)
    total_free = 0
    total_icount = 0
    total_ifree = 0
    for agno in range(geo.agcount):
        ag_base = geo.ag_offset(agno)
        agf = img.read(ag_base + geo.sectsize, geo.sectsize)
        if u32(agf, 0) != AGF_MAGIC:
            raise SystemExit(f"AG{agno}: bad AGF magic")
        if xfs_crc(agf, 216) != le32(agf, 216):
            raise SystemExit(f"AG{agno}: bad AGF CRC")
        if u32(agf, 8) != agno:
            raise SystemExit(f"AG{agno}: AGF seqno")
        if u32(agf, 12) != geo.ag_blocks(agno):
            raise SystemExit(f"AG{agno}: AGF length")
        if blocks_only:
            # A crash before replay may leave the individual buffers at
            # different transaction points; only each buffer's own identity
            # must be intact.
            read_short_btree(img, geo, agno, u32(agf, 16), BNO_MAGIC,
                             key_is_len=False, check_records=False)
            read_short_btree(img, geo, agno, u32(agf, 20), CNT_MAGIC,
                             key_is_len=True, check_records=False)
            agfl = img.read(ag_base + 3 * geo.sectsize, geo.sectsize)
            if u32(agfl, 0) != AGFL_MAGIC:
                raise SystemExit(f"AG{agno}: bad AGFL magic")
            if xfs_crc(agfl, 32) != le32(agfl, 32):
                raise SystemExit(f"AG{agno}: bad AGFL CRC")
            continue
        freeblks = u32(agf, 52)
        longest = u32(agf, 56)
        btreeblks = u32(agf, 60)
        flcount = u32(agf, 48)
        bno_recs, bno_blocks, bno_levels = read_short_btree(
            img, geo, agno, u32(agf, 16), BNO_MAGIC, key_is_len=False)
        cnt_recs, cnt_blocks, cnt_levels = read_short_btree(
            img, geo, agno, u32(agf, 20), CNT_MAGIC, key_is_len=True)
        if u32(agf, 28) != bno_levels or u32(agf, 32) != cnt_levels:
            raise SystemExit(f"AG{agno}: AGF btree levels")
        if sorted(bno_recs) != sorted(cnt_recs):
            raise SystemExit(f"AG{agno}: bnobt and cntbt differ")
        # Disjoint and ordered by start block.
        ordered = sorted(bno_recs)
        for i in range(1, len(ordered)):
            if ordered[i][0] < ordered[i - 1][0] + ordered[i - 1][1]:
                raise SystemExit(f"AG{agno}: free extents overlap")
        if sum(r[1] for r in bno_recs) != freeblks:
            raise SystemExit(
                f"AG{agno}: freeblks {freeblks} != records "
                f"{sum(r[1] for r in bno_recs)}")
        if max((r[1] for r in bno_recs), default=0) != longest:
            raise SystemExit(f"AG{agno}: longest mismatch")
        expected_btreeblks = (bno_blocks - 1) + (cnt_blocks - 1)
        if btreeblks != expected_btreeblks:
            raise SystemExit(
                f"AG{agno}: btreeblks {btreeblks} != {expected_btreeblks}")
        # The AGFL blocks are free, but reserved.
        entries = read_agfl(img, geo, agno, agf)
        for bno in entries:
            for start, length in bno_recs:
                if start <= bno < start + length:
                    raise SystemExit(
                        f"AG{agno}: AGFL block {bno} is also in the free space tree")
        total_free += freeblks + flcount + btreeblks

        agi = img.read(ag_base + 2 * geo.sectsize, geo.sectsize)
        if u32(agi, 0) != AGI_MAGIC:
            raise SystemExit(f"AG{agno}: bad AGI magic")
        if xfs_crc(agi, 312) != le32(agi, 312):
            raise SystemExit(f"AG{agno}: bad AGI CRC")
        total_icount += u32(agi, 16)
        total_ifree += u32(agi, 28)
        if verbose:
            print(f"  AG{agno}: freeblks={freeblks} flcount={flcount} "
                  f"btreeblks={btreeblks} longest={longest} "
                  f"bnobt={len(bno_recs)} records/{bno_blocks} blocks "
                  f"levels={bno_levels}")
    if blocks_only:
        print("OK: every metadata buffer is individually intact")
        return geo
    if total_free != geo.fdblocks:
        raise SystemExit(
            f"sb_fdblocks {geo.fdblocks} != freeblks+flcount+btreeblks "
            f"{total_free}")
    if total_icount != geo.icount or total_ifree != geo.ifree:
        raise SystemExit(
            f"sb inode counters {geo.icount}/{geo.ifree} != AGI "
            f"{total_icount}/{total_ifree}")
    if verbose:
        print(f"  sb: fdblocks={geo.fdblocks} icount={geo.icount} "
              f"ifree={geo.ifree}")
    return geo


# ---------------------------------------------------------------------------
# Inode data fork
# ---------------------------------------------------------------------------

def read_inode(img, geo, ino):
    offset = geo.inode_offset(ino)
    buf = img.read(offset, geo.inodesize)
    if u16(buf, 0) != DINODE_MAGIC:
        raise SystemExit(f"inode {ino}: bad magic")
    if xfs_crc(buf, 100) != le32(buf, 100):
        raise SystemExit(f"inode {ino}: bad CRC")
    if u64(buf, 152) != ino:
        raise SystemExit(f"inode {ino}: di_ino mismatch")
    if bytes(buf[160:176]) != geo.meta_uuid:
        raise SystemExit(f"inode {ino}: uuid mismatch")
    inode = {
        "raw": buf,
        "mode": u16(buf, 2),
        "format": buf[5],
        "nlink": u32(buf, 16),
        "size": u64(buf, 56),
        "nblocks": u64(buf, 64),
        "nextents": u32(buf, 76),
        "forkoff": buf[82],
        "aformat": buf[83],
        "lsn": u64(buf, 112),
        "ino": ino,
    }
    fork_size = inode["forkoff"] * 8 if inode["forkoff"] else geo.inodesize - 176
    inode["fork_size"] = fork_size
    extents = []
    bmbt_blocks = 0
    if inode["format"] == 2:  # extents
        if inode["nextents"] * 16 > fork_size:
            raise SystemExit(f"inode {ino}: extents exceed the fork")
        for i in range(inode["nextents"]):
            l0 = u64(buf, 176 + i * 16)
            l1 = u64(buf, 176 + i * 16 + 8)
            extents.append(decode_bmbt_rec(l0, l1))
    elif inode["format"] == 3:  # btree
        level = u16(buf, 176)
        numrecs = u16(buf, 178)
        if level < 1 or numrecs == 0:
            raise SystemExit(f"inode {ino}: bad bmbt root")
        root_max = (fork_size - 4) // 16
        if numrecs > root_max:
            raise SystemExit(f"inode {ino}: bmbt root too large")
        extents, bmbt_blocks = walk_bmbt(
            img, geo, ino, level, numrecs,
            [u64(buf, 176 + 4 + root_max * 8 + i * 8) for i in range(numrecs)])
        if inode["nextents"] != len(extents):
            raise SystemExit(
                f"inode {ino}: di_nextents {inode['nextents']} != "
                f"{len(extents)} extents")
    elif inode["format"] == 1:  # local
        pass
    else:
        raise SystemExit(f"inode {ino}: unexpected format {inode['format']}")
    if inode["format"] != 1:
        prev_end = 0
        for startoff, startblock, blockcount, _unwritten in extents:
            if blockcount == 0:
                raise SystemExit(f"inode {ino}: zero-length extent")
            if startoff < prev_end:
                raise SystemExit(f"inode {ino}: overlapping extents")
            prev_end = startoff + blockcount
            if startblock + blockcount > geo.dblocks:
                raise SystemExit(f"inode {ino}: extent out of range")
        data_blocks = sum(e[2] for e in extents)
        if inode["nblocks"] != data_blocks + bmbt_blocks:
            raise SystemExit(
                f"inode {ino}: di_nblocks {inode['nblocks']} != data "
                f"{data_blocks} + bmbt {bmbt_blocks}")
    inode["extents"] = extents
    return inode


def walk_bmbt(img, geo, ino, level, numrecs, ptrs):
    maxrecs = (geo.blocksize - 72) // 16
    extents = []
    blocks = 0
    stack = [(ptr, level - 1) for ptr in ptrs]
    while stack:
        fsb, expected = stack.pop()
        if not (0 < fsb < geo.dblocks):
            raise SystemExit(f"inode {ino}: bmbt pointer out of range")
        offset = geo.fsb_offset(fsb)
        buf = img.read(offset, geo.blocksize)
        if u32(buf, 0) != BMAP_MAGIC:
            raise SystemExit(f"inode {ino}: bad bmbt magic")
        if xfs_crc(buf, 64) != le32(buf, 64):
            raise SystemExit(f"inode {ino}: bad bmbt CRC")
        if u64(buf, 56) != ino:
            raise SystemExit(f"inode {ino}: bad bmbt owner")
        if u64(buf, 24) != offset // 512:
            raise SystemExit(f"inode {ino}: bad bmbt blkno")
        lvl = u16(buf, 4)
        if lvl != expected:
            raise SystemExit(f"inode {ino}: bmbt level mismatch")
        n = u16(buf, 6)
        if n > maxrecs:
            raise SystemExit(f"inode {ino}: bmbt numrecs too large")
        blocks += 1
        if lvl > 0:
            for i in range(n):
                stack.append((u64(buf, 72 + maxrecs * 8 + i * 8), lvl - 1))
        else:
            for i in range(n):
                extents.append(decode_bmbt_rec(u64(buf, 72 + i * 16),
                                               u64(buf, 72 + i * 16 + 8)))
    return extents, blocks


def read_file_data(img, geo, inode):
    if inode["format"] == 1:
        return bytes(inode["raw"][176:176 + inode["size"]])
    out = bytearray(inode["size"])
    for startoff, startblock, blockcount, unwritten in inode["extents"]:
        first = startoff << geo.blocklog
        if first >= inode["size"]:
            break
        length = min(blockcount << geo.blocklog, inode["size"] - first)
        if unwritten:
            continue
        data = img.read(geo.fsb_offset(startblock), length)
        out[first:first + length] = data
    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--inode", type=int, default=None)
    parser.add_argument("--cat", default=None)
    parser.add_argument("--expect-size", type=int, default=None)
    parser.add_argument("--old", default=None)
    parser.add_argument("--expect-sizes", default=None)
    parser.add_argument("--expect-content", default=None,
                        choices=["old", "new", "prefix"])
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--blocks-only", action="store_true",
                        help="pre-replay check: each metadata buffer must be "
                             "intact, cross-buffer counters are not checked")
    args = parser.parse_args()

    img = Image(args.image)
    geo = check_image(img, verbose=not args.quiet, blocks_only=args.blocks_only)
    if args.blocks_only:
        print("OK: every metadata buffer is individually intact")
        return 0
    if args.inode is None:
        print("OK: superblock counters, AGF/AGFL and free space trees are "
              "consistent")
        return 0

    inode = read_inode(img, geo, args.inode)
    if not args.quiet:
        print(f"  inode {args.inode}: size={inode['size']} "
              f"nblocks={inode['nblocks']} nextents={inode['nextents']} "
              f"format={inode['format']}")
        for extent in inode["extents"]:
            print(f"    extent file [{extent[0]}, {extent[0] + extent[2]}): "
                  f"fsb {extent[1]}, {extent[2]} blocks, "
                  f"{'unwritten' if extent[3] else 'written'}")

    if args.expect_size is not None and inode["size"] != args.expect_size:
        raise SystemExit(
            f"inode {args.inode}: size {inode['size']} != {args.expect_size}")

    data = read_file_data(img, geo, inode)
    if args.cat:
        with open(args.cat, "wb") as out:
            out.write(data)

    if args.expect_sizes:
        allowed = {int(x) for x in args.expect_sizes.split(",")}
        if inode["size"] not in allowed:
            raise SystemExit(
                f"inode {args.inode}: size {inode['size']} not in {allowed}")

    if args.expect_content:
        if not args.old:
            raise SystemExit("--expect-content needs --old")
        old_img = Image(args.old)
        old_geo = Geometry(old_img)
        old_inode = read_inode(old_img, old_geo, args.inode)
        old_data = read_file_data(old_img, old_geo, old_inode)
        if args.expect_content == "old":
            if data != old_data:
                raise SystemExit(f"inode {args.inode}: content is not the old one")
        elif args.expect_content == "new":
            if len(data) < len(old_data) or data[:len(old_data)] != old_data:
                raise SystemExit(
                    f"inode {args.inode}: new content does not extend the old one")
        elif args.expect_content == "prefix":
            # Truncate: everything below the new size must still be the old
            # content; the rest may be old data or holes (zeros).
            if data[:inode["size"]] != old_data[:inode["size"]]:
                raise SystemExit(
                    f"inode {args.inode}: retained prefix changed")
            tail = data[inode["size"]:]
            if any(tail) and data != old_data:
                raise SystemExit(
                    f"inode {args.inode}: tail beyond EOF is neither old data "
                    f"nor zeros")
    print("OK: image and inode are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
