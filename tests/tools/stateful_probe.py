#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent semantic oracle for FUZZ-2 stateful images.

This tool reads the raw image with the existing independent Python parsers
(no C++ code) and compares the result with a FUZZ-2 manifest emitted by
``stateful_fuzz``.  It verifies:

  * paths, object types, link counts, sizes, symlink targets;
  * logical file content (holes read as zeros) through an FNV-1a hash;
  * user/trusted/security attributes (the private parent namespace is ignored);
  * exact directory listings and hard-link identity (same model object must
    resolve to one on-disk inode);
  * the inobt/AGI/AGF/free-space structures through the existing oracles.

The manifest may contain one exact state (normal run) or two old/new states
(crash recovery).  A ``state_mode == "range"`` manifest is the weaker data
contract: metadata and the allowed size set are checked, while byte ranges are
validated by the C++ runner that has the before/after buffers.

usage:
  stateful_probe.py <image> --manifest <manifest.json> [--quiet]
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rw2_probe  # noqa: E402
import rw3_probe  # noqa: E402
import rw6_inode_probe  # noqa: E402
import rw6d_xattr_probe  # noqa: E402
import rw6e_parent_probe  # noqa: E402
import rw4_probe  # noqa: E402
import stateful_range  # noqa: E402

TYPE_BY_FT = {1: "file", 2: "dir", 7: "symlink"}
FT_BY_TYPE = {"file": 1, "dir": 2, "symlink": 7}
MODE_BY_TYPE = {"file": 0o100000, "dir": 0o040000, "symlink": 0o120000}
S_IFMT = 0xF000


class Geometry(rw2_probe.Geometry):
    """rw2 geometry with the modern inode parser's alias."""

    def ino_offset(self, ino):
        return self.inode_offset(ino)


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % value


def fail(message):
    raise SystemExit("stateful probe: FAIL: %s" % message)


def read_data(img, geo, inode):
    if inode["format"] == rw3_probe.FMT_LOCAL:
        return bytes(inode["raw"][176:176 + inode["size"]])
    out = bytearray(inode["size"])
    for startoff, startblock, blockcount, unwritten in inode["extents"]:
        first = startoff << geo.blocklog
        if first >= inode["size"]:
            break
        length = min(blockcount << geo.blocklog, inode["size"] - first)
        if unwritten:
            continue
        out[first:first + length] = img.read(geo.fsb_offset(startblock),
                                             length)
    return bytes(out)


def inode_type(record):
    bits = record["mode"] & S_IFMT
    if bits == 0o100000:
        return "file"
    if bits == 0o040000:
        return "dir"
    if bits == 0o120000:
        return "symlink"
    return "other"


def attrs_for(img, geo, ino, record):
    fork = rw6d_xattr_probe.AttrFork(img, geo, ino, record["raw"])
    result = {}
    for entry in fork.entries:
        if entry.get("namespace") == "parent":
            continue
        name = entry["name"]
        if entry.get("incomplete"):
            fail("inode %d has an incomplete attribute %s" % (ino, name))
        result[name] = {
            "length": len(entry["value"]),
            "value_fnv": fnv1a(entry["value"]),
        }
    return result


def walk(img, geo):
    listings = {}
    paths = {}
    records = {}
    visited = set()

    def recurse(ino, path):
        if ino in visited:
            fail("directory inode %d is reachable twice" % ino)
        visited.add(ino)
        try:
            listing = rw3_probe.enumerate_directory(img, geo, ino)
        except Exception as exc:  # noqa: BLE001
            fail("directory inode %d: %s" % (ino, exc))
        names = []
        for name, (child, ftype) in sorted(listing.items()):
            if name in (b".", b".."):
                continue
            display = name.decode("utf-8", "surrogateescape")
            names.append(display)
            child_path = path + display if path.endswith("/") else path + "/" + display
            if child not in records:
                records[child] = rw6e_parent_probe.inode_record(img, geo, child)
                records[child]["fork_size"] = (
                    records[child]["forkoff"] * 8
                    if records[child]["forkoff"] else geo.inodesize - 176)
            record = records[child]
            actual_type = inode_type(record)
            expected_type = TYPE_BY_FT.get(ftype)
            if expected_type and actual_type != expected_type:
                fail("%s: dirent type %s but inode type %s" %
                     (child_path, expected_type, actual_type))
            paths.setdefault(child, []).append(child_path)
            if actual_type == "dir":
                recurse(child, child_path)
            elif actual_type not in ("file", "symlink"):
                fail("%s: unsupported inode type %s" %
                     (child_path, actual_type))
        listings[path] = sorted(names)

    root = geo.rootino
    records[root] = rw6e_parent_probe.inode_record(img, geo, root)
    records[root]["fork_size"] = (
        records[root]["forkoff"] * 8
        if records[root]["forkoff"] else geo.inodesize - 176)
    paths[root] = ["/"]
    recurse(root, "/")
    return listings, paths, records


def check_entry(img, geo, entry, path_set, records, range_target=None,
                range_contract=None, range_before=None, range_after=None):
    want_type = entry["type"]
    nlink = entry["nlink"]
    size = entry["size"]
    content = entry["content_fnv"]
    xattrs = {a["name"]: a for a in entry.get("xattrs", [])}
    inos = []
    for path in entry["paths"]:
        if path not in path_set:
            fail("model path %s is missing" % path)
    # Resolve via the directory listings: paths map is authoritative.
    resolved = []
    for path in entry["paths"]:
        parts = [p for p in path.split("/") if p]
        ino = geo.rootino
        for part in parts:
            listing = rw3_probe.enumerate_directory(img, geo, ino)
            found = listing.get(part.encode())
            if found is None:
                fail("model path %s is missing" % path)
            ino = found[0]
        resolved.append(ino)
    if not resolved:
        fail("model entry %d has no path" % entry["id"])
    first = resolved[0]
    if any(ino != first for ino in resolved):
        fail("hard-link paths %s resolve to different inodes %s" %
             (entry["paths"], resolved))
    record = records.get(first)
    if record is None:
        record = rw6e_parent_probe.inode_record(img, geo, first)
        record["fork_size"] = (record["forkoff"] * 8
                               if record["forkoff"] else geo.inodesize - 176)
    if inode_type(record) != want_type:
        fail("%s: type %s != %s" %
             (entry["paths"][0], inode_type(record), want_type))
    if record["nlink"] != nlink:
        fail("%s: nlink %d != %d" %
             (entry["paths"][0], record["nlink"], nlink))
    if want_type != "dir":
        if range_contract is not None and entry["id"] == range_target:
            allowed_sizes = {int(range_contract["before_size"]),
                             int(range_contract["after_size"])}
            if record["size"] not in allowed_sizes:
                fail("%s: size %d not in allowed set %s" %
                     (entry["paths"][0], record["size"],
                      sorted(allowed_sizes)))
        elif record["size"] != size:
            fail("%s: size %d != %d" %
                 (entry["paths"][0], record["size"], size))
    if want_type == "file":
        data = read_data(img, geo, record)
        if range_contract is not None and entry["id"] == range_target:
            error = stateful_range.verify_range(
                data, range_contract, range_before, range_after)
            if error:
                fail("%s: %s" % (entry["paths"][0], error))
        elif len(data) != size or fnv1a(data) != content:
            fail("%s: content hash differs" % entry["paths"][0])
    elif want_type == "symlink":
        target = rw4_probe.symlink_target(img, geo, record)
        if fnv1a(target) != content:
            fail("%s: symlink target hash differs" % entry["paths"][0])
        if target.decode("utf-8", "surrogateescape") != entry.get("target", ""):
            fail("%s: symlink target differs" % entry["paths"][0])
    actual_attrs = attrs_for(img, geo, first, record)
    if set(actual_attrs) != set(xattrs):
        fail("%s: attribute names differ (%s != %s)" %
             (entry["paths"][0], sorted(actual_attrs), sorted(xattrs)))
    for name, wanted in xattrs.items():
        got = actual_attrs[name]
        if got["length"] != wanted["length"] or \
                got["value_fnv"] != wanted["value_fnv"]:
            fail("%s: attribute %s value differs" %
                 (entry["paths"][0], name))
    return first


def check_state(img, geo, state, range_mode, allowed_sizes=None,
                range_target=None, range_contract=None, range_before=None,
                range_after=None):
    listings, paths, records = walk(img, geo)
    path_set = {path for values in paths.values() for path in values}
    for path, names in state["listings"].items():
        if path not in listings:
            fail("model directory %s is missing" % path)
        if listings[path] != sorted(names):
            fail("directory %s = %s, expected %s" %
                 (path, listings[path], sorted(names)))
    for entry in state["entries"]:
        check_entry(img, geo, entry, path_set, records, range_target,
                    range_contract, range_before, range_after)
    return len(state["entries"])


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv[1:])

    with open(args.manifest, "r", encoding="utf-8-sig") as f:
        manifest = json.load(f)
    if manifest.get("format") != "xfswin-stateful-v1":
        fail("unknown manifest format")

    # Structural + allocator oracles first.  They are independent of the
    # semantic model and must always pass.
    rw6_inode_probe.check_image(args.image, quiet=True,
                                allow_empty_finobt=False)
    img = rw2_probe.Image(args.image)
    geo = Geometry(img)
    rw2_probe.check_image(img, verbose=False)

    # The manifest's exact states are alternatives for a crash image.
    states = manifest.get("states", [])
    if not states:
        fail("manifest has no states")
    range_mode = manifest.get("state_mode") == "range"
    if range_mode:
        try:
            contract, before, after = stateful_range.load_contract(
                manifest, args.manifest)
        except (KeyError, OSError, ValueError) as exc:
            fail("invalid range contract: %s" % exc)
        target = manifest.get("target_node")
        if target is None or contract is None:
            fail("range manifest is missing its target or byte contract")
        checked = check_state(img, geo, states[0], True,
                              range_target=int(target),
                              range_contract=contract,
                              range_before=before, range_after=after)
    else:
        errors = []
        for index, state in enumerate(states):
            try:
                checked = check_state(img, geo, state, False)
                if not args.quiet:
                    print("state %s matches (%d entries)" %
                          (manifest.get("labels", ["state"])[index]
                           if index < len(manifest.get("labels", []))
                           else index, checked))
                print("OK: FUZZ-2 stateful manifest verified")
                return 0
            except SystemExit as exc:
                errors.append(str(exc))
        fail("no manifest state matches:\n  " + "\n  ".join(errors))
    print("OK: FUZZ-2 stateful manifest metadata verified (range contract)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except AssertionError as error:
        print("stateful probe: FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
