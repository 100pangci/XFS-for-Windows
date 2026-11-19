#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Run seeded Controller workloads and strict schedule replays.

The workload implementations are the real public-API stress tests registered
in conc_tests.cpp. Every seed gets a fresh run, persisted worker operation
catalog, scheduler choices and normalized event stream, followed by replay of
that exact choice list. This runner needs no root privileges or disk images.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def require_one_test(log_path):
    output = Path(log_path).read_text(encoding="utf-8", errors="replace")
    matches = re.findall(r"\((\d+) tests run\)", output)
    if not matches or matches[-1] != "1":
        raise SystemExit("matrix: expected exactly one selected test in %s" % log_path)
    return output


SCENARIOS = (
    ("conc_stress_mixed_namespace_data_xattr_seeded", "mixed"),
    ("conc_stress_shared_names_history_is_legal", "shared"),
    ("conc_stress_cow_both_sides_seeded", "cow"),
)


def run_case(exe, work, scenario, label, seed, iterations):
    prefix = work / ("%s-%s-%s" % (label, scenario, seed))
    env = os.environ.copy()
    env["XFSWIN_CONC_STRESS_SEED"] = str(seed)
    env["XFSWIN_CONC_STRESS_ITERS"] = str(iterations)
    env["XFSWIN_CONC_ARTIFACT_PREFIX"] = str(prefix)
    env.pop("XFSWIN_CONC_REPLAY", None)
    command = [exe, "--filter=" + scenario]
    log_path = Path(str(prefix) + ".%s.log" % label)
    with open(log_path, "wb") as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True)
    require_one_test(log_path)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, help="path to xfs_tests")
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--mode", choices=("smoke", "soak"), default="smoke")
    args = parser.parse_args(argv)
    exe = os.path.abspath(args.exe)
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)
    seeds = (0x13579BDF,) if args.mode == "smoke" else (
        0x13579BDF, 0x2468ACE0, 0xC001D00D, 0x9E3779B9)
    iterations = 2 if args.mode == "smoke" else 20

    valid_schedules = {}
    for scenario, label in SCENARIOS:
        for seed in seeds:
            run_case(exe, work, scenario, "seeded", seed, iterations)
            prefix = work / ("seeded-%s-%s" % (scenario, seed))
            # Artifact scenario labels map to the workload's human name; locate
            # the unique schedule using its prefix to avoid assuming that name.
            candidates = list(work.glob(prefix.name + ".*.schedule"))
            if len(candidates) != 1:
                raise SystemExit("matrix: expected one schedule artifact for %s" % scenario)
            schedule = candidates[0].read_text(encoding="utf-8").strip()
            valid_schedules[scenario, seed] = schedule
            if not schedule:
                raise SystemExit("matrix: empty schedule for %s" % scenario)
            replay_prefix = work / ("replay-%s-%s" % (scenario, seed))
            env = os.environ.copy()
            env["XFSWIN_CONC_STRESS_SEED"] = str(seed)
            env["XFSWIN_CONC_STRESS_ITERS"] = str(iterations)
            env["XFSWIN_CONC_ARTIFACT_PREFIX"] = str(replay_prefix)
            env["XFSWIN_CONC_REPLAY"] = schedule
            replay_log = Path(str(replay_prefix) + ".log")
            with open(replay_log, "wb") as log:
                subprocess.run([exe, "--filter=" + scenario], env=env,
                               stdout=log, stderr=subprocess.STDOUT, check=True)
            require_one_test(replay_log)
            seed_events = candidates[0].with_suffix(".events.txt").read_bytes()
            replay_events = next(work.glob(replay_prefix.name + ".*.events.txt"))
            if seed_events != replay_events.read_bytes():
                raise SystemExit("matrix: replay event stream differs for %s seed %d" %
                                 (scenario, seed))
            print("PASS %s seed=%d steps=%d" %
                  (scenario, seed, len(schedule.split(","))))

    # Malformed values and valid-but-unused trailing choices must fail closed.
    scenario = SCENARIOS[0][0]
    negative_env = os.environ.copy()
    negative_env["XFSWIN_CONC_STRESS_SEED"] = str(seeds[0])
    negative_env["XFSWIN_CONC_STRESS_ITERS"] = str(iterations)
    negative_env["XFSWIN_CONC_ARTIFACT_PREFIX"] = str(work / "negative-invalid")
    negative_env["XFSWIN_CONC_REPLAY"] = "0,not-an-index"
    negative_log = work / "negative-invalid-schedule.log"
    with open(negative_log, "wb") as log:
        result = subprocess.run([exe, "--filter=" + scenario], env=negative_env,
                                stdout=log, stderr=subprocess.STDOUT)
    output = require_one_test(negative_log)
    if result.returncode == 0 or "invalid choice" not in output:
        raise SystemExit("matrix: malformed replay schedule was not rejected")

    valid_schedule = valid_schedules[scenario, seeds[0]]
    extra_env = os.environ.copy()
    extra_env["XFSWIN_CONC_STRESS_SEED"] = str(seeds[0])
    extra_env["XFSWIN_CONC_STRESS_ITERS"] = str(iterations)
    extra_env["XFSWIN_CONC_ARTIFACT_PREFIX"] = str(work / "negative-extra")
    extra_env["XFSWIN_CONC_REPLAY"] = valid_schedule + ",0"
    extra_log = work / "negative-extra-schedule.log"
    with open(extra_log, "wb") as log:
        result = subprocess.run([exe, "--filter=" + scenario], env=extra_env,
                                stdout=log, stderr=subprocess.STDOUT)
    output = require_one_test(extra_log)
    if result.returncode == 0 or "unused trailing choices" not in output:
        raise SystemExit("matrix: extra replay choices were not rejected")
    print("PASS malformed and trailing choices rejected")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except subprocess.CalledProcessError as error:
        raise SystemExit("matrix: command failed (%s); see its log in --work-dir" %
                         error.returncode)
