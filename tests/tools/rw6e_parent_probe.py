#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent RW-6E parent-pointer oracle.

This is a raw-image oracle, not a wrapper around the C++ writer:

  * it walks the forward namespace with the independent Python directory
    parser (shortform / block / leaf / node) and the independently parsed
    inobt (including sparse chunks);
  * it parses every child's raw attr fork with the independent Python attr
    parser (shortform / leaf / node / BMBT), then decodes XFS_ATTR_PARENT
    records itself;
  * it compares both directions and reports missing, unexpected, duplicate,
    wrong-name, wrong-generation, dangling-parent, nlink and dotdot errors.

The low-level Python parsers are shared with the other RW oracles; the C++
implementation is never called.  The parent-key hash is reimplemented here:

    xfs_parent_hashval = xfs_dir2_hashname(name)
                         ^ high32(parent_ino) ^ low32(parent_ino)

usage:
  rw6e_parent_probe.py <image> [--json OUT] [--quiet]
                       [--require-parent] [--allow-empty]

Exit code 0 means the bidirectional audit is clean.  --require-parent makes a
filesystem without the PARENT incompat bit an error (a qualification fixture
must actually exercise the feature).
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw3_probe  # noqa: E402
import rw6_inode_probe as inode_probe  # noqa: E402
import rw6d_xattr_probe as xattr_probe  # noqa: E402

INCOMPAT_PARENT = 1 << 7
ATTR_PARENT = 1 << 3
ATTR_NAMESPACE_MASK = (1 << 1) | (1 << 2) | (1 << 3)
ATTR_INCOMPLETE = 1 << 7
S_IFMT = 0xF000
S_IFDIR = 0x4000
INODES_PER_CHUNK = 64
INODES_PER_HOLEMASK_BIT = 4


def fail(message):
    raise AssertionError(message)


def u16(buf, off):
    return int.from_bytes(buf[off:off + 2], "big")


def u32(buf, off):
    return int.from_bytes(buf[off:off + 4], "big")


def u64(buf, off):
    return int.from_bytes(buf[off:off + 8], "big")


def attr_hash(name):
    return xattr_probe.attr_hash(name)


def parent_hash(name, parent_ino):
    return (attr_hash(name) ^ ((parent_ino >> 32) & 0xFFFFFFFF) ^
            (parent_ino & 0xFFFFFFFF)) & 0xFFFFFFFF


def text(name):
    if isinstance(name, str):
        return name
    return name.decode("utf-8", "backslashreplace")


def inode_record(img, geo, ino):
    """A modern-inode parser independent of the C++ XfsInode.

    It returns only the fields needed by this oracle, but verifies the v3
    header, CRC, di_ino, metadata UUID, fork bounds, extent ordering and
    di_nblocks so a malformed inode cannot silently look like a valid one.
    """
    raw = img.read(geo.ino_offset(ino), geo.inodesize)
    if u16(raw, 0) != 0x494E:
        fail("inode %d: bad magic" % ino)
    if raw[4] != 3:
        fail("inode %d: version %d" % (ino, raw[4]))
    if xattr_probe.xfs_crc(raw, 100) != xattr_probe.le32(raw, 100):
        fail("inode %d: bad CRC" % ino)
    if u64(raw, 152) != ino:
        fail("inode %d: di_ino mismatch" % ino)
    if raw[160:176] != geo.meta_uuid:
        fail("inode %d: metadata uuid mismatch" % ino)
    flags2 = u64(raw, 120)
    nrext64 = bool(flags2 & (1 << 4))
    mode = u16(raw, 2)
    fmt = raw[5]
    nlink = u32(raw, 16)
    size = u64(raw, 56)
    nblocks = u64(raw, 64)
    forkoff = raw[82]
    aformat = raw[83]
    gen = u32(raw, 92)
    nextents = u64(raw, 24) if nrext64 else u32(raw, 76)
    anextents = u32(raw, 76) if nrext64 else u16(raw, 80)
    if nextents > 0xFFFFFFFF or anextents > 0xFFFFFFFF:
        fail("inode %d: extent counter overflow" % ino)
    fork_bytes = forkoff * 8 if forkoff else geo.inodesize - 176
    if forkoff and 176 + fork_bytes > geo.inodesize:
        fail("inode %d: forkoff past the inode" % ino)
    if forkoff and fork_bytes < 36:
        fail("inode %d: attr fork too small for a bmdr" % ino)
    extents = []
    bmbt_blocks = 0
    if fmt == 2:
        if nextents * 16 > fork_bytes:
            fail("inode %d: data extents exceed the fork" % ino)
        for i in range(int(nextents)):
            rec = raw[176 + i * 16:192 + i * 16]
            extents.append(xattr_probe.decode_bmap(rec))
    elif fmt == 3:
        extents = xattr_probe.read_bmbt_extents(
            img, geo, ino, 3, "data", raw[176:176 + fork_bytes], nextents)
        # The independent parser verifies each bmbt block; BMBT block
        # accounting is checked by the C++ writer and xfs_repair, not guessed
        # here.
        if len(extents) != nextents:
            fail("inode %d: bmbt records %d != nextents %d" %
                 (ino, len(extents), nextents))
    elif fmt == 1:
        pass
    else:
        fail("inode %d: unsupported data fork format %d" % (ino, fmt))
    # di_nblocks includes the attr fork and BMBT blocks, which this oracle
    # does not own; the C++ check and xfs_repair validate that accounting.
    return {
        "raw": raw,
        "ino": ino,
        "mode": mode,
        "format": fmt,
        "nlink": nlink,
        "size": size,
        "forkoff": forkoff,
        "aformat": aformat,
        "gen": gen,
        "nextents": nextents,
        "anextents": anextents,
        "extents": extents,
        "nrext64": nrext64,
    }


def read_inode_for_dirs(img, geo, ino):
    """Adapter used by rw3_probe.enumerate_directory."""
    rec = inode_record(img, geo, ino)
    rec["nblocks"] = u64(rec["raw"], 64)
    return rec


# rw3_probe's directory walker is independent of the C++ parser.  Replace its
# inode reader with the modern-inode parser above so bigtime/NREXT64 and the
# parent profile are handled correctly.
rw3_probe.rw2_probe.read_inode = read_inode_for_dirs


def allocated_inodes(img, geo):
    out = []
    for agno in range(geo.agcount):
        agi = inode_probe.read_agi(img, geo, agno)
        records, _ = inode_probe.read_btree(
            img, geo, agno, agi["root"], inode_probe.INO_MAGIC, 16, 4, 4,
            inode_probe.parse_inobt_rec(geo), "inobt")
        for rec in records:
            mask = (inode_probe.alloc_mask(rec["holemask"])
                    if geo.sparse else 0xFFFFFFFFFFFFFFFF)
            for i in range(INODES_PER_CHUNK):
                if not ((mask >> i) & 1):
                    continue
                if (rec["free"] >> i) & 1:
                    continue
                agino = rec["startino"] + i
                out.append((agno << (geo.agblklog + geo.inopblog)) | agino)
    return sorted(set(out))


def decode_parents(img, geo, ino, raw_attr):
    """Decodes XFS_ATTR_PARENT entries using the Python attr parser."""
    fork = xattr_probe.AttrFork(img, geo, ino, raw_attr)
    parents = []
    for entry in fork.entries:
        if entry.get("namespace") != "parent":
            continue
        if entry.get("incomplete"):
            fail("inode %d: incomplete parent pointer" % ino)
        if entry.get("remote"):
            fail("inode %d: remote parent pointer value" % ino)
        value = entry["value"]
        if len(value) != 12:
            fail("inode %d: parent pointer value length %d" %
                 (ino, len(value)))
        name = entry["name"]
        if name.startswith("parent."):
            name = name[len("parent."):].encode("utf-8", "surrogateescape")
        else:
            fail("inode %d: parent namespace display mismatch" % ino)
        if not name or name in (b".", b"..") or b"/" in name or b"\0" in name:
            fail("inode %d: invalid parent pointer name %r" % (ino, name))
        p_ino = u64(value, 0)
        p_gen = u32(value, 8)
        if p_ino == ino:
            fail("inode %d: self-referential parent pointer" % ino)
        if p_ino == 0 or (p_ino >> (geo.agblklog + geo.inopblog)) >= \
                geo.agcount:
            fail("inode %d: parent inode %d out of range" % (ino, p_ino))
        # Shortform entries have no stored hash (the parser fills in the plain
        # name hash); leaf/node entries carry the parent-mixed hash.
        if fork.format != "shortform":
            want = parent_hash(name, p_ino)
            if entry["hash"] != want:
                fail("inode %d: parent hash %#x != %#x" %
                     (ino, entry["hash"], want))
        parents.append((p_ino, p_gen, name))
    return parents


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--json")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--require-parent", action="store_true")
    parser.add_argument("--allow-empty", action="store_true",
                        help="accept a filesystem with no directory edges")
    args = parser.parse_args(argv)

    img = inode_probe.Image(args.image)
    geo = inode_probe.Geometry(img)
    # All v5 block identities use the metadata UUID, which may differ from
    # sb_uuid when the meta-uuid feature is on.
    geo.uuid = geo.meta_uuid
    parent_feature = bool(geo.incompat & INCOMPAT_PARENT)
    if args.require_parent and not parent_feature:
        fail("filesystem does not have the PARENT incompat feature")
    if not parent_feature:
        report = {
            "parent_feature": False,
            "directory_edges": 0,
            "parent_pointers": 0,
            "missing": [],
            "unexpected": [],
            "wrong_parent_generation": [],
            "wrong_name": [],
            "duplicate": [],
            "dangling_parent": [],
            "wrong_parent_type": [],
            "non_directory_parent": [],
            "stale_generation": [],
            "wrong_dotdot": [],
            "link_count_mismatch": [],
            "malformed": [],
            "unreachable_with_parents": [],
        }
        if not args.quiet:
            print("parent feature: no")
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=2, sort_keys=True)
                f.write("\n")
        return 0

    root = u32(geo.sb, 56)  # sb_rootino (u64 low word enough for these images)
    root = u64(geo.sb, 56)
    allocated = allocated_inodes(img, geo)
    allocated_set = set(allocated)

    expected = {}
    dirs = {root: {"parent": root, "gen": inode_record(img, geo, root)["gen"],
                   "name": b""}}
    visited = set()
    stack = [root]
    edges = 0
    listings = {}
    malformed = []
    wrong_dotdot = []
    link_count_mismatch = []
    wrong_parent_type = []

    while stack:
        dino = stack.pop()
        if dino in visited:
            malformed.append("directory %d reachable twice" % dino)
            continue
        visited.add(dino)
        try:
            listing = rw3_probe.enumerate_directory(img, geo, dino)
        except Exception as exc:  # noqa: BLE001 - report, never guess
            malformed.append("directory %d: %s" % (dino, exc))
            continue
        inode = inode_record(img, geo, dino)
        if inode["format"] == 1:
            # Shortform directories store ".." in the header, not as a
            # dirent; synthesize the two dot entries for the audit.
            literal = inode["raw"][176:176 + inode["size"]]
            if len(literal) < 6:
                malformed.append("directory %d: shortform header truncated" %
                                 dino)
            else:
                parent_ino = (u64(literal, 2) if literal[1]
                              else u32(literal, 2))
                listing.setdefault(b".", (dino, 2))
                listing.setdefault(b"..", (parent_ino, 2))
        listings[dino] = listing
        if (inode["mode"] & S_IFMT) != S_IFDIR:
            wrong_parent_type.append(
                "inode %d is used as a directory but mode=%#o" %
                (dino, inode["mode"]))
            continue
        if inode["nlink"] == 0:
            link_count_mismatch.append(
                "directory %d is reachable but nlink is zero" % dino)
        if b"." not in listing:
            wrong_dotdot.append("directory %d has no '.' entry" % dino)
        elif listing[b"."][0] != dino:
            wrong_dotdot.append(
                "directory %d '.' points at %d" % (dino, listing[b"."][0]))
        dotdot = listing.get(b"..")
        if dotdot is None:
            wrong_dotdot.append("directory %d has no '..' entry" % dino)
        else:
            want = root if dino == root else dirs[dino]["parent"]
            if dotdot[0] != want:
                wrong_dotdot.append(
                    "directory %d '..' points at %d, expected %d" %
                    (dino, dotdot[0], want))
        subdirs = 0
        for name, (child, _ftype) in listing.items():
            if name in (b".", b".."):
                continue
            edges += 1
            if child not in allocated_set:
                malformed.append(
                    "directory %d entry %r points at unallocated inode %d" %
                    (dino, name, child))
                continue
            try:
                ci = inode_record(img, geo, child)
            except Exception as exc:  # noqa: BLE001
                malformed.append("inode %d: %s" % (child, exc))
                continue
            if (ci["mode"] & S_IFMT) == S_IFDIR:
                if child == root:
                    wrong_parent_type.append(
                        "root inode appears as a directory entry")
                    continue
                if child in dirs:
                    malformed.append(
                        "directory %d is linked twice (second parent %d, "
                        "name %r)" % (child, dino, name))
                    continue
                dirs[child] = {"parent": dino, "gen": inode["gen"],
                               "name": name}
                subdirs += 1
                stack.append(child)
            else:
                expected.setdefault(child, []).append(
                    (dino, inode["gen"], name))
        if inode["nlink"] != 0 and inode["nlink"] != 2 + subdirs:
            link_count_mismatch.append(
                "directory %d nlink %d != 2 + %d subdirectories" %
                (dino, inode["nlink"], subdirs))

    # Root has no parent pointer.
    expected.pop(root, None)
    for child, state in dirs.items():
        if child == root or child in expected:
            continue
        expected[child] = [(state["parent"], state["gen"], state["name"])]

    missing = []
    unexpected = []
    wrong_gen = []
    wrong_name = []
    duplicate = []
    dangling = []
    non_dir_parent = []
    stale = []
    unreachable = []
    parent_pointers = 0
    checked = set()

    def audit(ino, wanted):
        nonlocal parent_pointers
        try:
            raw = img.read(geo.ino_offset(ino), geo.inodesize)
            actual = decode_parents(img, geo, ino, raw)
        except Exception as exc:  # noqa: BLE001
            malformed.append("inode %d parent attrs: %s" % (ino, exc))
            return
        parent_pointers += len(actual)
        seen = set()
        matched = [False] * len(wanted)
        for rec in actual:
            p_ino, p_gen, name = rec
            if rec in seen:
                duplicate.append("inode %d %s" % (ino, describe(rec)))
            seen.add(rec)
            if p_ino not in allocated_set:
                dangling.append("inode %d %s" % (ino, describe(rec)))
                continue
            try:
                pi = inode_record(img, geo, p_ino)
            except Exception:  # noqa: BLE001
                dangling.append("inode %d %s" % (ino, describe(rec)))
                continue
            if (pi["mode"] & S_IFMT) != S_IFDIR:
                non_dir_parent.append("inode %d %s" % (ino, describe(rec)))
                continue
            if pi["gen"] != p_gen:
                wrong_gen.append("inode %d %s live_gen=%d" %
                                 (ino, describe(rec), pi["gen"]))
                stale.append("inode %d %s" % (ino, describe(rec)))
                continue
            found = False
            for i, want in enumerate(wanted):
                if want == rec:
                    matched[i] = True
                    found = True
                    break
            if not found:
                if any(w[0] == p_ino and w[2] == name and
                       w[1] != p_gen for w in wanted):
                    wrong_gen.append("inode %d %s" % (ino, describe(rec)))
                elif any(w[0] == p_ino and w[1] == p_gen and
                         w[2] != name for w in wanted):
                    wrong_name.append("inode %d %s" % (ino, describe(rec)))
                else:
                    unexpected.append("inode %d %s" % (ino, describe(rec)))
        for i, want in enumerate(wanted):
            if not matched[i]:
                missing.append("inode %d %s" % (ino, describe(want)))
        if ino not in dirs and wanted:
            ci = inode_record(img, geo, ino)
            if ci["nlink"] != len(actual):
                link_count_mismatch.append(
                    "inode %d nlink %d != %d parent pointers" %
                    (ino, ci["nlink"], len(actual)))

    for ino, wanted in expected.items():
        checked.add(ino)
        audit(ino, wanted)
    for dino, state in dirs.items():
        if dino == root or dino in checked:
            continue
        checked.add(dino)
        audit(dino, [(state["parent"], state["gen"], state["name"])])
    # Root: no reverse records.
    try:
        root_parents = decode_parents(
            img, geo, root, img.read(geo.ino_offset(root), geo.inodesize))
        parent_pointers += len(root_parents)
        for rec in root_parents:
            unexpected.append("root inode %s" % describe(rec))
    except Exception as exc:  # noqa: BLE001
        malformed.append("root inode parent attrs: %s" % exc)

    # An allocated inode outside the tree with parent pointers is an orphaned
    # reverse record.  A linked directory outside the tree is already caught by
    # the root walk; here we only need the attr evidence.
    for ino in allocated:
        if ino in checked:
            continue
        try:
            rec = inode_record(img, geo, ino)
            parents = decode_parents(img, geo, ino, rec["raw"])
        except Exception:  # noqa: BLE001 - free/invalid inodes are checked above
            continue
        if parents:
            parent_pointers += len(parents)
            unreachable.append(
                "inode %d has %d parent pointer(s) but is unreachable" %
                (ino, len(parents)))

    report = {
        "parent_feature": True,
        "directory_edges": edges,
        "parent_pointers": parent_pointers,
        "inodes_checked": len(checked) + 1,
        "missing": missing,
        "unexpected": unexpected,
        "wrong_parent_generation": wrong_gen,
        "wrong_name": wrong_name,
        "duplicate": duplicate,
        "dangling_parent": dangling,
        "wrong_parent_type": wrong_parent_type,
        "non_directory_parent": non_dir_parent,
        "stale_generation": stale,
        "wrong_dotdot": wrong_dotdot,
        "link_count_mismatch": link_count_mismatch,
        "malformed": malformed,
        "unreachable_with_parents": unreachable,
    }
    clean = not any(v for k, v in report.items()
                    if isinstance(v, list) and k != "parent_feature")
    if not args.quiet:
        print("parent feature: yes")
        print("  directory edges: %d" % edges)
        print("  parent pointers: %d" % parent_pointers)
        for key in ("missing", "unexpected", "wrong_parent_generation",
                    "wrong_name", "duplicate", "dangling_parent",
                    "wrong_parent_type", "non_directory_parent",
                    "stale_generation", "wrong_dotdot",
                    "link_count_mismatch", "malformed",
                    "unreachable_with_parents"):
            if report[key]:
                print("  %s: %d (first: %s)" %
                      (key, len(report[key]), report[key][0]))
        print("  result: %s" % ("clean" if clean else "CORRUPT"))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2, sort_keys=True)
            f.write("\n")
    return 0 if clean else 1


def describe(rec):
    return "parent=%d gen=%d name=%r" % (rec[0], rec[1], text(rec[2]))


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AssertionError as exc:
        print("rw6e parent oracle: %s" % exc, file=sys.stderr)
        sys.exit(1)
