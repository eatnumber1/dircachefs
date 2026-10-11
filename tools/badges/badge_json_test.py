"""Tests for badge_json.py (plan step 26.28): each parser on the line its job
prints, the endpoint schema shields.io reads, and a malformed input that fails
with exit 2 and writes nothing."""

import json
import os
import subprocess
import sys
import tempfile
import unittest

import badge_json

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")


def testdata(name):
    return os.path.join(DATA, name)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class CoverageTest(unittest.TestCase):
    def test_real_gate_line_is_green_at_baseline(self):
        obj = badge_json.badge(
            "coverage", read(testdata("coverage_gate.txt")),
            baseline=read(testdata("coverage_baseline.txt")))
        self.assertEqual(obj, {
            "schemaVersion": 1,
            "label": "coverage",
            "message": "lines 96.04% · branches 80.85%",
            "color": "green",
        })

    def test_below_baseline_is_red(self):
        obj = badge_json.badge(
            "coverage", read(testdata("coverage_gate.txt")),
            baseline="lines 96.50\nbranches 80.85\n")
        self.assertEqual(obj["color"], "red")
        self.assertEqual(obj["message"], "lines 96.04% · branches 80.85%")

    def test_branches_below_baseline_is_red(self):
        obj = badge_json.badge(
            "coverage", read(testdata("coverage_gate.txt")),
            baseline="lines 96.04\nbranches 81.00\n")
        self.assertEqual(obj["color"], "red")

    def test_gate_line_missing_is_an_error(self):
        with self.assertRaises(badge_json.BadInput):
            badge_json.badge(
                "coverage", read(testdata("coverage_malformed.txt")),
                baseline=read(testdata("coverage_baseline.txt")))


class ReproducibleTest(unittest.TestCase):
    def test_real_verdict_is_green(self):
        obj = badge_json.badge(
            "reproducible", read(testdata("reproducible_identical.txt")))
        self.assertEqual(obj["message"], "bit-identical")
        self.assertEqual(obj["color"], "green")

    def test_difference_is_red(self):
        obj = badge_json.badge(
            "reproducible", read(testdata("reproducible_differs.txt")))
        self.assertEqual(obj["message"], "differs")
        self.assertEqual(obj["color"], "red")

    def test_no_verdict_is_an_error(self):
        with self.assertRaises(badge_json.BadInput):
            badge_json.badge("reproducible", "make: *** [all] Error 1\n")


class MutationTest(unittest.TestCase):
    def test_all_row_gives_killed_of_total(self):
        obj = badge_json.badge("mutation", read(testdata("mutation_report.md")))
        self.assertEqual(obj["message"], "killed 2/4 (50%)")
        self.assertEqual(obj["label"], "mutation")

    def test_nothing_ran_is_an_error(self):
        text = ("| operator | killed | survived | invalid | suppressed | error "
                "| flaky | total |\n"
                "| **all** | 0 | 0 | 0 | 0 | 0 | 0 | 0 |\n")
        with self.assertRaises(badge_json.BadInput):
            badge_json.badge("mutation", text)


class TlcTest(unittest.TestCase):
    def test_completed_run_is_green_with_distinct_states(self):
        obj = badge_json.badge("tlc", read(testdata("tlc_completed.txt")))
        self.assertEqual(obj["message"], "1479307 states · 0 violations")
        self.assertEqual(obj["color"], "green")

    def test_violation_is_red(self):
        obj = badge_json.badge("tlc", read(testdata("tlc_violation.txt")))
        self.assertEqual(obj["message"], "1479307 states · violation")
        self.assertEqual(obj["color"], "red")

    def test_no_state_count_is_an_error(self):
        with self.assertRaises(badge_json.BadInput):
            badge_json.badge("tlc", "Model checking completed. No error has been found.\n")


class OsvTest(unittest.TestCase):
    def test_only_ignored_findings_is_green(self):
        obj = badge_json.badge(
            "osv", [read(testdata("osv_clean.json"))],
            config=read(testdata("osv-scanner.toml")))
        self.assertEqual(obj["message"],
                         "0 vulnerabilities · 21 ignored until 2027-01-06")
        self.assertEqual(obj["color"], "green")

    def test_an_unignored_finding_is_red(self):
        obj = badge_json.badge(
            "osv", [read(testdata("osv_found.json"))],
            config=read(testdata("osv-scanner.toml")))
        self.assertEqual(obj["message"],
                         "1 vulnerabilities · 21 ignored until 2027-01-06")
        self.assertEqual(obj["color"], "red")

    def test_no_ignores_says_zero_ignored(self):
        obj = badge_json.badge(
            "osv", [read(testdata("osv_found.json"))], config="")
        self.assertEqual(obj["message"], "2 vulnerabilities · 0 ignored")
        self.assertEqual(obj["color"], "red")

    def test_malformed_json_is_an_error(self):
        with self.assertRaises(badge_json.BadInput):
            badge_json.badge(
                "osv", [read(testdata("osv_malformed.json"))],
                config=read(testdata("osv-scanner.toml")))


class EndpointTest(unittest.TestCase):
    def test_writes_the_endpoint_schema_and_nothing_on_bad_input(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "reproducible.json")
            code = subprocess.call([
                sys.executable, "-I", badge_json.__file__, "reproducible",
                "--input", testdata("reproducible_identical.txt"),
                "--out", out])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(read(out)), {
                "schemaVersion": 1,
                "label": "reproducible",
                "message": "bit-identical",
                "color": "green",
            })
            bad = os.path.join(d, "bad.json")
            code = subprocess.call([
                sys.executable, "-I", badge_json.__file__, "reproducible",
                "--input", testdata("coverage_malformed.txt"), "--out", bad],
                stderr=subprocess.DEVNULL)
            self.assertEqual(code, 2)
            self.assertFalse(os.path.exists(bad))


if __name__ == "__main__":
    unittest.main()
