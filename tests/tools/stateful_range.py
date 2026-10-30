# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Shared semantic checks for stateful data-operation crash contracts."""

import os


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % value


def load_contract(manifest, manifest_path):
    contract = manifest.get("range_contract")
    if manifest.get("state_mode") != "range":
        if contract is not None:
            raise ValueError("exact manifest unexpectedly has a range contract")
        return None, None, None
    if not isinstance(contract, dict):
        raise ValueError("range manifest has no byte contract")
    data_path = os.path.join(os.path.dirname(os.path.abspath(manifest_path)),
                             contract["data_file"])
    with open(data_path, "rb") as f:
        data = f.read()
    before_size = int(contract["before_size"])
    after_size = int(contract["after_size"])
    if len(data) != before_size + after_size:
        raise ValueError("range contract byte file has the wrong length")
    before = data[:before_size]
    after = data[before_size:]
    if fnv1a(before) != contract["before_hash"]:
        raise ValueError("range contract before bytes do not match their hash")
    if fnv1a(after) != contract["after_hash"]:
        raise ValueError("range contract after bytes do not match their hash")
    return contract, before, after


def verify_range(actual, contract, before, after):
    """Return None when actual bytes obey the documented operation contract."""
    operation = contract["operation"]
    before_size = len(before)
    after_size = len(after)
    actual_size = len(actual)
    if actual_size not in {before_size, after_size}:
        return ("size %d is neither old %d nor new %d" %
                (actual_size, before_size, after_size))

    offset = int(contract["offset"])
    length = int(contract["length"])
    destination = int(contract["size"])
    for pos, got in enumerate(actual):
        has_old = pos < before_size
        has_new = pos < after_size
        old_value = before[pos] if has_old else 0
        new_value = after[pos] if has_new else 0

        if operation == "truncate_smaller":
            if pos < after_size:
                allowed = has_old and got == old_value
            else:
                allowed = got == 0 or (has_old and got == old_value)
        elif operation in ("truncate_larger", "preallocate"):
            if pos < before_size:
                allowed = got == old_value
            else:
                allowed = got == 0
        elif operation == "reflink":
            start = destination
            end = start + length
            if start <= pos < end:
                allowed = ((has_old and got == old_value) or
                           (has_new and got == new_value))
            elif has_old:
                allowed = got == old_value
            else:
                allowed = got == 0
        elif operation in ("write", "extend", "overwrite", "sparse_write",
                           "cow_overwrite"):
            if pos < before_size:
                allowed = ((has_old and got == old_value) or
                           (has_new and got == new_value))
            else:
                allowed = got == 0 or (has_new and got == new_value)
        else:
            return "unsupported range operation %s" % operation

        if not allowed:
            return "byte %d is outside the %s old/new/zero contract" % (
                pos, operation)
    return None
