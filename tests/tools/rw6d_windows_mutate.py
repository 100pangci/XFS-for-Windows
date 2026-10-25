#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6D Windows-side mutation of a Linux-created xattr image.

The source fixture is produced by tests/scripts/make_rw6d_linux.sh.  This
tool reads every attribute through the public CLI + independent oracle, then
performs native sets/replaces/removes across every representation, attributes
on directories/symlinks, data writes, rename/hardlink/unlink and a reflink
clone.  It writes windows-mutated.manifest.json for the Linux verifier.

usage: rw6d_windows_mutate.py --exe xfswin.exe --image in.img
       --work-dir out/ [--json manifest.json]
"""

import argparse
import json
import os
import sys

from rw6d_common import (Cli, deep_name, load_manifest, pattern,
                         require, require_attrs, run_probe, sha256,
                         write_manifest)


def find(manifest, path):
    item = next((i for i in manifest["inodes"] if i.get("path") == path),
                None)
    if item is None:
        raise SystemExit("manifest has no path %s" % path)
    return item


def set_attr(manifest, path, name, value, remote=None):
    from rw6d_common import set_manifest_attribute
    set_manifest_attribute(manifest, path, name, value, remote=remote)


def remove_attr(manifest, path, name):
    from rw6d_common import remove_manifest_attribute
    remove_manifest_attribute(manifest, path, name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)
    manifest = load_manifest(args.manifest)
    for item in manifest["inodes"]:
        for attr in item.get("attributes", []):
            if "value_hex" in attr:
                raw = bytes.fromhex(attr["value_hex"])
                attr["length"] = len(raw)
                attr["sha256"] = sha256(raw)
                del attr["value_hex"]
    cli = Cli(args.exe, args.image, args.work_dir)
    paths = {int(i["ino"]): i.get("path") for i in manifest["inodes"]}
    inodes = sorted(paths)

    # Initial read: the independent parser compares every value against the
    # Linux manifest, then the public CLI checks a sample through get().
    run_probe(args.image, inodes, args.manifest,
              os.path.join(args.work_dir, "before.oracle.json"))
    for item in manifest["inodes"]:
        path = item.get("path")
        if not path:
            continue
        attrs = item.get("attributes", [])
        for attr in attrs[:2]:
            if attr.get("value_hex") is not None:
                expected = bytes.fromhex(attr["value_hex"])
                require(cli.get(path, attr["name"]), expected,
                        "%s %s" % (path, attr["name"]))
            else:
                value = cli.get(path, attr["name"])
                if len(value) != attr["length"] or \
                        sha256(value) != attr["sha256"]:
                    raise SystemExit("%s %s value differs" % (path,
                                                              attr["name"]))
    for rel, want in manifest.get("files", {}).items():
        data = cli.cat("/" + rel)
        if len(data) != want["size"] or sha256(data) != want["sha256"]:
            raise SystemExit("%s content differs" % rel)
    cli.check()

    # /short: shortform -> single leaf with a remote value.
    short_value = pattern(4000, 0xb101)
    cli.set("/short", "user.alpha", short_value)
    set_attr(manifest, "/short", "user.alpha", short_value, remote=True)
    win_value = pattern(48, 0xb102)
    cli.set("/short", "user.win", win_value)
    set_attr(manifest, "/short", "user.win", win_value)
    cli.remove("/short", "user.beta")
    remove_attr(manifest, "/short", "user.beta")

    # /leaf: grow away from the single-leaf limit and back.
    for i in range(10):
        name = "user.lw%02d" % i
        value = pattern(200, 0xb200 + i)
        cli.set("/leaf", name, value)
        set_attr(manifest, "/leaf", name, value)
    cli.remove("/leaf", "user.l00")
    remove_attr(manifest, "/leaf", "user.l00")

    # /node: split, replace with a remote value and merge one leaf entry.
    for i in range(20):
        name = "user.win%02d" % i
        value = pattern(600, 0xb300 + i)
        cli.set("/node", name, value)
        set_attr(manifest, "/node", name, value)
    node_remote = pattern(8000, 0xb400)
    cli.set("/node", "user.n010", node_remote)
    set_attr(manifest, "/node", "user.n010", node_remote, remote=True)
    cli.remove("/node", "user.n020")
    remove_attr(manifest, "/node", "user.n020")

    # /deep (Linux fixture) or /bmbt (Windows fixture): modify a level-2 or
    # attribute-BMBT tree in place.
    for deep_path in ("/deep", "/bmbt"):
        if not any(i.get("path") == deep_path for i in manifest["inodes"]):
            continue
        deep_new = pattern(300, 0xb500)
        cli.set(deep_path, "user.win-deep", deep_new)
        set_attr(manifest, deep_path, "user.win-deep", deep_new)
        if deep_path == "/deep":
            cli.remove(deep_path, deep_name(4001))
            remove_attr(manifest, deep_path, deep_name(4001))
        else:
            cli.remove(deep_path, "user.b040")
            remove_attr(manifest, deep_path, "user.b040")

    # /remote: remote replace, new remote and local -> remote transitions.
    remote_value = pattern(12000, 0xb600)
    cli.set("/remote", "user.remote", remote_value)
    set_attr(manifest, "/remote", "user.remote", remote_value, remote=True)
    extra_remote = pattern(4000, 0xb601)
    cli.set("/remote", "user.win-remote", extra_remote)
    set_attr(manifest, "/remote", "user.win-remote", extra_remote,
             remote=True)
    local_value = pattern(77, 0xb602)
    cli.set("/remote", "user.local", local_value)
    set_attr(manifest, "/remote", "user.local", local_value)
    cli.remove("/remote", "user.remote2")
    remove_attr(manifest, "/remote", "user.remote2")
    require_attrs(cli, "/remote", {
        "user.remote": remote_value,
        "user.win-remote": extra_remote,
        "user.local": local_value,
    })

    # Data writes and a clone must leave the source attribute fork untouched.
    data_patch = pattern(4096, 0xb700)
    cli.write("/data", data_patch, 0)
    cli.set("/data", "user.win-data", pattern(100, 0xb701))
    set_attr(manifest, "/data", "user.win-data", pattern(100, 0xb701))
    cli.create("/clone-win")
    clone_ino = cli.stat_inode("/clone-win")
    manifest["inodes"].append({"ino": clone_ino, "path": "/clone-win",
                               "attributes": []})
    paths[clone_ino] = "/clone-win"
    cli.reflink("/data", "/clone-win")
    require_attrs(cli, "/data", {"user.keep": pattern(200, 0xd00d)})
    clone_attrs = cli.list("/clone-win")
    if clone_attrs:
        raise SystemExit("reflink unexpectedly copied xattrs: %s" %
                         clone_attrs)

    cli.set("/dir", "user.win-dir", pattern(90, 0xb800))
    set_attr(manifest, "/dir", "user.win-dir", pattern(90, 0xb800))
    cli.set("/link", "trusted.win-link", pattern(48, 0xb900))
    set_attr(manifest, "/link", "trusted.win-link", pattern(48, 0xb900))

    # Namespace operations preserve the inode's attribute fork.
    cli.rename("/rename", "/renamed-win")
    renamed = find(manifest, "/rename")
    renamed["path"] = "/renamed-win"
    paths[int(renamed["ino"])] = "/renamed-win"
    win_rename = pattern(70, 0xba00)
    cli.set("/renamed-win", "user.win-renamed", win_rename)
    set_attr(manifest, "/renamed-win", "user.win-renamed", win_rename)

    linkme = find(manifest, "/linkme")
    cli.link("/linkme", "/linkme2")
    require_attrs(cli, "/linkme2", {"user.h0": pattern(50, 0x9900)})
    cli.unlink("/linkme2")
    require_attrs(cli, "/linkme", {"user.h0": pattern(50, 0x9900)})

    cli.unlink("/del")
    cli.expect_missing("/del")
    manifest["deleted"] = manifest.get("deleted", []) + ["del"]
    manifest["inodes"] = [i for i in manifest["inodes"]
                          if i.get("path") != "/del"]
    paths = {int(i["ino"]): i.get("path") for i in manifest["inodes"]}

    # A brand-new file with its own attribute tree.
    cli.create("/created-win")
    created_ino = cli.stat_inode("/created-win")
    created = {"ino": created_ino, "path": "/created-win", "attributes": []}
    manifest["inodes"].append(created)
    for i in range(40):
        name = "user.cw%02d" % i
        value = pattern(150, 0xbb00 + i)
        cli.set("/created-win", name, value)
        created["attributes"].append({
            "name": name, "length": len(value), "sha256": sha256(value)
        })
    paths[created_ino] = "/created-win"

    cli.check()

    # The final probe is the structural oracle for the whole mutated image.
    # Format hints from the pre-mutation manifest may no longer hold.
    for item in manifest["inodes"]:
        for key in ("format", "root_level", "aformat"):
            item.pop(key, None)
    inodes = sorted(paths)
    after = run_probe(args.image, inodes, None,
                      os.path.join(args.work_dir, "windows-mutated.oracle.json"))
    observed = {int(item["inode"]): item for item in after["inodes"]}
    for item in manifest["inodes"]:
        ino = int(item["ino"])
        seen = observed.get(ino)
        if seen is None:
            raise SystemExit("oracle did not read inode %d" % ino)
        expected = {a["name"]: (a["length"], a["sha256"])
                    for a in item["attributes"]}
        actual = {a["name"]: (a["length"], a["sha256"])
                  for a in seen["attributes"]}
        if expected != actual:
            raise SystemExit("%s attributes differ from the oracle: %s != %s" %
                             (item.get("path"), sorted(expected),
                              sorted(actual)))
        item["format"] = seen["format"]
        item["root_level"] = seen["root_level"]
        item["aformat"] = seen["aformat"]

    # Content manifest for the Linux verifier.
    files = {}
    for rel in list(manifest.get("files", {})) + [
            "renamed-win", "created-win", "clone-win", "data"]:
        path = "/" + rel
        try:
            data = cli.cat(path)
        except SystemExit:
            continue
        files[rel] = {"size": len(data), "sha256": sha256(data)}
    manifest["files"] = files
    manifest["kind"] = "rw6d-windows-mutated"
    out = args.json or os.path.join(args.work_dir,
                                    "windows-mutated.manifest.json")
    write_manifest(out, manifest)
    print("RW-6D Windows mutation OK: %s" % args.image)
    print("manifest: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
