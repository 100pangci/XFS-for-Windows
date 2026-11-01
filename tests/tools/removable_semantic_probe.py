#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Validate the DEV-1 removable-media manifest and mounted XFS contents.

The manifest is the Windows/Linux v1 contract. The script has no project
imports and supports three modes:
  --validate-only                 validate schema and cross-field consistency
  --preflight-device DEVICE       verify explicit block-device identity/safety
  MOUNTPOINT                      compare mounted contents with the manifest
"""

from __future__ import annotations

import argparse
import datetime
import errno
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any


class ProbeError(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise ProbeError(message)


def require_object(value: Any, where: str, keys: set[str]) -> dict[str, Any]:
    check(isinstance(value, dict), f"{where} must be an object")
    missing = keys - set(value)
    extra = set(value) - keys
    check(not missing, f"{where} missing fields: {', '.join(sorted(missing))}")
    check(not extra, f"{where} has unknown fields: {', '.join(sorted(extra))}")
    return value


def require_string(value: Any, where: str, nonempty: bool = True,
                   allow_nul: bool = False) -> str:
    check(isinstance(value, str), f"{where} must be a string")
    check(not nonempty or bool(value), f"{where} must not be empty")
    check(allow_nul or "\x00" not in value, f"{where} contains NUL")
    return value


def require_int(value: Any, where: str, minimum: int = 0) -> int:
    check(type(value) is int, f"{where} must be an integer")
    check(value >= minimum, f"{where} must be >= {minimum}")
    return value


def require_sha256(value: Any, where: str) -> str:
    value = require_string(value, where)
    check(re.fullmatch(r"[0-9a-fA-F]{64}", value) is not None,
          f"{where} must be a 64-character SHA-256 hex digest")
    return value.lower()


def validate_path(value: Any, where: str, allow_root: bool = False) -> str:
    value = require_string(value, where, nonempty=not allow_root)
    if value == "" and allow_root:
        return value
    check(not value.startswith("/"), f"{where} must be relative")
    check("\\" not in value, f"{where} must use POSIX separators")
    parts = value.split("/")
    check(all(part not in ("", ".", "..") for part in parts),
          f"{where} has an empty, '.' or '..' path component")
    return value


def reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProbeError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def reject_json_constant(value: str) -> None:
    raise ProbeError(f"invalid JSON constant: {value}")


def load_json(path: str) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=reject_duplicate_pairs,
                             parse_constant=reject_json_constant)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ProbeError(f"cannot read manifest {path}: {exc}") from exc


def normalize_model(value: str) -> str:
    return " ".join(value.strip().split()).casefold()


def validate_manifest(path: str) -> tuple[dict[str, Any], dict[str, Any]]:
    data = load_json(path)
    root = require_object(data, "manifest", {
        "schema_version", "kind", "created_utc", "identity", "filesystem",
        "marker", "files", "truncate_results", "renames", "hardlink_groups",
        "xattrs", "xattr_roundtrip", "directories", "absent_paths",
    })
    check(type(root["schema_version"]) is int and root["schema_version"] == 1,
          "schema_version must be 1")
    check(root["kind"] == "xfswin-dev1-removable",
          'kind must be "xfswin-dev1-removable"')
    created_utc = require_string(root["created_utc"], "created_utc")
    try:
        parsed_created = datetime.datetime.fromisoformat(created_utc.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ProbeError("created_utc must be a valid ISO-8601 timestamp") from exc
    check(parsed_created.tzinfo is not None,
          "created_utc must include a timezone")

    identity = require_object(root["identity"], "identity", {
        "disk_size_bytes", "serial", "usb_serial", "windows_model",
        "linux_model_names", "bus_type", "media_type", "is_removable",
        "is_system", "is_boot", "contains_pagefile",
    })
    require_int(identity["disk_size_bytes"], "identity.disk_size_bytes", 1)
    require_string(identity["serial"], "identity.serial")
    require_string(identity["usb_serial"], "identity.usb_serial")
    check(re.fullmatch(r"[A-Za-z0-9._-]{1,64}", identity["usb_serial"]) is not None,
          "identity.usb_serial is not a plain USB serial number")
    require_string(identity["windows_model"], "identity.windows_model")
    check(identity["disk_size_bytes"] == 64160400896,
          "identity.disk_size_bytes does not match the qualified removable disk")
    check(identity["serial"] == "AA00000000000489",
          "identity.serial does not match the qualified removable disk")
    check(identity["windows_model"] == "Samsung Flash Drive USB Device",
          "identity.windows_model does not match the qualified removable disk")
    aliases = identity["linux_model_names"]
    check(isinstance(aliases, list) and aliases,
          "identity.linux_model_names must be a non-empty array")
    for index, alias in enumerate(aliases):
        require_string(alias, f"identity.linux_model_names[{index}]")
        check(bool(normalize_model(alias)),
              f"identity.linux_model_names[{index}] is empty after normalization")
    check(len({normalize_model(alias) for alias in aliases}) == len(aliases),
          "identity.linux_model_names contains duplicate normalized names")
    check(isinstance(identity["bus_type"], str)
          and identity["bus_type"].casefold() == "usb",
          'identity.bus_type must be "USB"')
    check(isinstance(identity["media_type"], str)
          and " ".join(identity["media_type"].split()).casefold() == "removable media",
          'identity.media_type must be "Removable Media"')
    check(identity["is_removable"] is True,
          "identity.is_removable must be true")
    for name in ("is_system", "is_boot", "contains_pagefile"):
        check(identity[name] is False, f"identity.{name} must be false")

    filesystem = require_object(root["filesystem"], "filesystem", {
        "target_kind", "size_bytes", "offset_bytes", "partition_number", "fs_type",
    })
    check(filesystem["target_kind"] in ("disk", "partition"),
          'filesystem.target_kind must be "disk" or "partition"')
    size_bytes = require_int(filesystem["size_bytes"], "filesystem.size_bytes", 1)
    offset_bytes = require_int(filesystem["offset_bytes"], "filesystem.offset_bytes")
    check(filesystem["fs_type"] == "xfs", 'filesystem.fs_type must be "xfs"')
    if filesystem["target_kind"] == "partition":
        require_int(filesystem["partition_number"], "filesystem.partition_number", 1)
    if filesystem["target_kind"] == "disk":
        check(filesystem["partition_number"] is None,
              "whole-disk filesystem.partition_number must be null")
        check(offset_bytes == 0,
              "whole-disk filesystem.offset_bytes must be zero")
        check(size_bytes == identity["disk_size_bytes"],
              "whole-disk filesystem.size_bytes must equal identity.disk_size_bytes")
    else:
        require_int(filesystem["partition_number"],
                    "filesystem.partition_number", 1)
        check(offset_bytes > 0, "partition filesystem.offset_bytes must be positive")
        check(size_bytes < identity["disk_size_bytes"],
              "partition filesystem.size_bytes must be smaller than the parent disk")
        check(offset_bytes + size_bytes <= identity["disk_size_bytes"],
              "partition filesystem range exceeds identity.disk_size_bytes")

    files_value = root["files"]
    check(isinstance(files_value, list), "files must be an array")
    files: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(files_value):
        where = f"files[{index}]"
        item = require_object(item, where, {"path", "size_bytes", "sha256"})
        file_path = validate_path(item["path"], f"{where}.path")
        check(file_path not in files, f"duplicate files path: {file_path}")
        size = require_int(item["size_bytes"], f"{where}.size_bytes")
        digest = require_sha256(item["sha256"], f"{where}.sha256")
        files[file_path] = {"size_bytes": size, "sha256": digest}

    marker = require_object(root["marker"], "marker", {
        "path", "content_utf8", "sha256",
    })
    marker_path = validate_path(marker["path"], "marker.path")
    marker_bytes = require_string(marker["content_utf8"], "marker.content_utf8",
                                  allow_nul=True).encode("utf-8")
    marker_digest = require_sha256(marker["sha256"], "marker.sha256")
    check(hashlib.sha256(marker_bytes).hexdigest() == marker_digest,
          "marker.sha256 does not match marker.content_utf8")
    check(marker_path in files, "marker.path must appear in files")
    check(files[marker_path]["size_bytes"] == len(marker_bytes),
          "marker content length contradicts files entry")
    check(files[marker_path]["sha256"] == marker_digest,
          "marker digest contradicts files entry")

    directories_value = root["directories"]
    check(isinstance(directories_value, list), "directories must be an array")
    directories: dict[str, dict[str, str]] = {}
    for index, item in enumerate(directories_value):
        where = f"directories[{index}]"
        item = require_object(item, where, {"path", "children"})
        dir_path = validate_path(item["path"], f"{where}.path", allow_root=True)
        check(dir_path not in directories, f"duplicate directory path: {dir_path!r}")
        children_value = item["children"]
        check(isinstance(children_value, list), f"{where}.children must be an array")
        children: dict[str, str] = {}
        for child_index, child in enumerate(children_value):
            child_where = f"{where}.children[{child_index}]"
            child = require_object(child, child_where, {"name", "type"})
            name = require_string(child["name"], f"{child_where}.name")
            check("/" not in name and "\\" not in name and name not in (".", ".."),
                  f"{child_where}.name must be one POSIX path component")
            check(child["type"] in ("file", "directory"),
                  f"{child_where}.type must be file or directory")
            check(name not in children, f"duplicate child name in {dir_path!r}: {name}")
            children[name] = child["type"]
        directories[dir_path] = children
    check("" in directories, 'directories must include the root directory with path ""')

    declared_dirs = set(directories)
    declared_files = set(files)
    check(not (declared_dirs & declared_files), "a path cannot be both a file and directory")
    all_objects = declared_dirs | declared_files
    for object_path in all_objects:
        if object_path == "":
            continue
        parent = object_path.rpartition("/")[0]
        check(parent in declared_dirs,
              f"parent directory {parent!r} is missing for {object_path!r}")
        check(parent in directories,
              f"parent directory {parent!r} has no children declaration")
    for dir_path, children in directories.items():
        expected: dict[str, str] = {}
        for object_path in all_objects:
            if object_path == "":
                continue
            parent, _, name = object_path.rpartition("/")
            if parent == dir_path:
                expected[name] = "directory" if object_path in declared_dirs else "file"
        check(children == expected,
              f"directories entry {dir_path!r} contradicts the declared file/directory paths")

    truncate = require_object(root["truncate_results"], "truncate_results", {
        "shrink", "shrink_then_grow",
    })
    shrink = require_object(truncate["shrink"], "truncate_results.shrink", {
        "path", "prefix_size_bytes", "prefix_sha256",
    })
    shrink_path = validate_path(shrink["path"], "truncate_results.shrink.path")
    shrink_size = require_int(shrink["prefix_size_bytes"],
                              "truncate_results.shrink.prefix_size_bytes")
    shrink_digest = require_sha256(shrink["prefix_sha256"],
                                   "truncate_results.shrink.prefix_sha256")
    check(shrink_path in files, "truncate shrink path must appear in files")
    check(files[shrink_path] == {"size_bytes": shrink_size, "sha256": shrink_digest},
          "truncate shrink result contradicts files entry")

    grow = require_object(truncate["shrink_then_grow"],
                          "truncate_results.shrink_then_grow", {
        "path", "final_size_bytes", "final_sha256", "preserved_prefix_bytes",
        "preserved_prefix_sha256", "zero_range",
    })
    grow_path = validate_path(grow["path"], "truncate_results.shrink_then_grow.path")
    final_size = require_int(grow["final_size_bytes"],
                             "truncate_results.shrink_then_grow.final_size_bytes")
    final_digest = require_sha256(grow["final_sha256"],
                                  "truncate_results.shrink_then_grow.final_sha256")
    prefix_size = require_int(grow["preserved_prefix_bytes"],
                              "truncate_results.shrink_then_grow.preserved_prefix_bytes")
    prefix_digest = require_sha256(grow["preserved_prefix_sha256"],
                                   "truncate_results.shrink_then_grow.preserved_prefix_sha256")
    zero_range = require_object(grow["zero_range"],
                                "truncate_results.shrink_then_grow.zero_range", {
        "offset_bytes", "length_bytes",
    })
    zero_offset = require_int(zero_range["offset_bytes"],
                              "truncate_results.shrink_then_grow.zero_range.offset_bytes")
    zero_length = require_int(zero_range["length_bytes"],
                              "truncate_results.shrink_then_grow.zero_range.length_bytes")
    check(grow_path in files, "truncate shrink_then_grow path must appear in files")
    check(shrink_path != grow_path, "truncate result paths must be distinct")
    check(files[grow_path] == {"size_bytes": final_size, "sha256": final_digest},
          "truncate shrink_then_grow result contradicts files entry")
    check(prefix_size <= final_size, "truncate preserved prefix exceeds final size")
    check(prefix_size > 0, "truncate preserved prefix must be non-empty")
    check(zero_length > 0, "truncate growth zero range must be non-empty")
    check(zero_offset == prefix_size and zero_length == final_size - prefix_size,
          "truncate growth zero_range must span exactly from the preserved prefix to EOF")

    absent_value = root["absent_paths"]
    check(isinstance(absent_value, list), "absent_paths must be an array")
    absent: set[str] = set()
    for index, value in enumerate(absent_value):
        absent_path = validate_path(value, f"absent_paths[{index}]")
        check(absent_path not in absent, f"duplicate absent path: {absent_path}")
        check(absent_path not in all_objects,
              f"absent path is also declared as an object: {absent_path}")
        absent.add(absent_path)
    for absent_path in absent:
        parent = absent_path.rpartition("/")[0]
        check(parent in declared_dirs,
              f"parent directory {parent!r} is missing for absent path {absent_path!r}")
        name = absent_path.rpartition("/")[2]
        check(name not in directories[parent],
              f"absent path appears as a declared child in {parent!r}: {name}")

    renames_value = root["renames"]
    check(isinstance(renames_value, list), "renames must be an array")
    rename_sources: set[str] = set()
    rename_targets: set[str] = set()
    for index, item in enumerate(renames_value):
        where = f"renames[{index}]"
        item = require_object(item, where, {"from", "to"})
        source = validate_path(item["from"], f"{where}.from")
        target = validate_path(item["to"], f"{where}.to")
        check(source != target, f"{where} source and target must differ")
        check(source not in rename_sources, f"duplicate rename source: {source}")
        check(target not in rename_targets, f"duplicate rename target: {target}")
        check(source in absent, f"rename source must appear in absent_paths: {source}")
        check(target in files, f"rename target must appear in files: {target}")
        rename_sources.add(source)
        rename_targets.add(target)

    hardlink_value = root["hardlink_groups"]
    check(isinstance(hardlink_value, list), "hardlink_groups must be an array")
    grouped_paths: set[str] = set()
    for index, item in enumerate(hardlink_value):
        where = f"hardlink_groups[{index}]"
        item = require_object(item, where, {"paths", "nlink"})
        paths_value = item["paths"]
        check(isinstance(paths_value, list) and len(paths_value) >= 2,
              f"{where}.paths must contain at least two paths")
        paths = [validate_path(value, f"{where}.paths[{n}]")
                 for n, value in enumerate(paths_value)]
        check(len(set(paths)) == len(paths), f"{where}.paths contains duplicates")
        nlink = require_int(item["nlink"], f"{where}.nlink", 2)
        check(nlink == len(paths), f"{where}.nlink must equal the complete paths list")
        for link_path in paths:
            check(link_path in files, f"hardlink path must appear in files: {link_path}")
            check(link_path not in grouped_paths,
                  f"path appears in multiple hardlink groups: {link_path}")
            grouped_paths.add(link_path)
        first = files[paths[0]]
        check(all(files[link_path] == first for link_path in paths[1:]),
              f"hardlink paths have contradictory file content: {paths}")

    xattrs_value = root["xattrs"]
    check(isinstance(xattrs_value, list), "xattrs must be an array")
    xattr_keys: set[tuple[str, str]] = set()
    for index, item in enumerate(xattrs_value):
        where = f"xattrs[{index}]"
        check(isinstance(item, dict), f"{where} must be an object")
        state = item.get("state")
        if state == "present":
            item = require_object(item, where, {
                "path", "name", "state", "value_hex", "length", "sha256",
            })
        elif state in ("absent", "removed"):
            item = require_object(item, where, {"path", "name", "state"})
        else:
            raise ProbeError(f"{where}.state must be present, absent, or removed")
        xattr_path = validate_path(item["path"], f"{where}.path")
        check(xattr_path in all_objects, f"{where}.path is not a declared object")
        name = require_string(item["name"], f"{where}.name")
        key = (xattr_path, name)
        check(key not in xattr_keys, f"duplicate xattr declaration: {key}")
        xattr_keys.add(key)
        if state == "present":
            value_hex = require_string(item["value_hex"], f"{where}.value_hex", nonempty=False)
            check(len(value_hex) % 2 == 0
                  and re.fullmatch(r"[0-9a-fA-F]*", value_hex) is not None,
                  f"{where}.value_hex must contain even-length hexadecimal bytes")
            value = bytes.fromhex(value_hex)
            length = require_int(item["length"], f"{where}.length")
            digest = require_sha256(item["sha256"], f"{where}.sha256")
            check(length == len(value), f"{where}.length contradicts value_hex")
            check(hashlib.sha256(value).hexdigest() == digest,
                  f"{where}.sha256 contradicts value_hex")

    roundtrip = require_object(root["xattr_roundtrip"], "xattr_roundtrip", {
        "path", "name", "value_hex",
    })
    roundtrip_path = validate_path(roundtrip["path"], "xattr_roundtrip.path")
    check(roundtrip_path in all_objects, "xattr_roundtrip.path is not a declared object")
    roundtrip_name = require_string(roundtrip["name"], "xattr_roundtrip.name")
    check(roundtrip_name.startswith("user."),
          "xattr_roundtrip.name must use the user. namespace")
    value_hex = require_string(roundtrip["value_hex"], "xattr_roundtrip.value_hex", nonempty=False)
    check(len(value_hex) % 2 == 0
          and re.fullmatch(r"[0-9a-fA-F]*", value_hex) is not None,
          "xattr_roundtrip.value_hex must contain even-length hexadecimal bytes")
    check((roundtrip_path, roundtrip_name) not in xattr_keys,
          "xattr_roundtrip attribute must not appear in xattrs")

    report = {
        "schema_version": 1,
        "kind": root["kind"],
        "identity": identity,
        "filesystem": filesystem,
        "manifest_path": os.path.realpath(path),
    }
    return root, report


def run_checked(args: list[str], where: str) -> str:
    try:
        result = subprocess.run(args, check=False, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise ProbeError(f"{where}: cannot execute {args[0]}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ProbeError(f"{where} failed ({result.returncode}): {detail}")
    return result.stdout


def node_children(node: dict[str, Any]) -> list[dict[str, Any]]:
    children = node.get("children") or []
    check(isinstance(children, list), "lsblk returned invalid children")
    return children


def flatten_nodes(nodes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for node in nodes:
        result.append(node)
        result.extend(flatten_nodes(node_children(node)))
    return result


def lsblk(path: str) -> list[dict[str, Any]]:
    columns = "NAME,KNAME,TYPE,SIZE,PKNAME,MODEL,SERIAL,TRAN,RM,PARTN,START"
    output = run_checked(["lsblk", "--json", "--bytes", "--paths",
                          "--output", columns, path], f"lsblk {path}")
    try:
        data = json.loads(output)
    except json.JSONDecodeError as exc:
        raise ProbeError(f"lsblk returned invalid JSON: {exc}") from exc
    nodes = data.get("blockdevices") if isinstance(data, dict) else None
    check(isinstance(nodes, list) and nodes,
          f"lsblk did not identify a block device for {path}")
    return flatten_nodes(nodes)


def sysfs_text(kname: str, attribute: str) -> str | None:
    path = Path("/sys/class/block") / kname / "device" / attribute
    try:
        value = path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None
    return value or None


def device_numbers(path: str) -> tuple[int, int]:
    try:
        st = os.stat(path)
    except OSError as exc:
        raise ProbeError(f"cannot stat block device {path}: {exc}") from exc
    check(stat.S_ISBLK(st.st_mode), f"{path} is not a block device")
    return os.major(st.st_rdev), os.minor(st.st_rdev)


def check_device_preflight(device: str, manifest: dict[str, Any], report: dict[str, Any]) -> dict[str, Any]:
    check(os.geteuid() == 0, "root privileges are required for block-device verification")
    supplied = os.path.realpath(device)
    target_st = os.stat(supplied)
    check(stat.S_ISBLK(target_st.st_mode), f"--device target is not a block device: {device}")
    target_nodes = lsblk(supplied)
    target = next((node for node in target_nodes
                   if node.get("name") and os.path.realpath(str(node["name"])) == supplied), None)
    check(target is not None, "lsblk does not contain the explicit target device")
    target_kind = str(target.get("type") or "")
    check(target_kind in ("disk", "part"),
          f"explicit target must be a disk or partition, got {target_kind!r}")

    if target_kind == "part":
        parent_name = target.get("pkname")
        check(isinstance(parent_name, str) and parent_name,
              "lsblk did not report the partition parent disk")
        parent_path = os.path.realpath(
            parent_name if os.path.isabs(parent_name) else os.path.join("/dev", parent_name))
        check(os.path.exists(parent_path), f"partition parent does not exist: {parent_path}")
        parent_nodes = lsblk(parent_path)
        parent = next((node for node in parent_nodes
                       if node.get("type") == "disk"
                       and node.get("name")
                       and os.path.realpath(str(node["name"])) == parent_path), None)
        check(parent is not None, "partition parent is not an identifiable whole disk")
    else:
        parent_path = supplied
        parent_nodes = lsblk(parent_path)
        parent = next((node for node in parent_nodes
                       if node.get("type") == "disk"
                       and node.get("name")
                       and os.path.realpath(str(node["name"])) == parent_path), None)
        check(parent is not None, "explicit disk target is not an identifiable whole disk")

    all_nodes = flatten_nodes([parent])
    tree_paths = {os.path.realpath(str(node.get("name")))
                  for node in all_nodes if node.get("name")}
    check(supplied in tree_paths, "explicit target is absent from its parent disk tree")
    disk_nodes = [node for node in all_nodes if node.get("type") == "disk"]
    check(len(disk_nodes) == 1, "parent disk tree must contain exactly one whole disk")
    disk = disk_nodes[0]
    partitions = [node for node in all_nodes if node.get("type") == "part"]
    target_partitions = [node for node in partitions
                         if node.get("name") and os.path.realpath(str(node["name"])) == supplied]
    check((target_kind == "part") == bool(target_partitions),
          "explicit target type contradicts the parent disk tree")
    check(len(target_partitions) <= 1,
          "explicit target path is ambiguous in the parent disk tree")
    if target_kind == "part":
        check(len(target_partitions) == 1,
              "explicit partition is missing from the parent disk tree")
    else:
        check(len(partitions) == 0,
              "whole-disk filesystem target must not be used when the disk has partitions")

    identity = manifest["identity"]
    filesystem = manifest["filesystem"]
    check(filesystem["target_kind"] == target_kind,
          "manifest filesystem.target_kind does not match the explicit target")
    try:
        parent_size = int(run_checked(["blockdev", "--getsize64", parent_path],
                                      "read parent disk size").strip())
        target_size = int(run_checked(["blockdev", "--getsize64", supplied],
                                      "read target device size").strip())
    except ValueError as exc:
        raise ProbeError("blockdev returned an invalid byte size") from exc
    check(parent_size == identity["disk_size_bytes"],
          f"parent disk size mismatch: {parent_size} != {identity['disk_size_bytes']}")
    check(target_size == filesystem["size_bytes"],
          f"target size mismatch: {target_size} != {filesystem['size_bytes']}")

    # util-linux reports KNAME as the full path when --paths is used, so keep
    # only the kernel name for the /sys/class/block lookups.
    target_kname = os.path.basename(str(target.get("kname") or ""))
    disk_kname = os.path.basename(str(disk.get("kname") or ""))
    check(bool(target_kname) and bool(disk_kname), "lsblk did not report kernel device names")
    if target_kind == "disk":
        check(filesystem["partition_number"] is None and filesystem["offset_bytes"] == 0,
              "whole-disk manifest partition metadata is invalid")
        check(target_size == parent_size,
              "whole-disk target size differs from the parent disk size")
        partition_number = None
        offset_bytes = 0
    else:
        part_dir = Path("/sys/class/block") / target_kname
        try:
            partition_number = int((part_dir / "partition").read_text().strip())
            start_sectors = int((part_dir / "start").read_text().strip())
        except (OSError, ValueError) as exc:
            raise ProbeError(f"cannot read Linux partition metadata: {exc}") from exc
        offset_bytes = start_sectors * 512
        check(partition_number == filesystem["partition_number"],
              f"partition number mismatch: {partition_number} != {filesystem['partition_number']}")
        check(offset_bytes == filesystem["offset_bytes"],
              f"partition start mismatch: {offset_bytes} != {filesystem['offset_bytes']}")
        check(offset_bytes + target_size <= parent_size,
              "target partition extends beyond the parent disk")

    actual_model = sysfs_text(disk_kname, "model") or str(disk.get("model") or "").strip() or None
    actual_serial = sysfs_text(disk_kname, "serial") or str(disk.get("serial") or "").strip() or None
    aliases = {normalize_model(alias) for alias in identity["linux_model_names"]}
    check(actual_model is not None, "Linux does not expose the parent disk model")
    normalized_actual_model = normalize_model(actual_model)
    check(bool(normalized_actual_model), "Linux exposed an empty parent disk model")
    # Serial and size are the authoritative pins.  Model strings differ between
    # Windows (full USB product string) and Linux (sysfs/SCSI product id, often
    # truncated), so accept an exact alias or a containment match after
    # whitespace/case normalization.
    model_matches = any(
        normalized_actual_model == alias
        or normalized_actual_model in alias
        or alias in normalized_actual_model
        for alias in aliases
        if alias
    )
    check(model_matches,
          f"parent model mismatch: {actual_model!r} is not compatible with manifest Linux model names")
    check(actual_serial is not None, "Linux does not expose the parent disk serial")
    # Windows reads the SCSI VPD serial through the disk; usb-storage exposes
    # the USB iSerial instead.  The manifest records both representations of
    # the same physical device, and exactly one must match.
    serial_forms = {
        "scsi_vpd": identity["serial"].strip(),
        "usb_iserial": identity["usb_serial"].strip(),
    }
    matched_serial_form = next(
        (name for name, value in serial_forms.items() if actual_serial.strip() == value),
        None,
    )
    check(matched_serial_form is not None,
          f"parent serial mismatch: {actual_serial!r} matches neither the SCSI VPD serial "
          f"({serial_forms['scsi_vpd']!r}) nor the USB iSerial ({serial_forms['usb_iserial']!r})")

    transport = str(disk.get("tran") or "").strip().casefold()
    if not transport:
        transport = (sysfs_text(disk_kname, "transport") or "").casefold()
    check(transport == "usb", f"parent disk transport is not USB: {transport or 'unavailable'}")
    removable_value = None
    try:
        removable_value = (Path("/sys/class/block") / disk_kname / "removable").read_text().strip()
    except OSError:
        pass
    check(removable_value == "1", "parent disk is not marked removable by Linux sysfs")

    node_numbers: dict[tuple[int, int], str] = {}
    checked_nodes = all_nodes
    for node in checked_nodes:
        node_path = str(node.get("name") or "")
        check(bool(node_path), "lsblk returned a parent/partition without a device path")
        numbers = device_numbers(node_path)
        node_numbers[numbers] = node_path
        holders = Path("/sys/dev/block") / f"{numbers[0]}:{numbers[1]}" / "holders"
        try:
            holder_names = sorted(entry.name for entry in os.scandir(holders))
        except OSError as exc:
            raise ProbeError(f"cannot inspect sysfs holders for {node_path}: {exc}") from exc
        check(not holder_names,
              f"device has sysfs holders ({', '.join(holder_names)}): {node_path}")

    mounted: list[str] = []
    try:
        with open("/proc/self/mountinfo", "r", encoding="utf-8") as stream:
            for line in stream:
                fields = line.split()
                if len(fields) >= 3 and fields[2].count(":") == 1:
                    major_text, minor_text = fields[2].split(":", 1)
                    try:
                        number = (int(major_text), int(minor_text))
                    except ValueError:
                        continue
                    if number in node_numbers:
                        mounted.append(f"{fields[2]} mounted at {fields[4] if len(fields) > 4 else '?'}")
    except OSError as exc:
        raise ProbeError(f"cannot read /proc/self/mountinfo: {exc}") from exc
    check(not mounted, "disk or partition is already mounted: " + "; ".join(mounted))

    active_swaps: list[str] = []
    try:
        with open("/proc/swaps", "r", encoding="utf-8") as stream:
            next(stream, None)
            for line in stream:
                fields = line.split()
                if not fields:
                    continue
                source = fields[0]
                try:
                    source_st = os.stat(source)
                except OSError:
                    continue
                if stat.S_ISBLK(source_st.st_mode):
                    number = (os.major(source_st.st_rdev), os.minor(source_st.st_rdev))
                    if number in node_numbers:
                        active_swaps.append(source)
    except OSError as exc:
        raise ProbeError(f"cannot read /proc/swaps: {exc}") from exc
    check(not active_swaps, "disk or partition is used as swap: " + ", ".join(active_swaps))

    root_major_minor = run_checked(
        ["findmnt", "--noheadings", "--raw", "--output", "MAJ:MIN", "--target", "/"],
        "find root filesystem source").strip().splitlines()
    root_mounted = False
    for value in root_major_minor:
        if ":" not in value:
            continue
        try:
            major_text, minor_text = value.split(":", 1)
            root_mounted |= (int(major_text), int(minor_text)) in node_numbers
        except ValueError:
            continue
    try:
        with open("/proc/self/mountinfo", "r", encoding="utf-8") as stream:
            for line in stream:
                fields = line.split()
                if len(fields) >= 5 and fields[4] == "/" and ":" in fields[2]:
                    major_text, minor_text = fields[2].split(":", 1)
                    try:
                        root_mounted |= (int(major_text), int(minor_text)) in node_numbers
                    except ValueError:
                        pass
    except OSError as exc:
        raise ProbeError(f"cannot inspect root mount source: {exc}") from exc
    check(not root_mounted, "parent disk is a source of the Linux root filesystem")

    try:
        fd = os.open(supplied, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
        try:
            first_sector = os.pread(fd, 512, 0)
        finally:
            os.close(fd)
    except OSError as exc:
        raise ProbeError(f"explicit block target is not readable: {exc}") from exc
    check(len(first_sector) == 512,
          "explicit block target did not provide a readable 512-byte first sector")

    identity_report = {
        "mode": "device",
        "target_path": supplied,
        "parent_disk_path": parent_path,
        "target_kind": target_kind,
        "parent_size_bytes": parent_size,
        "target_size_bytes": target_size,
        "partition_number": partition_number,
        "offset_bytes": offset_bytes,
        "parent_model": actual_model,
        "parent_serial": actual_serial,
        "matched_serial_form": matched_serial_form,
        "transport": transport,
        "linux_removable": removable_value,
        "tree_major_minor": {
            f"{major}:{minor}": path for (major, minor), path in sorted(node_numbers.items())
        },
        "tree_devices": [
            {"path": path, "major": major, "minor": minor}
            for (major, minor), path in sorted(node_numbers.items())
        ],
        "mounted_check": "clear",
        "swap_check": "clear",
        "holders_check": "clear",
        "root_filesystem_check": "clear",
        "read_only_probe": "512-byte read succeeded",
    }
    print("CHECK device type and explicit target: PASS")
    print("CHECK parent disk size and target size: PASS")
    print(f"CHECK parent serial ({matched_serial_form}) and normalized model alias: PASS")
    print("CHECK USB transport and removable flag: PASS")
    print("CHECK mounted devices, swaps, holders, and root source: PASS")
    print("CHECK target readability (read-only): PASS")
    return identity_report


def join_target(root: str, relative: str) -> str:
    return root if relative == "" else os.path.join(root, *relative.split("/"))


def file_sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def check_absent_xattr(path: str, name: str, where: str) -> None:
    try:
        os.getxattr(path, name)
    except OSError as exc:
        absent_codes = {errno.ENODATA}
        if hasattr(errno, "ENOATTR"):
            absent_codes.add(errno.ENOATTR)
        check(exc.errno in absent_codes,
              f"{where}: could not verify xattr absence: {exc}")
        return
    raise ProbeError(f"{where}: xattr unexpectedly exists")


def check_tree(root_path: str, files: dict[str, dict[str, Any]],
               directories: dict[str, dict[str, str]]) -> None:
    expected = set(files) | set(directories)
    actual: set[str] = {""}

    def visit(relative: str) -> None:
        path = join_target(root_path, relative)
        try:
            names = os.listdir(path)
        except OSError as exc:
            raise ProbeError(f"cannot list directory {relative or '/'}: {exc}") from exc
        for name in names:
            child = f"{relative}/{name}" if relative else name
            try:
                mode = os.stat(join_target(root_path, child), follow_symlinks=False).st_mode
            except OSError as exc:
                raise ProbeError(f"cannot stat tree entry {child}: {exc}") from exc
            if stat.S_ISDIR(mode):
                actual.add(child)
                visit(child)
            elif stat.S_ISREG(mode):
                actual.add(child)
            else:
                raise ProbeError(f"unsupported symlink or special object in XFS tree: {child}")

    visit("")
    extra = sorted(actual - expected)
    missing = sorted(expected - actual)
    check(not extra, f"unmanifested objects in XFS tree: {extra}")
    check(not missing, f"manifest objects missing from XFS tree: {missing}")
    print("CHECK complete tree has no undeclared or missing objects: PASS")


def check_semantics(mountpoint: str, manifest: dict[str, Any]) -> None:
    root_path = os.path.realpath(mountpoint)
    check(os.path.isdir(root_path), f"mountpoint is not a directory: {mountpoint}")

    files = {item["path"]: item for item in manifest["files"]}
    directories = {item["path"]: {child["name"]: child["type"]
                                    for child in item["children"]}
                   for item in manifest["directories"]}

    # Reject symlinks and undeclared objects before following any manifest path.
    check_tree(root_path, files, directories)

    for relative, expected in files.items():
        path = join_target(root_path, relative)
        try:
            st = os.stat(path, follow_symlinks=False)
        except OSError as exc:
            raise ProbeError(f"file missing or unreadable: {relative}: {exc}") from exc
        check(stat.S_ISREG(st.st_mode), f"manifest file is not a regular file: {relative}")
        check(st.st_size == expected["size_bytes"],
              f"file size mismatch for {relative}: {st.st_size} != {expected['size_bytes']}")
        actual_digest = file_sha256(path)
        check(actual_digest == expected["sha256"],
              f"SHA-256 mismatch for {relative}: {actual_digest} != {expected['sha256']}")
        print(f"CHECK file {relative}: PASS ({st.st_size} bytes, sha256 {actual_digest})")

    marker = manifest["marker"]
    marker_path = join_target(root_path, marker["path"])
    with open(marker_path, "rb") as stream:
        marker_actual = stream.read()
    expected_marker = marker["content_utf8"].encode("utf-8")
    check(marker_actual == expected_marker,
          f"marker UTF-8 bytes mismatch for {marker['path']}")
    check(hashlib.sha256(marker_actual).hexdigest() == marker["sha256"],
          f"marker SHA-256 mismatch for {marker['path']}")
    print(f"CHECK marker {marker['path']}: PASS (UTF-8 bytes and SHA-256)")

    truncate = manifest["truncate_results"]
    shrink = truncate["shrink"]
    shrink_path = join_target(root_path, shrink["path"])
    check(os.stat(shrink_path, follow_symlinks=False).st_size == shrink["prefix_size_bytes"],
          f"truncate shrink size mismatch for {shrink['path']}")
    check(file_sha256(shrink_path) == shrink["prefix_sha256"],
          f"truncate shrink prefix hash mismatch for {shrink['path']}")
    print(f"CHECK truncate shrink {shrink['path']}: PASS")

    grow = truncate["shrink_then_grow"]
    grow_path = join_target(root_path, grow["path"])
    grow_st = os.stat(grow_path, follow_symlinks=False)
    check(stat.S_ISREG(grow_st.st_mode),
          f"truncate shrink_then_grow target is not a regular file: {grow['path']}")
    check(grow_st.st_size == grow["final_size_bytes"],
          f"truncate shrink_then_grow size mismatch for {grow['path']}")
    check(file_sha256(grow_path) == grow["final_sha256"],
          f"truncate shrink_then_grow final hash mismatch for {grow['path']}")
    with open(grow_path, "rb") as stream:
        remaining = grow["preserved_prefix_bytes"]
        prefix_hash = hashlib.sha256()
        while remaining:
            chunk = stream.read(min(1024 * 1024, remaining))
            check(bool(chunk), f"short preserved prefix in {grow['path']}")
            prefix_hash.update(chunk)
            remaining -= len(chunk)
        check(prefix_hash.hexdigest() == grow["preserved_prefix_sha256"],
              f"truncate preserved prefix hash mismatch for {grow['path']}")
        stream.seek(grow["zero_range"]["offset_bytes"])
        zero_remaining = grow["zero_range"]["length_bytes"]
        while zero_remaining:
            chunk = stream.read(min(1024 * 1024, zero_remaining))
            check(bool(chunk), f"short zero-growth range in {grow['path']}")
            check(not any(chunk), f"truncate growth range contains non-zero bytes in {grow['path']}")
            zero_remaining -= len(chunk)
    print(f"CHECK truncate shrink_then_grow {grow['path']}: PASS (prefix and zero growth range)")

    for item in manifest["renames"]:
        source = join_target(root_path, item["from"])
        check(not os.path.lexists(source), f"rename source still exists: {item['from']}")
        target = item["to"]
        check(target in files, f"rename target has no files record: {target}")
        expected = files[target]
        target_path = join_target(root_path, target)
        check(os.stat(target_path, follow_symlinks=False).st_size == expected["size_bytes"],
              f"rename target size mismatch: {target}")
        check(file_sha256(target_path) == expected["sha256"],
              f"rename target SHA-256 mismatch: {target}")
        print(f"CHECK rename {item['from']} -> {target}: PASS (source absent, content matches)")

    for index, group in enumerate(manifest["hardlink_groups"]):
        inodes: set[tuple[int, int]] = set()
        for relative in group["paths"]:
            st = os.stat(join_target(root_path, relative), follow_symlinks=False)
            check(stat.S_ISREG(st.st_mode), f"hardlink path is not a regular file: {relative}")
            inodes.add((st.st_dev, st.st_ino))
            check(st.st_nlink == group["nlink"],
                  f"hardlink count mismatch for {relative}: {st.st_nlink} != {group['nlink']}")
        check(len(inodes) == 1,
              f"hardlink group {index} does not share st_dev/st_ino")
        print(f"CHECK hardlink group {index}: PASS ({len(inodes)} inode, nlink {group['nlink']})")

    for item in manifest["xattrs"]:
        path = join_target(root_path, item["path"])
        if item["state"] == "present":
            try:
                actual = os.getxattr(path, item["name"])
            except OSError as exc:
                raise ProbeError(f"cannot read xattr {item['name']} on {item['path']}: {exc}") from exc
            expected = bytes.fromhex(item["value_hex"])
            check(actual == expected, f"xattr bytes mismatch for {item['name']} on {item['path']}")
            check(len(actual) == item["length"],
                  f"xattr length mismatch for {item['name']} on {item['path']}")
            check(hashlib.sha256(actual).hexdigest() == item["sha256"],
                  f"xattr SHA-256 mismatch for {item['name']} on {item['path']}")
            print(f"CHECK xattr {item['name']} on {item['path']}: PASS (present)")
        else:
            check_absent_xattr(path, item["name"],
                               f"xattr {item['name']} on {item['path']}")
            print(f"CHECK xattr {item['name']} on {item['path']}: PASS (absent)")

    for relative, expected_children in directories.items():
        path = join_target(root_path, relative)
        try:
            names = os.listdir(path)
        except OSError as exc:
            raise ProbeError(f"cannot list directory {relative or '/'}: {exc}") from exc
        actual_children: dict[str, str] = {}
        for name in names:
            st = os.stat(os.path.join(path, name), follow_symlinks=False)
            if stat.S_ISDIR(st.st_mode):
                child_type = "directory"
            elif stat.S_ISREG(st.st_mode):
                child_type = "file"
            else:
                raise ProbeError(f"unsupported child type in directory {relative!r}: {name}")
            actual_children[name] = child_type
        check(actual_children == expected_children,
              f"direct children mismatch for directory {relative or '/'}: "
              f"actual={actual_children}, expected={expected_children}")
        print(f"CHECK directory {relative or '/'} children: PASS")

    for relative in manifest["absent_paths"]:
        check(not os.path.lexists(join_target(root_path, relative)),
              f"path declared absent exists: {relative}")
        print(f"CHECK absent path {relative}: PASS")

    check_tree(root_path, files, directories)

    # The Windows phase sets this attribute and deliberately leaves it in
    # place: the Linux kernel must read the same name and value back through
    # the mounted filesystem.  The probe does not write to the manifest tree;
    # the verifier's later mutation pass owns writes under its own temporary
    # directory.
    roundtrip = manifest["xattr_roundtrip"]
    roundtrip_path = join_target(root_path, roundtrip["path"])
    name = roundtrip["name"]
    expected_value = bytes.fromhex(roundtrip["value_hex"])
    try:
        actual_value = os.getxattr(roundtrip_path, name)
    except OSError as exc:
        raise ProbeError(
            f"xattr roundtrip attribute {name} is missing on "
            f"{roundtrip['path']}: {exc}") from exc
    check(actual_value == expected_value,
          f"xattr roundtrip byte mismatch for {name} on {roundtrip['path']}: "
          f"expected {expected_value.hex()}, got {actual_value.hex()}")
    print(f"CHECK xattr roundtrip read back {name} on {roundtrip['path']}: PASS")


def write_report(path: str | None, value: dict[str, Any]) -> None:
    if not path:
        return
    try:
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except OSError as exc:
        raise ProbeError(f"cannot write report {path}: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mountpoint", nargs="?", help="mounted XFS filesystem root")
    parser.add_argument("--manifest", required=True, help="Windows/Linux removable manifest v1")
    parser.add_argument("--validate-only", action="store_true",
                        help="validate the manifest without accessing a target")
    parser.add_argument("--preflight-device", metavar="BLOCK_PATH",
                        help="check explicit block-device identity and read-only safety")
    parser.add_argument("--report-json", help="write validation or device-check details as JSON")
    args = parser.parse_args()

    try:
        manifest, report = validate_manifest(args.manifest)
        if args.validate_only:
            check(args.mountpoint is None and args.preflight_device is None,
                  "--validate-only cannot be combined with a target")
            write_report(args.report_json, report)
            print("CHECK manifest schema v1 and cross-field consistency: PASS")
            return 0
        if args.preflight_device is not None:
            check(args.mountpoint is None, "--preflight-device cannot be combined with a mountpoint")
            device_report = check_device_preflight(args.preflight_device, manifest, report)
            report["device_preflight"] = device_report
            write_report(args.report_json, report)
            return 0
        check(args.mountpoint is not None, "provide a mountpoint, --validate-only, or --preflight-device")
        check(args.report_json is None, "--report-json is only valid with a preflight mode")
        check_semantics(args.mountpoint, manifest)
        print("CHECK all removable semantic assertions: PASS")
        return 0
    except ProbeError as exc:
        print(f"PROBE FAILED: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"PROBE FAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
