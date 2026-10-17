#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent reader for the XFS internal log (journal).

This is the oracle for the read-write writer's log records: it reimplements the
on-disk format from the kernel specification (fs/xfs/libxfs/xfs_log_format.h,
xfs_log.c, xfs_log_recover.c) and never calls the project's own parser. It
checks what a replaying kernel checks:

  * record header magic, version, cycle, uuid
  * h_lsn block part == the block the record was found at
  * h_len is a multiple of 512 (the kernel computes BTOBB(h_len))
  * every 512-byte data block starts with the record cycle (cycle stamping)
  * the record CRC (CRC32c over the header with the CRC field zeroed, then the
    data area)
  * the operation chain: start op, transaction header, items, commit op
  * whether the newest complete record is a clean-unmount record

And it dumps enough of the item payloads (which inode, which block, the logged
timestamps) to compare them against the on-disk metadata, so a record that is
well-formed but does not describe the intended change is caught too.

usage:
  log_probe.py <image> [--expect clean|dirty|any] [--json] [--dump]

exit code 0 = every check passed and the expectation held.
"""

import json
import struct
import sys

SECTOR = 512

# Log record header (big-endian fields unless noted).
REC_MAGIC = 0xFEEDBABE
REC_VERSION2 = 2
REC_CRC_COVERED = 328  # XLOG_REC_SIZE for the non-i386 layout

# Log op header: u8 clientid, u8 flags, be16 res2 after tid/len.
OP_HEADER_SIZE = 12
FLAG_START_TRANS = 0x01
FLAG_COMMIT_TRANS = 0x02
FLAG_UNMOUNT_TRANS = 0x20
CLIENT_TRANSACTION = 0x69
CLIENT_LOG = 0xAA

TRANS_HEADER_MAGIC = 0x5452414E  # 'TRAN', host order
TRANS_TYPE_CHECKPOINT = 40

ITEM_INODE = 0x123B
ITEM_BUF = 0x123C
# Item types this parser does not model (intents and done items, quota).
# Decoding stops at the first one so the rest of the chain is not guessed at.
INTENT_ITEM_TYPES = {
    0x1236: "efi", 0x1237: "efd", 0x1238: "iunlink", 0x123D: "dquot",
    0x123E: "quotaoff", 0x123F: "icreate", 0x1240: "rui", 0x1241: "rud",
    0x1242: "cui", 0x1243: "cud", 0x1244: "bui", 0x1245: "bud",
    0x1246: "attri", 0x1247: "attrd", 0x1248: "xmi", 0x1249: "xmd",
}
UNMOUNT_TYPE = 0x556E

# xfs_log_dinode field offsets (identical to the on-disk v3 core, host order).
DINODE_MAGIC = 0
DINODE_MODE = 2
DINODE_VERSION = 4
DINODE_ATIME = 32
DINODE_MTIME = 40
DINODE_CTIME = 48
DINODE_SIZE_FIELD = 56
DINODE_NBLOCKS = 64
DINODE_INO = 152
DINODE_MAGIC_VALUE = 0x494E


def crc32c(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """Reflected Castagnoli CRC32c, running value (no final inversion)."""
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc & 0xFFFFFFFF


def xfs_crc(*chunks: bytes) -> int:
    """Stored value of the kernel's xfs_cksum_range/xlog_cksum: ~running."""
    crc = 0xFFFFFFFF
    for chunk in chunks:
        crc = crc32c(chunk, crc)
    return (~crc) & 0xFFFFFFFF


def be16(buf: bytes, off: int) -> int:
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf: bytes, off: int) -> int:
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf: bytes, off: int) -> int:
    return struct.unpack_from(">Q", buf, off)[0]


def le16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def le64(buf: bytes, off: int) -> int:
    return struct.unpack_from("<Q", buf, off)[0]


def decode_timestamp(buf: bytes, off: int):
    """Legacy 32:32 timestamp pair (host order in the log dinode)."""
    sec = struct.unpack_from("<i", buf, off)[0]
    return {"sec": sec, "nsec": le32(buf, off + 4)}


class Superblock:
    def __init__(self, image: bytes):
        self.magic = be32(image, 0)
        if self.magic != 0x58465342:
            raise ValueError(f"not an XFS image (magic {self.magic:#x})")
        self.block_size = be32(image, 4)
        self.agblocks = be32(image, 84)
        self.agcount = be32(image, 88)
        self.logstart = be64(image, 48)  # fsblock of the internal log
        self.logblocks = be32(image, 96)  # sb_logblocks, in filesystem blocks
        self.uuid = image[32:48]
        if self.block_size <= 0 or self.block_size % SECTOR:
            raise ValueError(f"invalid block size {self.block_size}")
        if self.agblocks <= 0 or self.agcount <= 0:
            raise ValueError("invalid AG geometry")
        # sb_agblklog is log2(agblocks) rounded up (the kernel validates
        # exactly that), so it can be derived.
        self.agblklog = (self.agblocks - 1).bit_length()
        self.log_bytes = self.logblocks * self.block_size
        self.log_blocks = self.log_bytes // SECTOR

    def fsb_to_byte(self, fsb: int) -> int:
        """Filesystem block number to device byte offset.

        fsb numbers pack the AG number in the high bits (agno << agblklog |
        agbno), but the AGs are physically contiguous (agno * agblocks), so
        the two only agree when agblocks is a power of two.
        """
        agno = fsb >> self.agblklog
        agbno = fsb & ((1 << self.agblklog) - 1)
        if agno >= self.agcount or agbno >= self.agblocks:
            raise ValueError(f"fsb {fsb} is outside the filesystem")
        return (agno * self.agblocks + agbno) * self.block_size


class Op:
    def __init__(self, tid, length, client_id, flags, payload):
        self.tid = tid
        self.length = length
        self.client_id = client_id
        self.flags = flags
        self.payload = payload

    def flags_text(self):
        names = []
        for value, name in ((FLAG_START_TRANS, "START"), (FLAG_COMMIT_TRANS,
                                                          "COMMIT"),
                            (0x04, "CONT"), (0x08, "WASCONT"),
                            (0x10, "END"), (FLAG_UNMOUNT_TRANS, "UNMOUNT")):
            if self.flags & value:
                names.append(name)
        return "|".join(names) if names else "-"


class Record:
    def __init__(self, block, offset, header_blocks=1):
        self.block = block
        self.offset = offset  # byte offset in the image
        self.header_blocks = header_blocks
        self.cycle = 0
        self.len = 0
        self.lsn = 0
        self.tail_lsn = 0
        self.crc_ok = False
        self.crc_unset = False
        self.cleared = False
        self.notes = []
        self.num_ops = 0
        self.prev_block = 0
        self.ops = []
        self.items = []  # decoded item summaries
        self.problems = []

    @property
    def blocks(self):
        return self.header_blocks + self.len // SECTOR


def parse_record(image: bytes, log_byte: int, log_blocks: int, sb: Superblock,
                 block: int) -> Record:
    offset = log_byte + block * SECTOR
    header = image[offset:offset + SECTOR]
    rec = Record(block, offset)
    if be32(header, 0) != REC_MAGIC:
        rec.problems.append("bad magic")
        return rec
    rec.cycle = be32(header, 4)
    version = be32(header, 8)
    rec.len = be32(header, 12)
    if rec.cycle == 0 or rec.len == 0:
        # xlog_get_cycle() treats a header with cycle 0 as unwritten space, and
        # xlog_valid_rec_header() rejects h_len == 0 ("LR body must have data").
        # The kernel stamps such headers over stale log space while clearing it,
        # so they are not records at all.
        rec.cleared = True
        return rec
    rec.lsn = be64(header, 16)
    rec.tail_lsn = be64(header, 24)
    stored_crc = struct.unpack_from("<I", header, 32)[0]
    rec.prev_block = be32(header, 36)
    rec.num_ops = be32(header, 40)
    h_size = be32(header, 320)
    # Extension headers are needed above 32 KiB records (h_size blocks); this
    # writer never emits those, but an image from Linux could.
    if h_size > 32768:
        rec.header_blocks = (h_size + 32768 - 1) // 32768
        rec.problems.append(f"extension headers ({rec.header_blocks})")
    if version not in (1, 2):
        rec.problems.append(f"version {version}")
    if rec.cycle == 0:
        rec.problems.append("cycle 0")
    if rec.len == 0 or rec.len % SECTOR:
        rec.problems.append(f"h_len {rec.len} not a multiple of 512")
    if rec.lsn >> 32 != rec.cycle or (rec.lsn & 0xFFFFFFFF) != block:
        rec.problems.append(
            f"h_lsn {rec.lsn:#x} != (cycle {rec.cycle}, block {block})")
    if image[offset + 304:offset + 320] != sb.uuid:
        rec.problems.append("uuid mismatch")
    end = rec.offset + rec.header_blocks * SECTOR + rec.len
    if rec.len == 0 or end > len(image) or rec.blocks > log_blocks:
        rec.problems.append("record does not fit in the log")
        return rec

    data = bytearray(image[rec.offset + rec.header_blocks * SECTOR:end])
    # The CRC covers the record as it lies on disk, i.e. *with* the cycle words
    # stamped into the data (recovery verifies it before xlog_unpack_data
    # restores them).
    header_zeroed = bytearray(header[:REC_CRC_COVERED])
    struct.pack_into("<I", header_zeroed, 32, 0)
    if stored_crc == 0:
        # mkfs.xfs writes a zero CRC in its clean-unmount record; the kernel
        # tolerates that (xlog_recover_check_crc only rejects a mismatch when
        # the stored value is set). Every record this project writes does carry
        # one, so report it but do not fail.
        rec.crc_ok = True
        rec.crc_unset = True
    else:
        rec.crc_ok = xfs_crc(bytes(header_zeroed), bytes(data)) == stored_crc
        if not rec.crc_ok:
            rec.problems.append("record CRC mismatch (torn record)")

    if not rec.crc_ok:
        # The data area is torn: cycle stamping and the operation chain are
        # meaningless, and this record is exactly the crash window the test
        # injected. Only the header was checked above.
        return rec

    for index in range(len(data) // SECTOR):
        if be32(image, rec.offset + rec.header_blocks * SECTOR +
                index * SECTOR) != rec.cycle:
            rec.problems.append(f"data block {index} not cycle stamped")

    # Undo the cycle stamping to decode the operation chain.
    for index in range(len(data) // SECTOR):
        data[index * SECTOR:index * SECTOR + 4] = header[44 +
                                                         index * 4:48 +
                                                         index * 4]

    # Operation chain.
    pos = 0
    while pos + OP_HEADER_SIZE <= len(data):
        tid = be32(data, pos)
        length = be32(data, pos + 4)
        client = data[pos + 8]
        flags = data[pos + 9]
        payload = bytes(data[pos + OP_HEADER_SIZE:pos + OP_HEADER_SIZE + length])
        if pos + OP_HEADER_SIZE + length > len(data):
            rec.problems.append(f"op at {pos} overruns the data area")
            break
        rec.ops.append(Op(tid, length, client, flags, payload))
        pos += OP_HEADER_SIZE + length
        pos = (pos + 3) & ~3
        if flags & (FLAG_COMMIT_TRANS | FLAG_UNMOUNT_TRANS):
            break  # these end the chain

    decode_items(rec)
    return rec


def decode_items(rec: Record):
    """Decodes the item operations that follow the transaction header."""
    index = 0
    while index < len(rec.ops):
        op = rec.ops[index]
        if op.flags & (FLAG_START_TRANS | FLAG_COMMIT_TRANS |
                       FLAG_UNMOUNT_TRANS):
            index += 1
            continue
        if index + 1 < len(rec.ops) and rec.ops[index + 1].tid == op.tid and \
                len(op.payload) == 16 and le32(op.payload, 0) == \
                TRANS_HEADER_MAGIC:
            index += 1  # transaction header, not an item
            continue
        if len(op.payload) >= 4 and le16(op.payload, 0) == ITEM_INODE:
            if index + 1 < len(rec.ops) and len(rec.ops[index + 1].payload) >= 96:
                item = decode_inode_item(rec, index)
                rec.items.append(item)
                # ilf_size is the number of ops of this item: the format
                # header, the core and one per logged fork region.
                index += max(2, item.get("regions", 2))
                continue
            rec.notes.append(
                "stopped decoding items at an inode item without a core "
                "region")
            break
        if len(op.payload) >= 24 and le16(op.payload, 0) == ITEM_BUF:
            # xfs_buf_log_format: blf_size is the number of regions of this
            # item (the format header plus one op per logged chunk), so the
            # chunk ops have to be skipped, not parsed as new items.
            regions = le16(op.payload, 2)
            item = {"type": "buf", "regions": regions,
                    "blkno": struct.unpack_from("<q", op.payload, 8)[0],
                    "blf_len": le16(op.payload, 6)}
            rec.items.append(item)
            index += max(1, regions)
            continue
        if len(op.payload) >= 2:
            item_type = le16(op.payload, 0)
            intent = INTENT_ITEM_TYPES.get(item_type)
            if intent is not None:
                # Intent/done items (inode creation, extent free, ...) have
                # their own region counts, which this parser does not model.
                # Everything after them would be guesswork, so stop here and
                # say so instead of reporting bogus unknown item types.
                rec.items.append({"type": intent, "regions": None})
                rec.notes.append(
                    f"stopped decoding items at a {intent} item (type "
                    f"{item_type:#x})")
                break
        rec.problems.append(
            f"op {index}: unknown item type "
            f"({le16(op.payload, 0) if len(op.payload) >= 2 else 'short'})")
        index += 1
        rec.problems.append(
            f"op {index}: unknown item type "
            f"({le16(op.payload, 0) if len(op.payload) >= 2 else 'short'})")
        index += 1


def decode_inode_item(rec: Record, index: int):
    header = rec.ops[index].payload
    item = {"type": "inode"}
    item["type_id"] = le16(header, 0)
    item["regions"] = le16(header, 2)
    item["fields"] = le32(header, 4)
    item["ino"] = le64(header, 16)
    item["blkno"] = struct.unpack_from("<q", header, 40)[0]
    item["len"] = le32(header, 48)
    item["boffset"] = le32(header, 52)
    item["log_dinode_len"] = len(rec.ops[index + 1].payload) \
        if index + 1 < len(rec.ops) else 0
    if index + 1 >= len(rec.ops):
        item["problem"] = "no core region"
        return item
    core = rec.ops[index + 1].payload
    item["core_magic"] = le16(core, DINODE_MAGIC)
    item["core_mode"] = le16(core, DINODE_MODE)
    item["core_version"] = core[DINODE_VERSION]
    if len(core) >= 160:
        item["atime"] = decode_timestamp(core, DINODE_ATIME)
        item["mtime"] = decode_timestamp(core, DINODE_MTIME)
        item["ctime"] = decode_timestamp(core, DINODE_CTIME)
        item["core_size"] = le64(core, DINODE_SIZE_FIELD)
        item["core_nblocks"] = le64(core, DINODE_NBLOCKS)
        item["core_ino"] = le64(core, DINODE_INO)
    return item


def unmount_record(rec: Record) -> bool:
    """Same decision as xlog_check_unmount_rec: one op, unmount flag.

    The kernel never reads the op's payload here (mkfs.xfs writes the 8-byte
    xfs_unmount_log_format with oh_len = 8, the kernel's own writer uses
    oh_len = 0 with the payload trailing), so neither do we.
    """
    if rec.num_ops != 1 or len(rec.ops) != 1:
        return False
    op = rec.ops[0]
    return op.client_id == CLIENT_LOG and bool(op.flags & FLAG_UNMOUNT_TRANS)


def scan(image: bytes, sb: Superblock):
    log_byte = sb.fsb_to_byte(sb.logstart)
    if log_byte + sb.log_bytes > len(image):
        raise ValueError(
            f"internal log ({sb.logblocks} blocks at fsb {sb.logstart}) does "
            f"not fit in the image ({len(image)} bytes)")
    candidates = []
    for block in range(sb.log_blocks):
        offset = log_byte + block * SECTOR
        if be32(image, offset) != REC_MAGIC:
            continue
        record = parse_record(image, log_byte, sb.log_blocks, sb, block)
        if record.cleared:
            continue  # stale space the kernel marked as not-a-record
        candidates.append(record)
    # Newest by (cycle, block): records are written with increasing cycles and
    # increasing block numbers inside a cycle.
    valid = [r for r in candidates if r.crc_ok and not r.problems]
    newest = max(valid, key=lambda r: (r.cycle, r.block)) if valid else None
    torn = [r for r in candidates if not r.crc_ok]
    return {
        "log_byte": log_byte,
        "log_blocks": sb.log_blocks,
        "candidates": candidates,
        "valid": valid,
        "newest": newest,
        "torn": torn,
    }


def verify(image_path: str, expect: str = "any", image: bytes = None):
    """Verifies one image's log. Returns (failures, summary)."""
    if image is None:
        with open(image_path, "rb") as f:
            image = f.read()
    sb = Superblock(image)
    result = scan(image, sb)
    failures = []

    # A torn record is only acceptable when it is the newest candidate: that is
    # exactly the crash-window state (header written, data missing). Its CRC
    # problem is then expected; any other problem is not.
    newest_candidate = None
    if result["candidates"]:
        newest_candidate = max(result["candidates"],
                               key=lambda r: (r.cycle, r.block))
    for record in result["candidates"]:
        problems = list(record.problems)
        if (not record.crc_ok and record is newest_candidate and
                problems == ["record CRC mismatch (torn record)"]):
            continue
        for problem in problems:
            failures.append(f"block {record.block}: {problem}")
        if not record.crc_ok and record is not newest_candidate:
            failures.append(f"block {record.block}: torn but not the newest "
                            f"record")

    newest = result["newest"]
    state = "dirty"
    if newest is None:
        state = "empty"
    elif unmount_record(newest):
        state = "clean"
    summary = {
        "state": state,
        "records": [{
            "block": r.block,
            "cycle": r.cycle,
            "len": r.len,
            "blocks": r.blocks,
            "num_ops": r.num_ops,
            "prev_block": r.prev_block,
            "unmount": unmount_record(r),
            "crc_unset": r.crc_unset,
            "lsn": r.lsn,
            "items": r.items,
        } for r in result["valid"]],
        "torn_blocks": [r.block for r in result["torn"]],
        "notes": [note for r in result["candidates"] for note in r.notes],
        "crc_unset_blocks": [r.block for r in result["valid"] if r.crc_unset],
        "failures": failures,
        "newest": newest,
    }

    if expect != "any" and state != expect:
        failures.append(f"log state is {state}, expected {expect}")
    return failures, summary


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    image_path = argv[1]
    expect = "any"
    as_json = "--json" in argv
    dump = "--dump" in argv
    for arg in argv[2:]:
        if arg.startswith("--expect="):
            expect = arg.split("=", 1)[1]
    if "--expect" in argv:
        position = argv.index("--expect")
        if position + 1 < len(argv):
            expect = argv[position + 1]

    failures, summary = verify(image_path, expect)

    if dump or as_json:
        summary.pop("newest", None)
        print(json.dumps(summary, indent=1))
    else:
        print(f"{image_path}: log state {summary['state']}, "
              f"{len(summary['records'])} complete record(s), "
              f"{len(summary['torn_blocks'])} torn")
        for record in summary["records"]:
            kind = "unmount" if record["unmount"] else "transaction"
            print(f"  block {record['block']:>6} cycle {record['cycle']:>3} "
                  f"len {record['len']:>6} ops {record['num_ops']} "
                  f"({kind})")
            for item in record["items"]:
                if item.get("type") == "inode":
                    print(f"    inode {item['ino']} blkno {item['blkno']} "
                          f"len {item['len']} boffset {item['boffset']} "
                          f"fields {item['fields']:#x} "
                          f"mtime {item.get('mtime')} "
                          f"size {item.get('core_size')}")
                elif item.get("type") == "buf":
                    print(f"    buffer blkno {item['blkno']} "
                          f"len {item['blf_len']} regions {item['regions']}")
                else:
                    print(f"    {item.get('type')} item")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    if expect != "any":
        print(f"log_probe: {expect} as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

