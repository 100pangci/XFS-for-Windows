#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6D ENOSPC behaviour: failed attribute writes must change nothing.

The tool fills a Windows-formatted xattr fixture, requires remote-value sets
and replacements to fail with ENOSPC while leaving every existing attribute
intact, verifies that a pure-shortform set still works, then frees the filler
and requires the same remote set to succeed.

usage: rw6d_enospc_windows.py --exe xfswin.exe --image fill.img
       --manifest base.manifest.json --work-dir out/ [--json result.json]
"""

import argparse
import os
import sys

from rw6d_common import (Cli, load_manifest, manifest_inodes, pattern,
                         run, run_probe, sha256, write_manifest)


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


def expect_no_space(what, fn):
    try:
        fn()
    except SystemExit as error:
        text = str(error).lower()
        if "no space" not in text:
            raise SystemExit("%s failed for the wrong reason: %s" %
                             (what, error))
        print("ok: %s refused with ENOSPC" % what)
        return
    raise SystemExit("%s unexpectedly succeeded on a full filesystem" % what)


def unchanged(image, manifest, oracle, work):
    path = os.path.join(work, "unchanged.manifest.json")
    write_manifest(path, manifest)
    try:
        run_probe(image, manifest_inodes(manifest), path, oracle)
        return True
    except SystemExit:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)
    manifest = normalize(load_manifest(args.manifest))
    cli = Cli(args.exe, args.image, args.work_dir)
    run_probe(args.image, manifest_inodes(manifest), args.manifest,
              os.path.join(args.work_dir, "before.oracle.json"))
    cli.check()

    cli.create("/fill")
    offset = 0
    filled = 0
    errors = []
    for chunk in (4 << 20, 64 << 10, 4096):
        while True:
            rc, text = run(cli.exe, ["fallocate", cli.image, "/fill",
                                     str(offset), str(chunk)]
                           + cli.options, check=False)
            if rc == 0:
                offset += chunk
                filled += chunk
                continue
            if "no space" not in text.lower():
                errors.append("fallocate(%d) failed: %s" % (chunk, text))
            break
        if errors:
            break
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    if offset < (150 << 20):
        raise SystemExit("only %d bytes could be allocated; the test is "
                         "not exhausting the filesystem" % offset)
    print("filled %d bytes; free space exhausted" % offset)

    big = pattern(60000, 0xf1f1)
    expect_no_space("new remote value on /short",
                    lambda: cli.set("/short", "user.big", big))
    expect_no_space("replace remote value on /remote",
                    lambda: cli.set("/remote", "user.remote", big,
                                   mode="replace"))
    expect_no_space("new remote value on /data",
                    lambda: cli.set("/data", "user.big", big))
    expect_no_space("new remote value on /node",
                    lambda: cli.set("/node", "user.big", big))

    # A shortform value only consumes inode literal space.
    cli.set("/short", "user.small", b"s")
    if cli.get("/short", "user.small") != b"s":
        raise SystemExit("shortform set did not survive on a full filesystem")
    cli.remove("/short", "user.small")

    # The old values and every other attribute are byte-identical.
    if not unchanged(args.image, manifest,
                     os.path.join(args.work_dir, "after-failures.oracle.json"),
                     args.work_dir):
        raise SystemExit("failed attribute writes changed the image")
    cli.check()
    print("ok: failed writes left the attribute forks unchanged")

    cli.truncate("/fill", 0)
    recovered = pattern(60000, 0xf2f2)
    cli.set("/short", "user.recovered", recovered)
    if cli.get("/short", "user.recovered") != recovered:
        raise SystemExit("remote set after freeing space did not round-trip")
    cli.unlink("/fill")
    cli.check()

    item = next(i for i in manifest["inodes"] if i.get("path") == "/short")
    item["attributes"] = [a for a in item["attributes"]
                          if a["name"] != "user.recovered"]
    item["attributes"].append({
        "name": "user.recovered", "length": len(recovered),
        "sha256": sha256(recovered), "remote": True,
    })
    observed = run_probe(args.image, manifest_inodes(manifest), None,
                         os.path.join(args.work_dir, "final.oracle.json"))
    formats = {int(i["inode"]): i for i in observed["inodes"]}
    for entry in manifest["inodes"]:
        seen = formats[int(entry["ino"])]
        entry["format"] = seen["format"]
        entry["root_level"] = seen["root_level"]
        entry["aformat"] = seen["aformat"]
    manifest["kind"] = "rw6d-enospc"
    out = args.json or os.path.join(args.work_dir, "enospc.manifest.json")
    write_manifest(out, manifest)
    run_probe(args.image, manifest_inodes(manifest), out,
              os.path.join(args.work_dir, "final-verify.oracle.json"))
    print("RW-6D ENOSPC test OK: %s" % args.image)
    print("manifest: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
