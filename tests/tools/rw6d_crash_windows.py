#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6D crash matrix for extended attributes.

Each variant is killed at one injection point, restarted with `xfswin recover`,
checked by the independent xattr oracle, and archived with the legal old or
new manifest for the Linux xfs_repair/mount verifier.

usage: rw6d_crash_windows.py --exe xfswin.exe --image base.img
       --manifest base.manifest.json --work-dir crash-rw6d
       [--zip] [--json summary.json]
"""

import argparse
import copy
import json
import os
import shutil
import subprocess
import sys
import zipfile

from rw6d_common import (Cli, load_manifest, manifest_inodes, pattern,
                         run_probe, sha256, write_manifest)

CRASH_EXIT = 0x58435348


def set_attr(manifest, path, name, value, remote=None):
    from rw6d_common import set_manifest_attribute
    set_manifest_attribute(manifest, path, name, value, remote=remote)


def remove_attr(manifest, path, name):
    from rw6d_common import remove_manifest_attribute
    remove_manifest_attribute(manifest, path, name)


def normalize(manifest):
    for item in manifest["inodes"]:
        for attr in item.get("attributes", []):
            if "value_hex" in attr:
                raw = bytes.fromhex(attr["value_hex"])
                attr["length"] = len(raw)
                attr["sha256"] = sha256(raw)
                del attr["value_hex"]
        for key in ("format", "root_level", "aformat"):
            item.pop(key, None)
    return manifest


def case_new(manifest, kind):
    new = copy.deepcopy(manifest)
    if kind == "set-short":
        set_attr(new, "/short", "user.crash", b"crash-short")
    elif kind == "set-remote":
        set_attr(new, "/short", "user.crash-remote", pattern(9000, 0xd101),
                 remote=True)
    elif kind == "replace-remote":
        set_attr(new, "/remote", "user.remote", pattern(12000, 0xd102),
                 remote=True)
    elif kind == "remote-to-local":
        set_attr(new, "/remote", "user.remote", b"local-now")
    elif kind == "remove-local":
        remove_attr(new, "/remote", "user.local")
    elif kind == "remove-remote":
        remove_attr(new, "/remote", "user.remote2")
    elif kind == "node-grow":
        set_attr(new, "/node", "user.crash", pattern(600, 0xd103))
    elif kind == "node-shrink":
        remove_attr(new, "/node", "user.n050")
    elif kind == "bmbt-add":
        set_attr(new, "/bmbt", "user.crash", pattern(200, 0xd104))
    elif kind == "dir-add":
        set_attr(new, "/dir", "user.crash", pattern(100, 0xd105))
    elif kind == "delete":
        new["inodes"] = [i for i in new["inodes"]
                         if i.get("path") != "/del"]
        new["deleted"] = new.get("deleted", []) + ["del"]
        if "del" in new.get("files", {}):
            del new["files"]["del"]
    elif kind == "rename":
        item = next(i for i in new["inodes"] if i.get("path") == "/rename")
        item["path"] = "/renamed-crash"
        if "rename" in new.get("files", {}):
            new["files"]["renamed-crash"] = new["files"].pop("rename")
    else:
        raise SystemExit("unknown crash operation %s" % kind)
    return new


def apply_operation(cli, kind):
    if kind == "set-short":
        cli.set("/short", "user.crash", b"crash-short")
    elif kind == "set-remote":
        cli.set("/short", "user.crash-remote", pattern(9000, 0xd101))
    elif kind == "replace-remote":
        cli.set("/remote", "user.remote", pattern(12000, 0xd102))
    elif kind == "remote-to-local":
        cli.set("/remote", "user.remote", b"local-now")
    elif kind == "remove-local":
        cli.remove("/remote", "user.local")
    elif kind == "remove-remote":
        cli.remove("/remote", "user.remote2")
    elif kind == "node-grow":
        cli.set("/node", "user.crash", pattern(600, 0xd103))
    elif kind == "node-shrink":
        cli.remove("/node", "user.n050")
    elif kind == "bmbt-add":
        cli.set("/bmbt", "user.crash", pattern(200, 0xd104))
    elif kind == "dir-add":
        cli.set("/dir", "user.crash", pattern(100, 0xd105))
    elif kind == "delete":
        cli.unlink("/del")
    elif kind == "rename":
        cli.rename("/rename", "/renamed-crash")
    else:
        raise SystemExit("unknown crash operation %s" % kind)


def matches(image, manifest, work):
    """True when the image exactly matches the manifest (probe or none)."""
    try:
        inodes = manifest_inodes(manifest)
        if not inodes:
            return False
        run_probe(image, inodes, _write_temp(work, manifest))
        return True
    except SystemExit:
        return False


def _write_temp(work, manifest):
    path = os.path.join(work, "candidate.manifest.json")
    write_manifest(path, manifest)
    return path


def run(exe, args, env=None):
    proc = subprocess.run([exe] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env)
    return proc.returncode, (proc.stdout + proc.stderr).decode(
        "utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)
    exe = os.path.abspath(args.exe)
    base = os.path.abspath(args.image)
    base_manifest = normalize(load_manifest(args.manifest))
    cli = Cli(exe, base, args.work_dir)
    cli.check()
    run_probe(base, manifest_inodes(base_manifest), args.manifest,
              os.path.join(args.work_dir, "base.oracle.json"))

    single = ["before_log_write", "after_log_commit",
              "before_metadata_write", "after_metadata_write"]
    remote = ["before_data_write", "after_data_write", "before_device_flush",
              "after_device_flush"] + single
    multi = ["before_log_write", "after_log_commit", "before_metadata_write",
             "mid_metadata_write", "after_metadata_write"]
    cases = [
        ("set-short", single, 1),
        ("set-remote", remote, 1),
        ("replace-remote", remote, 1),
        ("remote-to-local", single, 1),
        ("remove-local", single, 1),
        ("remove-remote", single, 1),
        ("node-grow", multi, 1),
        ("node-shrink", multi, 1),
        ("bmbt-add", multi, 1),
        ("dir-add", single, 1),
        ("delete", ["after_log_commit", "before_metadata_write",
                    "after_metadata_write"], 4),
        ("rename", ["after_log_commit", "before_metadata_write",
                    "after_metadata_write"], 1),
    ]

    old_manifest = base_manifest
    failures = []
    crashes = 0
    archived = 0
    for kind, points, max_hit in cases:
        new_manifest = case_new(base_manifest, kind)
        for point in points:
            hits = range(1, max_hit + 1)
            for hit in hits:
                tag = "%s-%s%s" % (kind, point,
                                   "" if max_hit == 1 else "-%d" % hit)
                image = os.path.join(args.work_dir, tag + ".img")
                shutil.copyfile(base, image)
                env = dict(os.environ)
                env["XFSWIN_CRASH_AT"] = "%s:%d" % (point, hit)
                rc, text = run(exe, apply_operation_args(kind, image,
                                                         args.work_dir),
                               env=env)
                if rc == 0:
                    # This hit lies beyond the operation's transaction count.
                    if max_hit == 1:
                        failures.append("%s: crash point was not reached" %
                                        tag)
                    os.unlink(image)
                    continue
                if rc != CRASH_EXIT:
                    failures.append("%s: expected crash, got exit %d:\n%s" %
                                    (tag, rc, text))
                    os.unlink(image)
                    continue
                crashes += 1
                rc, text = run(exe, ["recover", image,
                                     "--yes-i-really-mean-it"])
                if rc != 0:
                    failures.append("%s: recovery failed:\n%s" % (tag, text))
                    os.unlink(image)
                    continue
                if run(exe, ["check", image])[0] != 0:
                    failures.append("%s: check failed" % tag)
                    os.unlink(image)
                    continue
                if kind == "rename":
                    old_present = run(exe, ["cat", image, "/rename"])[0] == 0
                    new_present = run(exe, ["cat", image,
                                            "/renamed-crash"])[0] == 0
                    if old_present and not new_present:
                        chosen = copy.deepcopy(old_manifest)
                        state = "old"
                    elif new_present and not old_present:
                        chosen = copy.deepcopy(new_manifest)
                        state = "new"
                    else:
                        failures.append("%s: rename namespace is neither old "
                                        "nor new" % tag)
                        os.unlink(image)
                        continue
                elif kind == "delete":
                    present = run(exe, ["cat", image, "/del"])[0] == 0
                    chosen = copy.deepcopy(old_manifest if present
                                           else new_manifest)
                    state = "old" if present else "new"
                elif matches(image, old_manifest, args.work_dir):
                    chosen = copy.deepcopy(old_manifest)
                    state = "old"
                elif matches(image, new_manifest, args.work_dir):
                    chosen = copy.deepcopy(new_manifest)
                    state = "new"
                else:
                    failures.append("%s: neither the old nor the new state" %
                                    tag)
                    os.unlink(image)
                    continue
                if not matches(image, chosen, args.work_dir):
                    failures.append("%s: selected %s state does not match" %
                                    (tag, state))
                    os.unlink(image)
                    continue
                observed = run_probe(image, manifest_inodes(chosen), None,
                                     os.path.join(args.work_dir,
                                                  tag + ".oracle.json"))
                formats = {int(i["inode"]): i for i in observed["inodes"]}
                for item in chosen["inodes"]:
                    seen = formats[int(item["ino"])]
                    item["format"] = seen["format"]
                    item["root_level"] = seen["root_level"]
                    item["aformat"] = seen["aformat"]
                chosen["kind"] = "rw6d-crash"
                chosen["crash"] = {"operation": kind, "point": point,
                                   "hit": hit, "state": state}
                manifest_path = os.path.join(args.work_dir,
                                             tag + ".manifest.json")
                write_manifest(manifest_path, chosen)
                if args.zip:
                    archive = os.path.join(args.work_dir, tag + ".zip")
                    with zipfile.ZipFile(archive, "w",
                                         zipfile.ZIP_DEFLATED) as z:
                        z.write(image, tag + ".img")
                        z.write(manifest_path, tag + ".manifest.json")
                    os.unlink(image)
                else:
                    os.unlink(image)
                os.unlink(manifest_path)
                archived += 1
                print("ok: %s recovered (%s state, formats %s)" %
                      (tag, state, ",".join(sorted({i["format"]
                                                    for i in chosen["inodes"]}))))

    summary = {"result": "ok" if not failures else "failed",
               "cases": len(cases), "crashes": crashes,
               "archived": archived, "failures": failures}
    out = args.json or os.path.join(args.work_dir, "crash.summary.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("RW-6D crash matrix OK: %d crash images, %d archived" %
          (crashes, archived))
    return 0


def apply_operation_args(kind, image, work):
    if kind == "set-short":
        return ["xattr", "set", image, "/short", "user.crash",
                "--value-file=" + _value(work, b"crash-short"),
                "--rw-experimental"]
    if kind == "set-remote":
        return ["xattr", "set", image, "/short", "user.crash-remote",
                "--value-file=" + _value(work, pattern(9000, 0xd101)),
                "--rw-experimental"]
    if kind == "replace-remote":
        return ["xattr", "set", image, "/remote", "user.remote",
                "--value-file=" + _value(work, pattern(12000, 0xd102)),
                "--rw-experimental"]
    if kind == "remote-to-local":
        return ["xattr", "set", image, "/remote", "user.remote",
                "--value-file=" + _value(work, b"local-now"),
                "--rw-experimental"]
    if kind == "remove-local":
        return ["xattr", "remove", image, "/remote", "user.local",
                "--rw-experimental"]
    if kind == "remove-remote":
        return ["xattr", "remove", image, "/remote", "user.remote2",
                "--rw-experimental"]
    if kind == "node-grow":
        return ["xattr", "set", image, "/node", "user.crash",
                "--value-file=" + _value(work, pattern(600, 0xd103)),
                "--rw-experimental"]
    if kind == "node-shrink":
        return ["xattr", "remove", image, "/node", "user.n050",
                "--rw-experimental"]
    if kind == "bmbt-add":
        return ["xattr", "set", image, "/bmbt", "user.crash",
                "--value-file=" + _value(work, pattern(200, 0xd104)),
                "--rw-experimental"]
    if kind == "dir-add":
        return ["xattr", "set", image, "/dir", "user.crash",
                "--value-file=" + _value(work, pattern(100, 0xd105)),
                "--rw-experimental"]
    if kind == "delete":
        return ["unlink", image, "/del", "--rw-experimental"]
    if kind == "rename":
        return ["rename", image, "/rename", "/renamed-crash",
                "--rw-experimental"]
    raise SystemExit("unknown crash operation %s" % kind)


def _value(work, data):
    path = os.path.join(work, "crash-value.bin")
    with open(path, "wb") as f:
        f.write(data)
    return path


if __name__ == "__main__":
    sys.exit(main())
