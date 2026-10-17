#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Checks a crash image *after* Linux has replayed its log and mounted it.

The window contract of the read-write writer is:

  * a crash before the log record is durable leaves the old timestamps,
  * a crash after it leaves the old timestamps *on disk* but the committed
    record is replayed by the kernel, so the mounted filesystem shows the new
    ones - the kernel has to apply a record this project wrote,
  * file data is never journalled, so it is old or new but never garbage,
  * the mounted filesystem stays consistent (xfs_repair -n is run separately).

This tool compares the mounted file against both possible states. The "new"
timestamp is not guessed: it is read out of the complete log record in the
image (that is what the kernel replays), and the expected file contents come
from tests/tools/rw_content.py, the same definition the Windows side used.

usage:
  rw_replay_check.py <mounted-file> <image> <pristine-image> --ino <ino>
                     --allow-times old,new --allow-data old,new,mixed
                     [--data-offset 4096 --data-size 4096]

exit code 0 = the mounted state is one of the allowed ones.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_probe  # noqa: E402
import rw_content  # noqa: E402
import rw_crash_check  # noqa: E402


def logged_mtime(image: bytes, ino: int):
    """Timestamps the newest complete transaction record describes for `ino`."""
    _, summary = log_probe.verify("<image>", "any", image)
    for record in summary["records"]:
        for item in record["items"]:
            if item.get("type") == "inode" and item.get("ino") == ino:
                return (item["mtime"]["sec"], item["mtime"]["nsec"])
    return None


def main(argv) -> int:
    if len(argv) < 4:
        print(__doc__)
        return 2
    mounted, image_path, pristine_path = argv[1], argv[2], argv[3]
    ino = None
    allow_times = {"old", "new"}
    allow_data = {"old", "new", "mixed"}
    data_offset = 4096
    data_size = 4096
    index = 4
    while index < len(argv):
        arg = argv[index]
        value = argv[index + 1] if index + 1 < len(argv) else ""
        if arg == "--ino":
            ino = int(value)
        elif arg == "--allow-times":
            allow_times = set(value.split(","))
        elif arg == "--allow-data":
            allow_data = set(value.split(","))
        elif arg == "--data-offset":
            data_offset = int(value)
        elif arg == "--data-size":
            data_size = int(value)
        else:
            print(f"unknown argument {arg}", file=sys.stderr)
            return 2
        index += 2
    if ino is None:
        print("--ino is required", file=sys.stderr)
        return 2

    with open(image_path, "rb") as f:
        image = f.read()
    with open(pristine_path, "rb") as f:
        pristine = f.read()

    failures = []
    old = rw_crash_check.CrashState(pristine,
                                    rw_crash_check.Geometry(pristine), ino)
    logged = logged_mtime(image, ino)

    actual_ns = os.stat(mounted).st_mtime_ns
    old_ns = old.mtime[0] * 10**9 + old.mtime[1]
    new_ns = logged[0] * 10**9 + logged[1] if logged else None

    if actual_ns == old_ns:
        times = "old"
    elif new_ns is not None and actual_ns == new_ns:
        times = "new"
    else:
        times = "unknown"
    if times not in allow_times:
        failures.append(
            f"mounted mtime {actual_ns} is {times} (old={old_ns}, "
            f"logged={new_ns}); allowed: {sorted(allow_times)}")

    with open(mounted, "rb") as f:
        f.seek(data_offset)
        payload = f.read(data_size)
    pattern = rw_content.pattern(data_offset + data_size)
    old_data = pattern[data_offset:data_offset + data_size]
    if payload == rw_content.overlay():
        data = "new"
    elif payload == old_data:
        data = "old"
    else:
        data = "mixed"
    if data not in allow_data:
        failures.append(f"file data is {data}; allowed: {sorted(allow_data)}")

    print(f"{os.path.basename(mounted)}: size {os.path.getsize(mounted)}, "
          f"mtime {times} ({actual_ns}), data {data}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
