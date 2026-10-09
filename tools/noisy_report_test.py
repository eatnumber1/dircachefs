"""Tests of noisy_report.py (step 26.14e): over canned build events, a run with
a failed test, a flaky one (two of three runs failed) and a timeout lists each,
and a clean run says so; a damaged event file is an error, not an empty table;
the logs of the tests that were not clean (and only those) are copied."""

import json
import os
import subprocess
import sys
import tempfile
import unittest

import noisy_report


def event(label, status, runs=1, failed=0):
    return json.dumps(
        {
            "id": {"testSummary": {"label": label}},
            "testSummary": {
                "overallStatus": status,
                "totalRunCount": runs,
                "passed": [{}] * (runs - failed),
                "failed": [{}] * failed,
            },
        }
    )


def write(path, lines):
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


class NoisyReportTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.bep = os.path.join(self.dir, "bep.json")

    def test_a_clean_run_has_no_findings(self):
        write(self.bep, [event("//a:t", "PASSED", 3), json.dumps({"id": {"started": {}}}), event("//a:u", "PASSED")])
        text = noisy_report.report("plain", noisy_report.load(self.bep))
        self.assertIn("2 tests: 2 passed.", text)
        self.assertIn("No failures and no flaky tests.", text)
        self.assertNotIn("|", text)

    def test_failures_flakes_and_timeouts_are_listed(self):
        write(
            self.bep,
            [
                event("//a:ok", "PASSED", 3),
                event("//a:broken", "FAILED", 3, 3),
                event("//a:racy", "FLAKY", 3, 2),
                event("//a:slow", "TIMEOUT", 3, 1),
            ],
        )
        text = noisy_report.report("asan", noisy_report.load(self.bep))
        self.assertIn("4 tests: 1 failed, 1 flaky, 1 passed, 1 timeout.", text)
        self.assertIn("| `//a:broken` | failed | 3 of 3 |", text)
        self.assertIn("| `//a:racy` | flaky | 2 of 3 |", text)
        self.assertIn("| `//a:slow` | timeout | 1 of 3 |", text)
        self.assertNotIn("//a:ok", text)
        self.assertIn("noisy-logs-asan", text)

    def test_a_damaged_file_is_an_error(self):
        write(self.bep, [event("//a:t", "PASSED"), "{not json"])
        with self.assertRaises(ValueError):
            noisy_report.load(self.bep)
        self.assertEqual(noisy_report.main(["--name", "x", "--bep", self.bep]), 2)
        self.assertEqual(noisy_report.main(["--name", "x", "--bep", os.path.join(self.dir, "missing")]), 2)

    def test_only_the_logs_of_findings_are_copied(self):
        write(self.bep, [event("//p/q:good", "PASSED"), event("//p/q:racy", "FLAKY", 3, 1)])
        logs = os.path.join(self.dir, "testlogs")
        for target in ("good", "racy"):
            os.makedirs(os.path.join(logs, "p/q", target, "run_1_of_3"))
            with open(os.path.join(logs, "p/q", target, "run_1_of_3", "test.log"), "w") as f:
                f.write(target)
        out = os.path.join(self.dir, "out")
        rc = noisy_report.main(["--name", "x", "--bep", self.bep, "--testlogs", logs, "--logs-out", out])
        self.assertEqual(rc, 0)
        self.assertTrue(os.path.exists(os.path.join(out, "p/q/racy/run_1_of_3/test.log")))
        self.assertFalse(os.path.exists(os.path.join(out, "p/q/good")))

    def test_the_exit_status_is_zero_with_findings(self):
        write(self.bep, [event("//a:broken", "FAILED", 3, 3)])
        self.assertEqual(noisy_report.main(["--name", "x", "--bep", self.bep]), 0)


if __name__ == "__main__":
    unittest.main()
