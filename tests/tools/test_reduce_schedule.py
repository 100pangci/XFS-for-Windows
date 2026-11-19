#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
import importlib.util
import pathlib
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "reduce_stateful_trace.py"
spec = importlib.util.spec_from_file_location("reduce_stateful_trace", SCRIPT)
reducer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reducer)


class ScheduleWriterTests(unittest.TestCase):
    def test_schedule_is_written_with_a_real_newline(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "reduced.schedule"
            reducer.write_schedule(path, [0, 2, 1])
            self.assertEqual(path.read_bytes(), b"0,2,1\n")


if __name__ == "__main__":
    unittest.main()
