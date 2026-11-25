# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import stateful_contract
import stateful_range


class StatefulCrashContractTests(unittest.TestCase):
    def setUp(self):
        self.manifest = {
            "state_expectation": "either",
            "labels": ["before", "after"],
            "states": [{"value": "old"}, {"value": "new"}],
        }

    def matches_observed(self, manifest, observed):
        return any(manifest["states"][index] == observed
                   for index in stateful_contract.state_candidates(manifest))

    def test_before_contract_rejects_new_state(self):
        self.manifest["state_expectation"] = "before"
        self.assertTrue(self.matches_observed(self.manifest, {"value": "old"}))
        self.assertFalse(self.matches_observed(self.manifest, {"value": "new"}))

    def test_after_contract_rejects_old_state(self):
        self.manifest["state_expectation"] = "after"
        self.assertTrue(self.matches_observed(self.manifest, {"value": "new"}))
        self.assertFalse(self.matches_observed(self.manifest, {"value": "old"}))

    def test_either_contract_accepts_either_state(self):
        self.assertTrue(self.matches_observed(self.manifest, {"value": "old"}))
        self.assertTrue(self.matches_observed(self.manifest, {"value": "new"}))

    def test_required_state_label_must_exist(self):
        self.manifest["state_expectation"] = "after"
        self.manifest["labels"] = ["before", "before"]
        with self.assertRaisesRegex(ValueError, "no such state"):
            stateful_contract.state_candidates(self.manifest)

    def test_required_state_label_must_be_unique(self):
        self.manifest["state_expectation"] = "before"
        self.manifest["labels"] = ["before", "before"]
        with self.assertRaisesRegex(ValueError, "multiple before states"):
            stateful_contract.state_candidates(self.manifest)

    def test_range_keeps_payload_allowance_with_before_metadata(self):
        self.manifest["state_expectation"] = "before"
        self.assertEqual(stateful_contract.state_candidates(self.manifest), [0])
        before = b"abcdefgh"
        after = b"aXYZefgh"
        contract = {"operation": "overwrite", "offset": 1, "length": 3,
                    "size": 0, "before_size": len(before),
                    "after_size": len(after)}
        # Physical file data can reach disk before the transaction's metadata.
        self.assertIsNone(stateful_range.verify_range(
            b"aXcZefgh", contract, before, after))

    def test_range_contract_pins_size_but_allows_mixed_payload(self):
        contract = {"operation": "extend", "offset": 3, "length": 1,
                    "size": 0, "before_size": 3, "after_size": 4}
        self.assertIsNone(stateful_contract.range_size_error(3, 3, contract))
        self.assertIsNone(stateful_contract.range_size_error(4, 4, contract))
        self.assertIn("size 4 != expected state size 3",
                      stateful_contract.range_size_error(4, 3, contract))
        before = b"abc"
        after = b"XYZQ"
        self.assertIsNone(stateful_range.verify_range(
            b"aYZ", contract, before, after))

    def test_unknown_expectation_is_rejected(self):
        self.manifest["state_expectation"] = "partial"
        with self.assertRaisesRegex(ValueError, "unknown state expectation"):
            stateful_contract.state_candidates(self.manifest)


if __name__ == "__main__":
    unittest.main()
