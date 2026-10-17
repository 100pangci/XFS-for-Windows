#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Content of the read-write interoperability image, generated independently.

The read-write CI round trip is:

    Windows format an image (read-write safe profile)
      -> Linux populates it with deterministic files      (populate)
        -> Windows mounts it read-write and overwrites bytes in two files
          -> Linux verifies every file and the overlays    (verify)
            -> xfs_repair -n must stay clean

This tool defines the file contents and the overlay placement, and is the only
implementation of them: the Windows job applies the overlay bytes it is given,
Linux regenerates the expected result from scratch. Nothing here reads the
Windows code, so a bug that changed file contents cannot hide.

usage:
  rw_content.py populate <mountpoint>
  rw_content.py verify <mountpoint> [--with-overlays]
  rw_content.py make-overlay <file>

exit code 0 = content matches.
"""

import json
import os
import sys

# Deterministic file content (same documented pattern as the C++ test fixture):
#   byte(i) = ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF
def pattern_byte(i: int) -> int:
    return ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF


def pattern(size: int) -> bytes:
    return bytes(pattern_byte(i) for i in range(size))


# Overlay written into existing allocated data by the Windows side.
#   byte(i) = (0xC3 ^ (i * 37)) & 0xFF
OVERLAY_SIZE = 4096


def overlay_byte(i: int) -> int:
    return (0xC3 ^ (i * 37)) & 0xFF


def overlay() -> bytes:
    return bytes(overlay_byte(i) for i in range(OVERLAY_SIZE))


# Files created on Linux. Sizes are deliberately not block aligned: an
# overwrite that lands in the last partial block must work as well.
FILES = [
    # path, size
    ("hello.bin", 32768),
    ("big.bin", 4 << 20),
    ("ro.txt", 512),
    ("sub/file.txt", 128),
    ("sub/data.bin", 8192 + 123),
]

# Where the Windows side overwrites bytes. "path" must be an FILES entry and
# offset + OVERLAY_SIZE must stay inside the file.
OVERLAYS = [
    # path, file offset, how Windows writes it
    ("hello.bin", 4096, "winfsp"),
    ("big.bin", 1 << 20, "pwrite"),
]

MODES = {
    "hello.bin": 0o644,
    "big.bin": 0o644,
    "ro.txt": 0o444,
    "sub": 0o755,
    "sub/file.txt": 0o644,
    "sub/data.bin": 0o644,
}

# Mode bits are checked on Linux only: Windows does not represent POSIX modes
# (the populate/verify steps in CI always run on Linux; locally the check is
# skipped so the tool can still be exercised).
CHECK_MODES = os.name != "nt"


def expected_content(path: str, with_overlay: bool) -> bytes:
    size = dict(FILES)[path]
    data = bytearray(pattern(size))
    if with_overlay:
        for overlay_path, offset, _ in OVERLAYS:
            if overlay_path == path:
                data[offset:offset + OVERLAY_SIZE] = overlay()
    return bytes(data)


def populate(mnt: str) -> int:
    for path, size in FILES:
        full = os.path.join(mnt, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(pattern(size))
    for path, mode in MODES.items():
        os.chmod(os.path.join(mnt, path), mode)
    print(f"populated {mnt}: {len(FILES)} files "
          f"({sum(size for _, size in FILES)} bytes)")
    return 0


def verify(mnt: str, with_overlays: bool) -> int:
    failures = []
    for path, size in FILES:
        full = os.path.join(mnt, path)
        if not os.path.isfile(full):
            failures.append(f"{path}: missing")
            continue
        actual = open(full, "rb").read()
        expect = expected_content(path, with_overlays)
        if len(actual) != len(expect):
            failures.append(f"{path}: size {len(actual)} != {len(expect)}")
            continue
        if actual != expect:
            first = next(i for i in range(len(actual)) if actual[i] != expect[i])
            failures.append(
                f"{path}: content differs at offset {first} "
                f"({actual[first]:#x} != {expect[first]:#x})")
            continue
        want_mode = MODES[path] & 0o777
        got_mode = os.stat(full).st_mode & 0o777
        if CHECK_MODES and got_mode != want_mode:
            failures.append(f"{path}: mode {got_mode:o} != {want_mode:o}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    suffix = " (with overlays applied)" if with_overlays else ""
    print(f"verified {mnt}: {len(FILES)} files match the documented content"
          f"{suffix}")
    return 0


def write_recipe(path: str) -> int:
    recipe = {
        "comment": "read-write interoperability recipe; see "
                   "tests/tools/rw_content.py for the content definition",
        "overlay_size": OVERLAY_SIZE,
        "overlays": [
            {"path": p, "offset": o, "via": via} for p, o, via in OVERLAYS
        ],
        "files": [{"path": p, "size": s} for p, s in FILES],
        "readonly_path": "ro.txt",
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(recipe, f, indent=1)
        f.write("\n")
    print(f"wrote {path}")
    return 0


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    command = argv[1]
    if command == "populate" and len(argv) == 3:
        return populate(argv[2])
    if command == "verify" and len(argv) in (3, 4):
        return verify(argv[2], "--with-overlays" in argv[3:])
    if command == "make-overlay" and len(argv) == 3:
        with open(argv[2], "wb") as f:
            f.write(overlay())
        print(f"wrote {argv[2]} ({OVERLAY_SIZE} bytes)")
        return 0
    if command == "recipe" and len(argv) == 3:
        return write_recipe(argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
