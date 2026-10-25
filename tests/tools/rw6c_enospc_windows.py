#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C near-ENOSPC qualification on a real Linux reflink image.

Fill free space until 4 contiguous filesystem blocks can no longer be
allocated, then attempt a 4-block CoW write to a shared file.  The write must
fail with a genuine free-space error, leave the target byte-identical, leave no
CoW staging record, and keep the refcount/rmap trees structurally valid.

usage: rw6c_enospc_windows.py --exe xfswin.exe --image rw6c.img
       --work-dir enospc/ [--json evidence.json]
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

BLOCK = 4096


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(exe, args):
    proc = subprocess.run([exe] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    return proc.returncode, text


def must(exe, args):
    rc, text = run(exe, args)
    if rc != 0:
        raise SystemExit("xfswin %s failed (exit %d):\n%s" %
                         (" ".join(args), rc, text))
    return text


def cat(exe, image, path):
    proc = subprocess.run([exe, "cat", image, path], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise SystemExit("cat %s failed" % path)
    return proc.stdout


def info(exe, image):
    text = must(exe, ["info", image])
    free = None
    block = None
    for line in text.splitlines():
        if "free blocks" in line:
            free = int(line.split(":", 1)[1].strip())
        if "block size" in line:
            block = int(line.split(":", 1)[1].split()[0])
    if free is None or block is None:
        raise SystemExit("cannot parse xfswin info")
    return free, block


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)
    exe = os.path.abspath(args.exe)
    image = os.path.join(os.path.abspath(args.work_dir), "enospc.img")
    shutil.copyfile(args.image, image)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = os.path.join(repo, "tools", "rw6c_refcount_probe.py")
    qual = ["--rw-experimental", "--rw-reflink-qualification"]
    free_before, block = info(exe, image)
    if block not in (1024, 2048, 4096, 8192):
        raise SystemExit("unexpected filesystem block size %d" % block)

    must(exe, ["check", image])
    before = cat(exe, image, "/base/cow.bin")
    if len(before) < 4 * block:
        raise SystemExit("source /base/cow.bin is too small for the CoW test")

    must(exe, ["mkdir", image, "/enospc"] + qual)
    chunk_bytes = 32 << 20
    chunk = os.path.join(args.work_dir, "fill.bin")
    with open(chunk, "wb") as f:
        f.truncate(chunk_bytes)
    free_bytes = free_before * block
    fill_bytes = max(0, free_bytes - (32 << 20))
    must(exe, ["create", image, "/enospc/fill.bin"] + qual)
    offset = 0
    while offset < fill_bytes:
        length = min(chunk_bytes, fill_bytes - offset)
        if length != os.path.getsize(chunk):
            with open(chunk, "wb") as f:
                f.truncate(length)
        must(exe, ["pwrite", image, "/enospc/fill.bin", str(offset)] +
             qual + ["--from=" + chunk])
        offset += length
    print("filled %.1f MiB, %d free blocks remain" %
          (fill_bytes / (1 << 20), info(exe, image)[0]))

    # Drain the remaining ~32 MiB with 1 MiB preallocations, then with
    # four-block preallocations.  The final failure proves that no AG has a
    # four-block free run, so a four-block CoW staging allocation must fail.
    must(exe, ["create", image, "/enospc/mib.bin"] + qual)
    failed = None
    drained = 0
    for i in range(128):
        rc, text = run(exe, ["fallocate", image, "/enospc/mib.bin",
                             str(i * (1 << 20)), str(1 << 20)] + qual)
        if rc != 0:
            failed = (i, text)
            break
        drained += 1
    if failed is None:
        raise SystemExit("1 MiB preallocation never failed")
    if not any(word in failed[1].lower()
               for word in ("no space", "nospace", "enospc", "free space")):
        raise SystemExit("fill stopped for a non-ENOSPC reason:\n%s" %
                         failed[1])
    must(exe, ["create", image, "/enospc/small.bin"] + qual)
    small_start = failed[0]
    failed = None
    for i in range(1024):
        rc, text = run(exe, ["fallocate", image, "/enospc/small.bin",
                             str(i * 4 * block), str(4 * block)] + qual)
        if rc != 0:
            failed = (small_start + i, text)
            break
        drained += 1
    if failed is None:
        raise SystemExit("four-block preallocation never failed")
    if not any(word in failed[1].lower()
               for word in ("no space", "nospace", "enospc", "free space")):
        raise SystemExit("four-block drain stopped for an unrelated reason:\n%s"
                         % failed[1])

    # The CoW allocator retries 2- and 1-block runs after ENOSPC, so drain the
    # remaining single-block fragments too.  Only then must a 4-block CoW fail.
    must(exe, ["create", image, "/enospc/one.bin"] + qual)
    failed = None
    for i in range(256):
        rc, text = run(exe, ["fallocate", image, "/enospc/one.bin",
                             str(i * block), str(block)] + qual)
        if rc != 0:
            failed = (i, text)
            break
        drained += 1
    if failed is None:
        raise SystemExit("single-block preallocation never failed")
    if not any(word in failed[1].lower()
               for word in ("no space", "nospace", "enospc", "free space")):
        raise SystemExit("single-block drain stopped for an unrelated reason:"
                         "\n%s" % failed[1])

    before_cow = cat(exe, image, "/base/cow.bin")
    patch = os.path.join(args.work_dir, "cow4.bin")
    with open(patch, "wb") as f:
        f.write(bytes(((i * 29 + 7) & 0xFF) for i in range(4 * block)))
    rc, text = run(exe, ["pwrite", image, "/base/cow.bin", "0"] + qual +
                   ["--from=" + patch])
    if rc == 0:
        raise SystemExit("CoW write unexpectedly succeeded with no 4-block "
                         "free extent")
    if not any(word in text.lower()
               for word in ("no space", "nospace", "enospc", "free space")):
        raise SystemExit("CoW failed for a non-ENOSPC reason:\n%s" % text)
    after_cow = cat(exe, image, "/base/cow.bin")
    if after_cow != before_cow:
        raise SystemExit("failed CoW changed the target file")
    must(exe, ["check", image])

    oracle_out = os.path.join(args.work_dir, "oracle.json")
    cmd = [sys.executable, probe, image, "--json", oracle_out]
    if "rmapbt=no" in must(exe, ["info", image]):
        cmd.append("--allow-no-rmapbt")
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print((proc.stdout + proc.stderr).decode("utf-8", "replace"),
              file=sys.stderr)
        raise SystemExit("refcount oracle rejected the ENOSPC image")
    oracle = json.load(open(oracle_out))
    if oracle.get("cow_records") != 0:
        raise SystemExit("failed CoW leaked %d staging records" %
                         oracle.get("cow_records"))
    free_after, _ = info(exe, image)
    evidence = {
        "block_size": block,
        "free_blocks_before": free_before,
        "free_blocks_after": free_after,
        "fill_bytes": fill_bytes,
        "drained_allocations": drained,
        "failed_index": failed[0],
        "cow_failure": text.strip(),
        "cow_sha256": sha(before_cow),
        "cow_unchanged": after_cow == before_cow,
        "oracle": {
            "shared_records": oracle.get("shared_records"),
            "shared_blocks": oracle.get("shared_blocks"),
            "cow_records": oracle.get("cow_records"),
            "result": oracle.get("result"),
        },
        "result": "ok",
    }
    out = args.json or os.path.join(args.work_dir, "enospc.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(evidence, f, indent=2, sort_keys=True)
    print("RW-6C ENOSPC qualification OK: CoW refused, image valid")
    print("evidence: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
