#!/usr/bin/env python3
"""Build a JSON manifest of a mounted XFS tree for differential testing.

Usage: make_manifest.py <mount-point> <output.json>

Each entry records the relative POSIX path, type (dir/file/symlink), size,
sha256 for regular files and the target for symlinks. The manifest is used by
the Windows reader test (tests/scripts/verify_reader_windows.ps1) to prove that
file contents and names match what Linux wrote.
"""

import hashlib
import json
import os
import stat
import sys


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    root, output = sys.argv[1], sys.argv[2]
    root = os.path.abspath(root)

    entries = []

    def add_dir(rel: str) -> None:
        entries.append({"path": rel, "type": "dir", "size": 0})

    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir != ".":
            add_dir(rel_dir.replace(os.sep, "/"))

        # os.walk lists symlinks to directories in dirnames; record them as
        # symlinks and do not descend (followlinks=False).
        for name in list(dirnames):
            full = os.path.join(dirpath, name)
            if os.path.islink(full):
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                entries.append(
                    {
                        "path": rel,
                        "type": "symlink",
                        "target": os.readlink(full),
                        "size": os.lstat(full).st_size,
                    }
                )
                dirnames.remove(name)

        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            st = os.lstat(full)
            if stat.S_ISLNK(st.st_mode):
                entries.append(
                    {
                        "path": rel,
                        "type": "symlink",
                        "target": os.readlink(full),
                        "size": st.st_size,
                    }
                )
            elif stat.S_ISREG(st.st_mode):
                entries.append(
                    {
                        "path": rel,
                        "type": "file",
                        "size": st.st_size,
                        "sha256": sha256_file(full),
                    }
                )

    entries.sort(key=lambda e: e["path"])
    with open(output, "w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False, indent=1)
        f.write("\n")

    files = sum(1 for e in entries if e["type"] == "file")
    dirs = sum(1 for e in entries if e["type"] == "dir")
    links = sum(1 for e in entries if e["type"] == "symlink")
    print(f"manifest: {files} files, {dirs} directories, {links} symlinks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
