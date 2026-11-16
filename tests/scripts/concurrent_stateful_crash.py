#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Record, replay, recover and independently inspect concurrent FUZZ-2 crashes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import tempfile


# Stable FUZZ-2 identities: file1=1, file2=2. The data crash modifies file1;
# file2 and its xattr remain unrelated state checked strictly by the oracle.
OPS = """# fixed FUZZ-2 replay plan
seed=42
profile=modern
workload=mixed
create\t1\t0\t0\t0\t6631\t\t0\t0\t0\t0\t\t0\t0\t0\t0\t
create\t2\t0\t0\t0\t6632\t\t0\t0\t0\t0\t\t0\t0\t0\t0\t
hardlink\t2\t0\t0\t0\t\t6633\t0\t0\t0\t0\t\t0\t0\t0\t0\t
setxattr\t2\t0\t0\t0\t\t\t0\t0\t0\t0\t757365722e7332\t239788948\t8\t0\t0\t
write\t1\t0\t0\t0\t\t\t0\t4096\t0\t1015077638\t\t0\t0\t0\t0\t
rename\t0\t0\t0\t0\t6632\t6634\t0\t0\t0\t0\t\t0\t0\t0\t0\t
"""
CRASH_EXIT = 0x58435348 & 0xff


def remove_if_exists(path):
    if path.exists():
        path.unlink()


def invoke(command, log, expected=None):
    with open(log, "wb") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if expected is not None and result.returncode != expected:
        raise SystemExit("concurrent crash: expected exit %s, got %s (see %s)" %
                         (expected, result.returncode, log))
    if expected is None and result.returncode:
        raise SystemExit("concurrent crash: command failed (%s), see %s" %
                         (result.returncode, log))
    return result.returncode


def base_command(exe, ops, image, trace, manifest, steps, crash_op, point, observer, seed):
    return [exe, "--seed", str(seed), "--steps", str(steps), "--ops-file", str(ops),
            "--image", str(image), "--trace", str(trace), "--manifest", str(manifest),
            "--profile", "modern", "--workload", "mixed", "--crash-op", str(crash_op),
            "--crash-point", point, "--concurrent-observer", observer, "--quiet"]


def mutate_manifest_negative(exe, image, manifest, work):
    """Prove the raw oracle still rejects unrelated expected-state corruption."""
    original = json.loads(manifest.read_text(encoding="utf-8"))
    states = original.get("states", [])
    if not states:
        raise SystemExit("concurrent crash: recovery manifest has no states")
    state_entries = [state.get("entries", []) for state in original["states"]]
    file_node = next((node for entries in state_entries for node in entries
                      if "/f3" in node.get("paths", [])), None)
    if file_node is None or len(file_node.get("paths", [])) < 2:
        raise SystemExit("concurrent crash: manifest lacks the expected hard link")

    mutations = {
        "file": lambda node: node.__setitem__("size", int(node.get("size", 0)) + 1),
        "link": lambda node: node.__setitem__("nlink", int(node.get("nlink", 0)) + 1),
        "xattr": lambda node: node["xattrs"][0].__setitem__(
            "value_fnv", "0000000000000000" if node["xattrs"][0].get("value_fnv") != "0000000000000000"
            else "ffffffffffffffff"),
    }
    oracle = Path(__file__).parents[1] / "tools" / "stateful_probe.py"
    for kind, mutate in mutations.items():
        candidate = json.loads(json.dumps(original))
        targets = [next(node for node in state["entries"]
                        if "/f3" in node.get("paths", []))
                   for state in candidate["states"]]
        if kind == "xattr" and any(not target.get("xattrs") for target in targets):
            raise SystemExit("concurrent crash: fixture file has no xattr")
        for target in targets:
            mutate(target)
        bad = work / ("negative-unrelated-%s.manifest.json" % kind)
        bad.write_text(json.dumps(candidate), encoding="utf-8")
        command = [sys.executable, "-I", str(oracle), str(image),
                   "--manifest", str(bad), "--quiet"]
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        output = result.stdout.decode("utf-8", "replace")
        (work / ("negative-unrelated-%s.log" % kind)).write_text(output, encoding="utf-8")
        bad.unlink()
        if result.returncode == 0 or "stateful probe: FAIL:" not in output:
            raise SystemExit("concurrent crash: unrelated %s mutation did not produce a semantic oracle failure" % kind)


def run_case(exe, ops, work, crash_op, point, observer, seed, case_index,
             kernel_replay, kernel_recovered):
    case = work / ("seed%d-case%d-op%d-%s-%s" %
                   (seed, case_index, crash_op, point, observer))
    case.mkdir()
    image = case / "crash.img"
    trace = case / "trace.txt"
    manifest = case / "manifest.json"
    schedule = case / "recorded.schedule"
    steps = 6
    command = base_command(exe, ops, image, trace, manifest, steps,
                           crash_op, point, observer, seed)
    invoke(command + ["--schedule-out", str(schedule)], case / "record.log", CRASH_EXIT)
    choices = schedule.read_text(encoding="utf-8").splitlines()
    if not choices or any(not choice.isdecimal() for choice in choices):
        raise SystemExit("concurrent crash: recorded schedule is incomplete")

    # Preserve a byte-for-byte dirty image for the optional independent kernel
    # journal replay. The project recovery run uses the original image below.
    dirty_copy = case / "kernel-replay.img"
    if kernel_replay:
        shutil.copyfile(image, dirty_copy)
        verifier = Path(__file__).with_name("verify_stateful_linux.sh")
        invoke([str(verifier), str(dirty_copy), str(manifest), str(case / "kernel"),
                "--kernel-replay"], case / "kernel-replay.log")
        dirty_copy.unlink()

    image.unlink()
    invoke(command + ["--schedule-file", str(schedule)], case / "replay.log", CRASH_EXIT)
    recover = [exe, "--seed", str(seed), "--steps", str(steps), "--ops-file", str(ops),
               "--image", str(image), "--trace", str(trace), "--manifest", str(manifest),
               "--profile", "modern", "--workload", "mixed", "--crash-op", str(crash_op),
               "--crash-point", point, "--recover", "--quiet"]
    invoke(recover, case / "recover.log")

    probe = [sys.executable, "-I", str(Path(__file__).parents[1] / "tools" / "stateful_probe.py"),
             str(image), "--manifest", str(manifest), "--quiet"]
    invoke(probe, case / "oracle.log")
    mutate_manifest_negative(exe, image, manifest, case)
    repair = shutil.which("xfs_repair")
    if not repair:
        raise SystemExit("concurrent crash: xfs_repair is required for this matrix")
    invoke([repair, "-n", str(image)], case / "xfs_repair.log")
    if kernel_recovered:
        verifier = Path(__file__).with_name("verify_stateful_linux.sh")
        invoke([str(verifier), str(image), str(manifest), str(case / "kernel-recovered")],
               case / "kernel-recovered.log")
    image.unlink()
    print("PASS concurrent crash op=%d point=%s observer=%s" %
          (crash_op, point, observer))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--work-dir", default=None)
    parser.add_argument("--mode", choices=("smoke", "soak"), default="smoke")
    parser.add_argument("--kernel-replay", action="store_true",
                        help="root-only replay of a preserved dirty image with the Linux kernel")
    parser.add_argument("--kernel-recovered", action="store_true",
                        help="root-only full Linux verification after project recovery")
    args = parser.parse_args(argv)
    exe = os.path.abspath(args.exe)
    work = (Path(args.work_dir).resolve() if args.work_dir else
            Path(tempfile.mkdtemp(prefix="concurrent-stateful-crash-")))
    if work.exists() and any(work.iterdir()):
        work = work / ("run-%d-%d" % (time.time_ns(), os.getpid()))
    work.mkdir(parents=True, exist_ok=True)
    ops = work / "fixed.ops"
    ops.write_text(OPS, encoding="utf-8")

    if args.mode == "smoke":
        cases = ((4, "after_log_commit", "flush"),
                 (6, "mid_metadata_write", "read"),
                 (5, "after_data_write", "read"),
                 (5, "before_log_write", "flush"))
        seeds = (42,)
    else:
        cases = ((4, "after_log_commit", "flush"),
                 (6, "mid_metadata_write", "read"),
                 (5, "after_data_write", "read"),
                 (5, "before_log_write", "flush"))
        seeds = (42, 0x2468ACE1)
    case_count = 0
    for seed in seeds:
        seed_ops = work / ("fixed-%d.ops" % seed)
        seed_ops.write_text(OPS.replace("seed=42", "seed=%d" % seed), encoding="utf-8")
        for case_index, (crash_op, point, observer) in enumerate(cases):
            run_case(exe, seed_ops, work, crash_op, point, observer, seed,
                     case_index, args.kernel_replay, args.kernel_recovered)
            case_count += 1

    # A successful ordinary replay with a valid schedule plus one extra choice
    # must fail after all operations, and leave both schedule and diagnostic.
    normal_image = work / "schedule-exhaustion.img"
    normal_trace = work / "schedule-exhaustion.trace.txt"
    normal_manifest = work / "schedule-exhaustion.manifest.json"
    normal_schedule = work / "schedule-exhaustion.schedule"
    normal = [exe, "--seed", "42", "--steps", "6", "--ops-file", str(ops),
              "--image", str(normal_image), "--trace", str(normal_trace),
              "--manifest", str(normal_manifest), "--profile", "modern",
              "--workload", "mixed", "--concurrent-observer", "read", "--quiet"]
    invoke(normal + ["--schedule-out", str(normal_schedule)], work / "schedule-record.log")
    valid_text = normal_schedule.read_text(encoding="utf-8")
    for label, contents, signature in (
            ("invalid", "0\nbad\n", "invalid schedule choice"),
            ("truncated", valid_text.rstrip("\n"), "schedule is empty or truncated")):
        bad_schedule = work / ("schedule-%s.schedule" % label)
        bad_schedule.write_text(contents, encoding="utf-8")
        remove_if_exists(normal_image)
        negative = subprocess.run(normal + ["--schedule-file", str(bad_schedule)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log_path = work / ("schedule-%s.log" % label)
        log_path.write_bytes(negative.stdout)
        if negative.returncode == 0 or signature.encode() not in negative.stdout:
            raise SystemExit("concurrent crash: %s schedule was not rejected as expected" % label)
        if normal_image.exists():
            raise SystemExit("concurrent crash: invalid schedule created an image")
    extended_schedule = work / "schedule-with-extra-choice.schedule"
    extended_schedule.write_text(normal_schedule.read_text(encoding="utf-8") + "0\n",
                                 encoding="utf-8")
    remove_if_exists(normal_image)
    failure_log = work / "schedule-extra-choice.log"
    result = subprocess.run(normal + ["--schedule-file", str(extended_schedule)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    failure_log.write_bytes(result.stdout)
    if result.returncode == 0 or b"unused trailing choices" not in result.stdout:
        raise SystemExit("concurrent crash: FUZZ-2 replay accepted a trailing choice")
    remove_if_exists(normal_image)
    print("PASS concurrent FUZZ-2 unused schedule choice rejected")
    print("PASS concurrent stateful crash matrix mode=%s cases=%d artifacts=%s" %
          (args.mode, case_count, work))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
