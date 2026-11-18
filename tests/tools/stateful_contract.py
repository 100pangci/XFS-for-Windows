# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""State selection rules shared by the independent stateful image oracle."""


_EXPECTATIONS = {"before", "after", "either"}


def state_candidates(manifest):
    """Return manifest state indices allowed by its explicit contract."""
    states = manifest.get("states")
    if not isinstance(states, list) or not states:
        raise ValueError("manifest has no states")

    expectation = manifest.get("state_expectation", "either")
    if not isinstance(expectation, str) or expectation not in _EXPECTATIONS:
        raise ValueError("unknown state expectation %r" % expectation)

    labels = manifest.get("labels")
    if labels is None:
        labels = []
    if not isinstance(labels, list):
        raise ValueError("manifest state labels are not a list")
    if labels and len(labels) != len(states):
        raise ValueError("manifest state labels do not match its states")

    if expectation == "either":
        return list(range(len(states)))
    if not labels:
        raise ValueError("manifest state expectation requires state labels")
    candidates = [index for index, label in enumerate(labels)
                  if label == expectation]
    if not candidates:
        raise ValueError("manifest expects %s state but has no such state" %
                         expectation)
    if len(candidates) != 1:
        raise ValueError("manifest has multiple %s states" % expectation)
    return candidates


def range_size_error(actual_size, expected_size, contract):
    """Check that a data target has the size of its selected logical state."""
    try:
        allowed_sizes = {int(contract["before_size"]), int(contract["after_size"])}
        actual_size = int(actual_size)
        expected_size = int(expected_size)
    except (KeyError, TypeError, ValueError):
        return "invalid range size contract"
    if expected_size not in allowed_sizes:
        return "manifest state size %d is outside the range contract %s" % (
            expected_size, sorted(allowed_sizes))
    if actual_size != expected_size:
        return "size %d != expected state size %d" % (
            actual_size, expected_size)
    return None
