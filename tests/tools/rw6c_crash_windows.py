#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C crash matrix for native clone, CoW and shared truncate.

Each variant is killed at one injection point, restarted with `xfswin recover`,
checked by the independent refcount oracle, and archived with an expected
content manifest for the Linux xfs_repair/mount verifier.

usage: rw6c_crash_windows.py --exe xfswin.exe --work-dir crash-rw6c
       [--profile rmap|normap] [--zip]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile

BLOCK = 4096
CRASH_EXIT = 0x58435348


def pattern(size, seed):
    return bytes(((i * 37 + seed * 17 + 11) & 0xFF) for i in range(size))


def sha(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def run(exe, args, env=None):
    proc = subprocess.run([exe] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    return proc.returncode, text


def must(exe, args, env=None):
    rc, text = run(exe, args, env=env)
    if rc != 0:
        raise SystemExit("xfswin %s failed (exit %d):\n%s" %
                         (" ".join(args), rc, text))
    return text


def cat(exe, image, path):
    proc = subprocess.run([exe, "cat", image, path], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise SystemExit("cat %s failed:\n%s" %
                         (path, proc.stderr.decode("utf-8", "replace")))
    return proc.stdout


def oracle(python, probe, image, allow_no_rmapbt):
    out = image + ".oracle.json"
    cmd = [python, probe, image, "--json", out]
    if allow_no_rmapbt:
        cmd.append("--allow-no-rmapbt")
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print((proc.stdout + proc.stderr).decode("utf-8", "replace"),
              file=sys.stderr)
        raise SystemExit("independent oracle rejected %s" % image)
    data = json.load(open(out))
    if data.get("result") != "ok":
        raise SystemExit("oracle result is not ok for %s" % image)
    return data


def make_base(exe, work, profile):
    image = os.path.join(work, "base.img")
    with open(image, "wb") as f:
        f.truncate(300 << 20)
    args = ["format", image, "--reflink", "--yes-i-really-mean-it"]
    if profile == "rmap":
        args.insert(2, "--rmapbt")
    must(exe, args)
    qual = ["--rw-experimental", "--rw-reflink-qualification"]
    must(exe, ["create", image, "/src"] + qual)
    must(exe, ["create", image, "/dst"] + qual)
    must(exe, ["create", image, "/clone"] + qual)
    source = pattern(4 * BLOCK, 700)
    src_file = os.path.join(work, "source.bin")
    with open(src_file, "wb") as f:
        f.write(source)
    must(exe, ["pwrite", image, "/src", "0"] + qual + ["--from=" + src_file])
    must(exe, ["reflink", image, "/src", "/dst", "0", "0", "0"] + qual)
    return image, source


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--profile", choices=("rmap", "normap"), default="rmap")
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)
    exe = os.path.abspath(args.exe)
    work = os.path.abspath(args.work_dir)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = os.path.join(repo, "tools", "rw6c_refcount_probe.py")
    python = sys.executable
    base, source = make_base(exe, work, args.profile)
    cow_patch = pattern(2 * BLOCK, 701)
    cow_new = source[:BLOCK] + cow_patch + source[3 * BLOCK:]
    patch_path = os.path.join(work, "cow-patch.bin")
    with open(patch_path, "wb") as f:
        f.write(cow_patch)
    qual = ["--rw-experimental", "--rw-reflink-qualification"]

    cases = [
        ("cow", "cow_staged:1"),
        ("cow", "before_cow_data_write:1"),
        ("cow", "mid_cow_data_write:1"),
        ("cow", "after_cow_data_write:1"),
        ("cow", "before_cow_remap_commit:1"),
        ("cow", "after_cow_remap:1"),
        ("cow", "before_log_write:1"),
        ("cow", "after_log_commit:1"),
        ("cow", "before_metadata_write:1"),
        ("cow", "after_metadata_write:1"),
        ("cow", "before_log_write:2"),
        ("cow", "after_log_commit:2"),
        ("clone", "after_bmap_switch:1"),
        ("clone", "after_log_commit:1"),
        ("clone", "before_metadata_write:1"),
        ("truncate", "after_log_commit:1"),
        ("truncate", "before_metadata_write:1"),
    ]
    commands = {
        "cow": ["pwrite", base, "/dst", str(BLOCK)] + qual + [
            "--from=" + patch_path],
        "clone": ["reflink", base, "/src", "/clone", "0", "0", "0"] + qual,
        "truncate": ["truncate", base, "/dst", "0"] + qual,
    }
    failures = []
    crashes = 0
    manifest_out = {}
    for op, point in cases:
        tag = "%s-%s" % (op, point.replace(":", "-"))
        image = os.path.join(work, tag + ".img")
        shutil.copyfile(base, image)
        argv = [image if a == base else a for a in commands[op]]
        env = dict(os.environ)
        env["XFSWIN_CRASH_AT"] = point
        rc, text = run(exe, argv, env=env)
        if rc != CRASH_EXIT:
            failures.append("%s: expected crash, got exit %d:\n%s" %
                            (tag, rc, text))
            continue
        crashes += 1
        if "CRASH INJECTION at %s" % point.split(":")[0] not in text:
            failures.append("%s: crash marker missing" % tag)
            continue
        rc, text = run(exe, ["recover", image, "--yes-i-really-mean-it"])
        if rc != 0:
            failures.append("%s: recovery failed:\n%s" % (tag, text))
            continue
        rc, text = run(exe, ["check", image])
        if rc != 0:
            failures.append("%s: check failed:\n%s" % (tag, text))
            continue
        try:
            data = oracle(python, probe, image, args.profile == "normap")
        except SystemExit as exc:
            failures.append("%s: %s" % (tag, exc))
            continue
        src_actual = cat(exe, image, "/src")
        dst_actual = cat(exe, image, "/dst")
        clone_actual = cat(exe, image, "/clone")
        if src_actual != source:
            failures.append("%s: source content changed" % tag)
            continue
        expected = {"/src": source}
        if op == "cow":
            if dst_actual not in (source, cow_new):
                failures.append("%s: destination is neither old nor new" % tag)
                continue
            expected["/dst"] = dst_actual
            expected["/clone"] = b""
        elif op == "clone":
            if clone_actual not in (b"", source):
                failures.append("%s: clone is neither empty nor complete" % tag)
                continue
            expected["/dst"] = source
            expected["/clone"] = clone_actual
        else:
            if dst_actual not in (b"", source):
                failures.append("%s: truncate left an invalid size" % tag)
                continue
            expected["/dst"] = dst_actual
            expected["/clone"] = b""
        manifest = {
            "kind": "rw6c-crash",
            "operation": op,
            "crash_point": point,
            "files": {
                rel.lstrip("/"): {"size": len(data),
                                  "sha256": sha(data)}
                for rel, data in expected.items()
            },
            "oracle": {
                "shared_records": data["shared_records"],
                "shared_blocks": data["shared_blocks"],
                "cow_records": data["cow_records"],
            },
        }
        manifest_path = os.path.join(work, tag + ".manifest.json")
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
        manifest_out[tag] = manifest
        if args.zip:
            archive = os.path.join(work, tag + ".zip")
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
                z.write(image, tag + ".img")
                z.write(manifest_path, tag + ".manifest.json")
            os.unlink(image)
        else:
            os.unlink(image)
        os.unlink(manifest_path)
        print("ok: %s recovered, oracle shared=%d CoW=%d" %
              (tag, data["shared_records"], data["cow_records"]))

    summary = {"profile": args.profile, "crashes": crashes,
               "cases": len(cases), "failures": failures,
               "result": "ok" if not failures else "failed"}
    out = args.json or os.path.join(work, "crash.manifest.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("RW-6C crash matrix OK: %d/%d crashes recovered" %
          (crashes, len(cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
