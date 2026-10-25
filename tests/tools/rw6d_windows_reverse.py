#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6D Windows read/re-mutation after the Linux reverse pass.

usage: rw6d_windows_reverse.py --exe xfswin.exe --image reverse.img
       --manifest linux-reverse.manifest.json [--json final.manifest.json]
"""

import argparse
import os
import sys

from rw6d_common import (Cli, deep_name, load_manifest, pattern,
                         require_attrs, run_probe, sha256,
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


def has_attr(manifest, path, name):
    item = find(manifest, path)
    return any(a["name"] == name for a in item.get("attributes", []))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--json")
    ap.add_argument("--work-dir", default=".")
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
    run_probe(args.image, sorted(paths), args.manifest,
              os.path.join(args.work_dir, "linux-reverse.oracle.json"))
    for item in manifest["inodes"]:
        path = item.get("path")
        for attr in item.get("attributes", [])[:2]:
            value = cli.get(path, attr["name"])
            if len(value) != attr["length"] or \
                    sha256(value) != attr["sha256"]:
                raise SystemExit("%s %s value differs" % (path, attr["name"]))
    cli.check()

    win = pattern(64, 0xc101)
    cli.set("/short", "user.win2", win)
    set_attr(manifest, "/short", "user.win2", win)
    if has_attr(manifest, "/short", "user.linux"):
        cli.remove("/short", "user.linux")
        remove_attr(manifest, "/short", "user.linux")

    leaf = pattern(120, 0xc200)
    cli.set("/leaf", "user.win2-leaf", leaf)
    set_attr(manifest, "/leaf", "user.win2-leaf", leaf)
    if has_attr(manifest, "/leaf", "user.l99"):
        cli.remove("/leaf", "user.l99")
        remove_attr(manifest, "/leaf", "user.l99")

    if has_attr(manifest, "/node", "user.linux-node"):
        node_remote = pattern(5000, 0xc300)
        cli.set("/node", "user.linux-node", node_remote)
        set_attr(manifest, "/node", "user.linux-node", node_remote,
                 remote=True)
    node_new = pattern(100, 0xc301)
    cli.set("/node", "user.win2-node", node_new)
    set_attr(manifest, "/node", "user.win2-node", node_new)

    deep_new = pattern(300, 0xc400)
    for deep_path in ("/deep", "/bmbt"):
        if not any(i.get("path") == deep_path for i in manifest["inodes"]):
            continue
        cli.set(deep_path, "user.win2-deep", deep_new)
        set_attr(manifest, deep_path, "user.win2-deep", deep_new)
        if deep_path == "/deep":
            if has_attr(manifest, deep_path, deep_name(4002)):
                cli.remove(deep_path, deep_name(4002))
                remove_attr(manifest, deep_path, deep_name(4002))
        else:
            if has_attr(manifest, deep_path, "user.b041"):
                cli.remove(deep_path, "user.b041")
                remove_attr(manifest, deep_path, "user.b041")

    remote = pattern(6000, 0xc500)
    cli.set("/remote", "user.remote", remote)
    set_attr(manifest, "/remote", "user.remote", remote, remote=True)
    remote_new = pattern(3000, 0xc501)
    cli.set("/remote", "user.win2-remote", remote_new)
    set_attr(manifest, "/remote", "user.win2-remote", remote_new,
             remote=True)
    if has_attr(manifest, "/remote", "user.linux-remote"):
        cli.remove("/remote", "user.linux-remote")
        remove_attr(manifest, "/remote", "user.linux-remote")

    # Size changes on a file whose data fork is a btree and whose attr fork
    # holds a remote value.
    cli.truncate("/data", 2048)
    cli.truncate("/data", 6000)
    require_attrs(cli, "/data", {"user.keep": pattern(200, 0xd00d)})

    for path, name, value in (
            ("/created-win", "user.win2-created", pattern(90, 0xc600)),
            ("/linux-new", "user.win2-linux", pattern(90, 0xc601)),
            ("/dir", "user.win2-dir", pattern(90, 0xc602))):
        if not any(i.get("path") == path for i in manifest["inodes"]):
            continue
        cli.set(path, name, value)
        set_attr(manifest, path, name, value)

    old_rename = next((p for p in ("/renamed-linux", "/renamed-win",
                                   "/rename")
                       if any(i.get("path") == p
                              for i in manifest["inodes"])), None)
    if old_rename is None:
        raise SystemExit("no rename source is present")
    cli.rename(old_rename, "/renamed-final")
    renamed = find(manifest, old_rename)
    renamed["path"] = "/renamed-final"
    paths[int(renamed["ino"])] = "/renamed-final"
    final = pattern(70, 0xc700)
    cli.set("/renamed-final", "user.win2-final", final)
    set_attr(manifest, "/renamed-final", "user.win2-final", final)
    cli.check()

    for item in manifest["inodes"]:
        for key in ("format", "root_level", "aformat"):
            item.pop(key, None)
    after = run_probe(args.image, sorted(paths), None,
                      os.path.join(args.work_dir, "windows-final.oracle.json"))
    observed = {int(item["inode"]): item for item in after["inodes"]}
    for item in manifest["inodes"]:
        seen = observed[int(item["ino"])]
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

    files = {}
    for rel in list(manifest.get("files", {})) + [
            "renamed-final", "created-win", "linux-new", "data",
            "clone-win"]:
        try:
            data = cli.cat("/" + rel)
        except SystemExit:
            continue
        files[rel.lstrip("/")] = {"size": len(data), "sha256": sha256(data)}
    manifest["files"] = files
    manifest["kind"] = "rw6d-windows-final"
    out = args.json or os.path.join(args.work_dir,
                                    "windows-final.manifest.json")
    write_manifest(out, manifest)
    print("RW-6D Windows reverse mutation OK: %s" % args.image)
    print("manifest: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
