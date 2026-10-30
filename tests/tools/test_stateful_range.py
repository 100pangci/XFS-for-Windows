# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

import unittest

import stateful_range


class StatefulRangeTests(unittest.TestCase):
    def test_write_allows_old_new_and_zero_fill(self):
        before = b"abcdefgh"
        after = b"aXYZefgh0000"
        actual = b"aXcZefgh\x00\x00\x00\x00"
        contract = {"operation": "overwrite", "offset": 1, "length": 3,
                    "size": 0, "before_size": len(before),
                    "after_size": len(after)}
        self.assertIsNone(stateful_range.verify_range(
            actual, contract, before, after))

    def test_truncate_smaller_preserves_visible_prefix(self):
        before = b"abcdefgh"
        after = b"abcd"
        contract = {"operation": "truncate_smaller", "offset": 0,
                    "length": 0, "size": 4, "before_size": 8,
                    "after_size": 4}
        self.assertIsNone(stateful_range.verify_range(
            b"abcd", contract, before, after))
        self.assertIsNone(stateful_range.verify_range(
            b"abcd\x00\x00gh", contract, before, after))
        self.assertIn("byte 2", stateful_range.verify_range(
            b"abXd", contract, before, after))

    def test_truncate_larger_requires_zero_extension(self):
        before = b"abc"
        after = b"abc\x00\x00"
        contract = {"operation": "truncate_larger", "offset": 0,
                    "length": 0, "size": 5, "before_size": 3,
                    "after_size": 5}
        self.assertIsNone(stateful_range.verify_range(
            after, contract, before, after))
        self.assertIn("byte 3", stateful_range.verify_range(
            b"abcX\x00", contract, before, after))

    def test_reflink_checks_bytes_outside_cloned_range(self):
        before = b"abcdefgh"
        after = b"abXYefgh"
        contract = {"operation": "reflink", "offset": 1, "length": 2,
                    "size": 2, "before_size": 8, "after_size": 8}
        self.assertIsNone(stateful_range.verify_range(
            b"abXYefgh", contract, before, after))
        self.assertIn("byte 6", stateful_range.verify_range(
            b"abXYefXh", contract, before, after))

    def test_rejects_size_outside_old_new_set(self):
        before = b"abc"
        after = b"abcde"
        contract = {"operation": "extend", "offset": 3, "length": 2,
                    "size": 0, "before_size": 3, "after_size": 5}
        self.assertIn("neither old 3 nor new 5",
                      stateful_range.verify_range(
                          b"abc\x00", contract, before, after))


if __name__ == "__main__":
    unittest.main()
