#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Delta-debugs a failing FUZZ-2 operation file.

FUZZ-2 records every stateful run as a deterministic operation log
(``stateful_fuzz --ops-out``).  This reducer removes operation chunks while
the original failure signature still appears; the result is an independent,
small textual reproducer that can be replayed with ``--ops-file``.

It intentionally targets normal-run semantic failures.  A crash failure is
already reduced by ``--crash-op`` and is reproduced by the seed + crash point
line printed by the harness.

usage:
  reduce_stateful_trace.py --ops failure.ops --exe build/tests/stateful_fuzz \
      --signature "content hash differs" [--work-dir out] [--max-runs 80]
"""

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile


def read_ops(path):
    lines = []
    header = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#") or not line:
                continue
            if "=" in line and "\t" not in line:
                key, value = line.split("=", 1)
                header[key] = value
            else:
                lines.append(line)
    return header, lines


def write_ops(path, header, lines):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# FUZZ-2 reduced operation file\n")
        for key in ("seed", "profile", "workload"):
            if key in header:
                f.write("%s=%s\n" % (key, header[key]))
        for line in lines:
            f.write(line + "\n")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--ops", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--signature", required=True)
    parser.add_argument("--work-dir", default=None)
    parser.add_argument("--max-runs", type=int, default=80)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args(argv[1:])

    header, original = read_ops(args.ops)
    if not original:
        raise SystemExit("reduce: operation file is empty")
    work = args.work_dir or tempfile.mkdtemp(prefix="fuzz2-reduce-")
    os.makedirs(work, exist_ok=True)
    image = os.path.join(work, "candidate.img")
    trace = os.path.join(work, "candidate.trace.txt")
    manifest = os.path.join(work, "candidate.manifest.json")
    ops_path = os.path.join(work, "candidate.ops")
    runs = 0

    def reproduces(lines):
        nonlocal runs
        runs += 1
        write_ops(ops_path, header, lines)
        for path in (image, trace, manifest):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        command = [
            args.exe,
            "--ops-file", ops_path,
            "--steps", str(len(lines)),
            "--image", image,
            "--trace", trace,
            "--manifest", manifest,
            "--profile", header.get("profile", "modern"),
            "--workload", header.get("workload", "mixed"),
            "--check-every", "10",
            "--consistency-every", "0",
            "--quiet",
        ]
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
        except subprocess.TimeoutExpired:
            return False
        output = result.stdout.decode("utf-8", "replace")
        if "model rejected" in output or "operation file line" in output:
            return False
        return result.returncode != 0 and args.signature in output

    if not reproduces(original):
        raise SystemExit("reduce: the original failure does not reproduce")

    current = list(original)
    granularity = 2
    while len(current) >= 2 and runs < args.max_runs:
        chunk = max(1, math.ceil(len(current) / granularity))
        reduced = False
        for start in range(0, len(current), chunk):
            if runs >= args.max_runs:
                break
            candidate = current[:start] + current[start + chunk:]
            if not candidate:
                continue
            if reproduces(candidate):
                current = candidate
                granularity = max(2, granularity - 1)
                reduced = True
                break
        if not reduced:
            if granularity >= len(current):
                break
            granularity = min(len(current), granularity * 2)

    if not reproduces(current):
        raise SystemExit("reduce: internal error, final candidate stopped failing")

    reduced_ops = os.path.join(work, "reduced.ops")
    write_ops(reduced_ops, header, current)
    print("FUZZ-2 reduced: %d -> %d operations (%d runs)" %
          (len(original), len(current), runs))
    print("ops: %s" % reduced_ops)
    print("reproduce:")
    print("%s --ops-file %s --steps %d --image %s --trace %s --manifest %s "
          "--profile %s --workload %s --check-every 10 "
          "--consistency-every 0" %
          (args.exe, reduced_ops, len(current),
           os.path.join(work, "repro.img"),
           os.path.join(work, "repro.trace.txt"),
           os.path.join(work, "repro.manifest.json"),
           header.get("profile", "modern"),
           header.get("workload", "mixed")))
    if not args.work_dir:
        # Keep the artifact directory; print it rather than deleting it.
        print("artifacts: %s" % work)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except AssertionError as error:
        print("reduce: FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
