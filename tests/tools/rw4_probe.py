#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent structural oracle for XFS images written by RW-4.

This tool parses the raw image itself (no C++ core, no mount) and re-derives
the namespace and every RW-4 invariant from the public on-disk format:

  * the whole RW-2/RW-3 structure set (allocator btrees, counters, inobt/AGI,
    directory formats) through the existing probes;
  * every allocated inode: mode/type, LOCAL and remote symlink layouts
    (xfs_dsymlink_hdr header fields, per-extent chunking, owner/blkno/uuid and
    the v5 CRC), di_nlink range and di_nextents/di_nblocks accounting;
  * a namespace walk from the root: directory ".." must name the true parent,
    a directory's di_nlink must be 2 + its subdirectory count, every
    file/symlink di_nlink must equal the number of names that resolve to it,
    and no inode may be reachable under two conflicting types or twice through
    a directory cycle;
  * allocated inodes that the namespace cannot reach must be on their AGI
    unlinked list (the open-but-unlinked target of a rename/replace), or have
    mode 0 (the reserved realtime inodes);
  * optionally, a model file describes the exact expected namespace (paths,
    inode numbers, link counts, sizes, symlink targets, exact directory
    listings and absent names), which is how the crash matrix pins down the
    two legal states of an operation.

usage:
  rw4_probe.py <image> [--model model.json] [--allow-unlinked] [--quiet]

model.json:
  {
    "entries": [
      {"path": "/a.bin", "type": "file", "ino": 68, "nlink": 1, "size": 0},
      {"path": "/sub", "type": "dir", "ino": 71, "nlink": 2},
      {"path": "/l1", "type": "symlink", "ino": 70, "target": "../a.bin"}
    ],
    "absent": ["/gone.bin"],
    "listings": {"/": ["a.bin", "l1", "sub"]},
    "allow_unreachable": false
  }
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw2_probe  # noqa: E402
import rw3_probe  # noqa: E402

SYMLINK_MAGIC = 0x58534C4D  # 'XSLM'
SYMLINK_MAXLEN = 1024
SYMLINK_HDR = 56
INODES_PER_CHUNK = 64
NULL_AGINO = 0xFFFFFFFF

TYPE_NAME = {1: "file", 2: "dir", 7: "symlink"}
FTYPE_FOR_TYPE = {"file": 1, "dir": 2, "symlink": 7}
MODE_FOR_TYPE = {"file": 0o100000, "dir": 0o040000, "symlink": 0o120000}


def fail(message):
    rw3_probe.fail(message)


def u32(buf, off):
    return rw2_probe.u32(buf, off)


def u64(buf, off):
    return rw2_probe.u64(buf, off)


def le32(buf, off):
    return rw2_probe.le32(buf, off)


def inode_type(inode):
    return (inode["mode"] >> 12) & 0xF


def symlink_target(img, geo, inode):
    """Decodes a symlink inode and validates its on-disk representation."""
    size = inode["size"]
    if size == 0 or size >= SYMLINK_MAXLEN:
        fail(f"symlink {inode['ino']}: size {size} outside [1, 1024)")
    if inode["format"] == 1:  # XFS_DINODE_FMT_LOCAL
        if size > inode["fork_size"]:
            fail(f"symlink {inode['ino']}: local target larger than the fork")
        target = inode["raw"][176:176 + size]
        if b"\0" in target:
            fail(f"symlink {inode['ino']}: NUL in the local target")
        return target
    if inode["format"] not in (2, 3):
        fail(f"symlink {inode['ino']}: bad data fork format "
             f"{inode['format']}")
    if inode["format"] == 3:
        fail(f"symlink {inode['ino']}: btree data fork is never valid")
    if inode["nextents"] < 1 or inode["nextents"] > 3:
        fail(f"symlink {inode['ino']}: {inode['nextents']} extents")
    target = b""
    expected_off = 0
    for startoff, startblock, blockcount, unwritten in inode["extents"]:
        if startoff != expected_off:
            fail(f"symlink {inode['ino']}: extent file offset {startoff} != "
                 f"{expected_off}")
        if unwritten:
            fail(f"symlink {inode['ino']}: unwritten extent")
        byte_cnt = blockcount * geo.blocksize
        dev = geo.fsb_offset(startblock)
        buf = img.read(dev, byte_cnt)
        if u32(buf, 0) != SYMLINK_MAGIC:
            fail(f"symlink {inode['ino']}: bad block magic at fsb "
                 f"{startblock}")
        if rw2_probe.xfs_crc(buf, 12) != le32(buf, 12):
            fail(f"symlink {inode['ino']}: bad block CRC at fsb {startblock}")
        sl_off = u32(buf, 4)
        sl_bytes = u32(buf, 8)
        owner = u64(buf, 32)
        blkno = u64(buf, 40)
        chunk = min(size - len(target), byte_cnt - SYMLINK_HDR)
        if sl_off != len(target):
            fail(f"symlink {inode['ino']}: header offset {sl_off} != "
                 f"{len(target)}")
        if sl_bytes != chunk:
            fail(f"symlink {inode['ino']}: header bytes {sl_bytes} != {chunk}")
        if owner != inode["ino"]:
            fail(f"symlink {inode['ino']}: header owner {owner}")
        if blkno != dev // 512:
            fail(f"symlink {inode['ino']}: header blkno {blkno} != {dev // 512}")
        if bytes(buf[16:32]) != geo.meta_uuid:
            fail(f"symlink {inode['ino']}: header uuid mismatch")
        if sl_off + sl_bytes >= SYMLINK_MAXLEN:
            fail(f"symlink {inode['ino']}: header length out of range")
        target += buf[SYMLINK_HDR:SYMLINK_HDR + chunk]
        expected_off += blockcount
    if len(target) != size:
        fail(f"symlink {inode['ino']}: decoded {len(target)} of {size} bytes")
    if b"\0" in target:
        fail(f"symlink {inode['ino']}: NUL in a remote target")
    return target


def allocated_inodes(img, geo):
    out = []
    for agno in range(geo.agcount):
        records, _, _ = rw3_probe.read_inobt(img, geo, agno)
        for startino, _, free in records:
            for i in range(INODES_PER_CHUNK):
                if not (free >> i) & 1:
                    ino = ((agno << (geo.agblklog + geo.inopblog)) |
                           (startino + i))
                    out.append(ino)
    return out


def check_inode_chunks(img, geo):
    """Every inode in an allocated chunk must be a valid inode image.

    xfs_repair reads whole inode clusters through the inode buffer verifier,
    which checks every inode in the cluster - including the free ones. A new
    chunk therefore has to be initialised like xfs_ialloc_inode_init does
    (magic, version 3, generation, NULLAGINO, di_ino, uuid, per-inode CRC);
    otherwise the cluster is rejected even though the allocated inodes look
    fine. The record alignment is repair's check from repair/scan.c: with the
    align feature, every chunk must start at a sb_inoalignmt multiple."""
    versionnum = rw2_probe.u16(geo.sb, 100)  # sb_versionnum
    inoalignmt = u32(geo.sb, 180)            # sb_inoalignmt
    has_align = (versionnum & 0x0080) != 0   # XFS_SB_VERSION_ALIGNBIT (v5)
    ino_per_block = 1 << geo.inopblog
    checked = 0
    for agno in range(geo.agcount):
        records, _, _ = rw3_probe.read_inobt(img, geo, agno)
        for startino, _, free in records:
            if startino % ino_per_block != 0:
                fail(f"AG{agno}: inobt rec startino {startino} is not "
                     f"block-aligned")
            if has_align and inoalignmt and \
                    (startino >> geo.inopblog) % inoalignmt != 0:
                fail(f"AG{agno}: inobt rec startino {startino} (block "
                     f"{startino >> geo.inopblog}) is not aligned to "
                     f"sb_inoalignmt {inoalignmt}")
            for i in range(INODES_PER_CHUNK):
                ino = ((agno << (geo.agblklog + geo.inopblog)) |
                       (startino + i))
                buf = img.read(geo.inode_offset(ino), geo.inodesize)
                if rw2_probe.u16(buf, 0) != 0x494E:
                    fail(f"inode {ino}: bad magic")
                if buf[4] != 3:
                    fail(f"inode {ino}: version {buf[4]} != 3")
                if rw2_probe.xfs_crc(buf, 100) != le32(buf, 100):
                    fail(f"inode {ino}: bad CRC")
                if u64(buf, 152) != ino:
                    fail(f"inode {ino}: di_ino mismatch")
                if bytes(buf[160:176]) != geo.meta_uuid:
                    fail(f"inode {ino}: uuid mismatch")
                if (free >> i) & 1:
                    if rw2_probe.u16(buf, 2) != 0:
                        fail(f"free inode {ino} has mode "
                             f"{rw2_probe.u16(buf, 2):o}")
                    if rw2_probe.u32(buf, 16) != 0:
                        fail(f"free inode {ino} has nlink "
                             f"{rw2_probe.u32(buf, 16)}")
                checked += 1
    print(f"  inode chunks: {checked} inode images valid")


def unlinked_chain(img, geo, agno):
    """Returns the inodes on every AGI unlinked bucket of `agno`."""
    agi_off = geo.ag_offset(agno) + 2 * geo.sectsize
    agi = img.read(agi_off, geo.sectsize)
    if u32(agi, 0) != rw3_probe.AGI_MAGIC:
        fail(f"AG{agno}: bad AGI magic")
    result = []
    for bucket in range(64):
        agino = u32(agi, 40 + bucket * 4)
        steps = 0
        while agino != NULL_AGINO:
            if steps > 1 << 20:
                fail(f"AG{agno}: unlinked bucket {bucket} does not terminate")
            ino = ((agno << (geo.agblklog + geo.inopblog)) | agino)
            inode = rw2_probe.read_inode(img, geo, ino)
            if inode["nlink"] != 0:
                fail(f"inode {ino} is on the unlinked list with nlink "
                     f"{inode['nlink']}")
            result.append(ino)
            agino = u32(inode["raw"], 96)  # di_next_unlinked
            steps += 1
    return result


def namespace_walk(img, geo):
    """Walks the namespace; returns (refs, parents, listings).

    refs:     inode -> list of (parent_ino, name_bytes, ftype)
    parents:  directory inode -> its parent inode
    listings: directory inode -> {name: (ino, ftype)}
    """
    refs = {}
    parents = {}
    listings = {}
    visited = set()

    def recurse(ino, parent):
        if ino in visited:
            fail(f"directory {ino} is reachable twice (cycle)")
        visited.add(ino)
        listing = rw3_probe.enumerate_directory(img, geo, ino)
        listings[ino] = listing
        inode = rw2_probe.read_inode(img, geo, ino)
        if inode["format"] == rw3_probe.FMT_LOCAL:
            # Shortform directories keep "." and ".." implicitly: the parent
            # inode is a header field.
            literal = inode["raw"][176:176 + inode["size"]]
            if len(literal) < 6:
                fail(f"directory {ino}: shortform header truncated")
            parent_ino = (u64(literal, 2) if literal[1] else u32(literal, 2))
        else:
            dotdot = listing.get(b"..")
            if dotdot is None:
                fail(f"directory {ino} has no '..' entry")
            parent_ino = dotdot[0]
        if parent_ino != parent:
            fail(f"directory {ino}: '..' points at {parent_ino}, the namespace "
                 f"walk says {parent}")
        parents[ino] = parent
        subdirs = 0
        for name, (child, ftype) in sorted(listing.items()):
            if name in (b".", b".."):
                continue
            refs.setdefault(child, []).append((ino, name, ftype))
            inode = rw2_probe.read_inode(img, geo, child)
            child_type = inode_type(inode)
            if ftype == 2:
                if child_type != 4:
                    fail(f"{ino}/{name.decode('utf-8', 'replace')}: dirent "
                         f"type dir but inode mode {inode['mode']:o}")
                subdirs += 1
                recurse(child, ino)
            elif ftype == 1 and child_type != 8:
                fail(f"{ino}/{name.decode('utf-8', 'replace')}: dirent type "
                     f"file but inode mode {inode['mode']:o}")
            elif ftype == 7 and child_type != 10:
                fail(f"{ino}/{name.decode('utf-8', 'replace')}: dirent type "
                     f"symlink but inode mode {inode['mode']:o}")
        inode = rw2_probe.read_inode(img, geo, ino)
        want_nlink = 2 + subdirs
        if inode["nlink"] != want_nlink:
            fail(f"directory {ino}: nlink {inode['nlink']} != 2 + {subdirs} "
                 f"subdirectories")
        if ino != geo.rootino and inode["nlink"] < 2:
            fail(f"directory {ino}: nlink below 2")

    recurse(geo.rootino, geo.rootino)
    return refs, parents, listings


def data_entsize(namelen, ftype=True):
    """xfs_dir2_data_entsize: the data-space size of a directory entry."""
    n = 8 + 1 + namelen + 2 + (1 if ftype else 0)
    return (n + 7) & ~7


def check_shortform_offsets(img, geo, listings):
    """xfs_dir2_sf_verify's offset rule for every shortform directory.

    The stored offset is the entry's *data-space* position: the first entry
    starts at data_first_offset (after the data block header and the implicit
    "." and ".." entries), and every later entry follows the previous one by
    at least xfs_dir2_data_entsize. A smaller offset means the entry overlaps
    its predecessor, which the kernel and xfs_repair reject even though the
    names themselves look fine."""
    first = 64 + data_entsize(1) + data_entsize(2)
    checked = 0
    for ino in listings:
        inode = rw2_probe.read_inode(img, geo, ino)
        if inode["format"] != rw3_probe.FMT_LOCAL:
            continue
        literal = inode["raw"][176:176 + inode["size"]]
        count = literal[0]
        i8 = literal[1] != 0
        off = 10 if i8 else 6
        ino_len = 8 if i8 else 4
        need = first
        for _ in range(count):
            if off + 3 > len(literal):
                fail(f"directory {ino}: shortform entry truncated")
            namelen = literal[off]
            if namelen == 0:
                fail(f"directory {ino}: empty shortform name")
            eoffset = rw2_probe.u16(literal, off + 1)
            if eoffset < need:
                name = literal[off + 3:off + 3 + namelen]
                fail(f"directory {ino}: shortform entry "
                     f"{name.decode('utf-8', 'replace')!r} at offset {eoffset}"
                     f" overlaps the previous entry (needs >= {need})")
            need = eoffset + data_entsize(namelen)
            off += 3 + namelen + 1 + ino_len
        if off != len(literal):
            fail(f"directory {ino}: shortform entries cover {off} bytes, the "
                 f"fork has {len(literal)}")
        checked += 1
    print(f"  shortform dirs: {checked} offset chains valid")


def check_namespace(img, geo, allow_unlinked):
    refs, parents, listings = namespace_walk(img, geo)
    check_shortform_offsets(img, geo, listings)
    allocated = allocated_inodes(img, geo)
    # The realtime bitmap/summary inodes are allocated but have no name.
    rt_inodes = {u64(geo.sb, 64), u64(geo.sb, 72)}
    unlinked = set()
    if allow_unlinked:
        for agno in range(geo.agcount):
            unlinked.update(unlinked_chain(img, geo, agno))
    for ino in allocated:
        inode = rw2_probe.read_inode(img, geo, ino)
        if ino not in refs:
            if inode["mode"] == 0:
                continue  # never-used slots
            if ino == geo.rootino or ino in rt_inodes:
                continue
            if ino in unlinked:
                if inode["nlink"] != 0:
                    fail(f"unlinked inode {ino} has nlink {inode['nlink']}")
                continue
            fail(f"inode {ino} (mode {inode['mode']:o}) is allocated but not "
                 f"reachable from the root")
            continue
        if inode_type(inode) == 4:
            continue  # a directory's nlink is 2 + its subdirectories (above)
        want = len(refs[ino])
        if inode["nlink"] != want:
            names = ", ".join(
                f"{p}/{n.decode('utf-8', 'replace')}" for p, n, _ in refs[ino])
            fail(f"inode {ino}: di_nlink {inode['nlink']} but {want} names "
                 f"resolve to it ({names})")
        seen_types = {ftype for _, _, ftype in refs[ino]}
        if len(seen_types) > 1:
            fail(f"inode {ino}: reachable with conflicting dirent types "
                 f"{sorted(seen_types)}")
        if inode_type(inode) == 10:
            symlink_target(img, geo, inode)
        elif inode_type(inode) == 4:
            if inode["size"] > 0 and inode["format"] != 1 and \
                    inode["nextents"] == 0 and inode["nblocks"] == 0:
                fail(f"directory {ino}: extents format without extents")
    return refs, parents, listings


def directory_format(img, geo, inode):
    """The on-disk directory format: shortform, block, leaf or node."""
    if inode["format"] == rw3_probe.FMT_LOCAL:
        return "shortform"
    blocklog = geo.blocklog
    by_off = {}
    for startoff, startblock, blockcount, _ in inode["extents"]:
        by_off[startoff] = (startblock, blockcount)

    def read_file_block(dablk):
        for startoff, (startblock, blockcount) in by_off.items():
            if startoff <= dablk < startoff + blockcount:
                fsb = startblock + (dablk - startoff)
                return img.read(geo.fsb_offset(fsb), geo.blocksize)
        return None

    data0 = read_file_block(0)
    if data0 is None:
        return "?"
    if u32(data0, 0) == rw3_probe.DIR3_BLOCK_MAGIC:
        return "block"
    if u32(data0, 0) != rw3_probe.DIR3_DATA_MAGIC:
        return "?"
    leaf_space = rw3_probe.SPACE_SIZE >> blocklog
    root = read_file_block(leaf_space)
    if root is None:
        return "?"
    magic = rw2_probe.u16(root, 8)
    if magic == rw3_probe.DIR3_LEAF1_MAGIC:
        return "leaf"
    return "node"


def check_model(img, geo, listings, rootino, model, refs):
    for entry in model.get("entries", []):
        path = entry["path"]
        parts = [p for p in path.split("/") if p]
        ino = rootino
        ftype = 2
        for part in parts:
            listing = listings.get(ino)
            if listing is None:
                fail(f"model: {path}: {part} is not a directory")
            found = listing.get(part.encode())
            if found is None:
                fail(f"model: {path} is missing (stopped at {part})")
            ino, ftype = found
        want_type = entry.get("type")
        if want_type is not None and ftype != FTYPE_FOR_TYPE[want_type]:
            fail(f"model: {path} has ftype "
                 f"{rw3_probe.FTYPE_NAME.get(ftype)}, expected {want_type}")
        inode = rw2_probe.read_inode(img, geo, ino)
        if "ino" in entry and entry["ino"] != ino:
            fail(f"model: {path} resolves to inode {ino}, expected "
                 f"{entry['ino']}")
        if "nlink" in entry and entry["nlink"] != inode["nlink"]:
            fail(f"model: {path} (inode {ino}) nlink {inode['nlink']} != "
                 f"{entry['nlink']}")
        if "size" in entry and entry["size"] != inode["size"]:
            fail(f"model: {path} (inode {ino}) size {inode['size']} != "
                 f"{entry['size']}")
        if "format" in entry:
            want_format = entry["format"]
            if want_type == "dir" or inode_type(inode) == 4:
                got_format = directory_format(img, geo, inode)
            else:
                got_format = {1: "local", 2: "extents", 3: "btree"}.get(
                    inode["format"], f"?{inode['format']}")
            if got_format != want_format:
                fail(f"model: {path} (inode {ino}) format {got_format} != "
                     f"{want_format}")
        if want_type == "symlink":
            target = symlink_target(img, geo, inode)
            if target != entry["target"].encode():
                fail(f"model: {path} target {target!r} != "
                     f"{entry['target']!r}")
    for path in model.get("absent", []):
        parts = [p for p in path.split("/") if p]
        ino = rootino
        missing = False
        for i, part in enumerate(parts):
            listing = listings.get(ino)
            if listing is None:
                missing = True
                break
            found = listing.get(part.encode())
            if found is None:
                missing = True
                break
            ino = found[0]
        if not missing:
            fail(f"model: {path} must not exist")
    for path, names in model.get("listings", {}).items():
        parts = [p for p in path.split("/") if p]
        ino = rootino
        for part in parts:
            found = listings.get(ino, {}).get(part.encode())
            if found is None:
                fail(f"model: listing {path} does not exist")
            ino = found[0]
        listing = listings.get(ino)
        if listing is None:
            fail(f"model: listing {path}: not a directory")
        real = sorted(n.decode("utf-8", "replace") for n in listing
                      if n not in (b".", b".."))
        if real != sorted(names):
            fail(f"model: listing {path} = {real}, expected {sorted(names)}")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--model", help="JSON namespace expectation")
    parser.add_argument("--allow-unlinked", action="store_true",
                        help="allocated unreachable inodes may be on the AGI "
                             "unlinked list (open-but-unlinked state)")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv[1:])

    img = rw2_probe.Image(args.image)
    geo = rw2_probe.check_image(img, verbose=not args.quiet)
    rw3_probe.check_inobt(img, geo)
    check_inode_chunks(img, geo)
    refs, parents, listings = check_namespace(img, geo, args.allow_unlinked)
    if args.model:
        with open(args.model, "r", encoding="utf-8-sig") as f:
            model = json.load(f)
        if model.get("allow_unreachable"):
            pass
        check_model(img, geo, listings, geo.rootino, model, refs)
    reachable = len(refs)
    print(f"OK: RW-4 namespace consistent ({reachable} inodes reachable, "
          f"link counts, symlinks, AGI unlinked all check out)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
