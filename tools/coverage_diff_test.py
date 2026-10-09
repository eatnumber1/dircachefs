"""Tests of coverage_diff.py, the coverage determinism check (step 26.14).

The check must pass two identical reports and fail, naming file and line, on
a line or branch covered in one run and not the other; counts that differ but
stay covered pass unless --exact; files outside the prefixes are ignored; and
per test (two copies of bazel-testlogs) it names the test that differs.
"""

import io
import os
import tempfile
import unittest
import unittest.mock

import coverage_diff

A = """TN:
SF:dcfs/a.cc
DA:1,5
DA:2,0
DA:3,1
BRDA:3,0,0,1
BRDA:3,0,1,-
LF:3
LH:2
end_of_record
SF:test/other.cc
DA:1,1
end_of_record
"""


def run(args):
    out = io.StringIO()
    return coverage_diff.main(args, out), out.getvalue()


class CoverageDiffTest(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=os.environ.get("TEST_TMPDIR"))

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_identical_reports_pass(self):
        a = self.write("a.dat", A)
        b = self.write("b.dat", A)
        status, out = run([a, b])
        self.assertEqual(status, 0, out)
        self.assertIn("0 differences", out)

    def test_a_line_covered_in_one_run_only_fails_naming_it(self):
        a = self.write("a.dat", A)
        b = self.write("b.dat", A.replace("DA:2,0", "DA:2,4"))
        status, out = run([a, b])
        self.assertEqual(status, 1)
        self.assertIn("dcfs/a.cc: line 2: first 0, second 4", out)
        self.assertIn("1 differences", out)

    def test_a_branch_taken_in_one_run_only_fails_naming_it(self):
        a = self.write("a.dat", A)
        b = self.write("b.dat", A.replace("BRDA:3,0,1,-", "BRDA:3,0,1,2"))
        status, out = run([a, b])
        self.assertEqual(status, 1)
        self.assertIn("dcfs/a.cc: line 3 branch 0.1: first 0, second 2", out)

    def test_a_line_missing_from_one_run_fails(self):
        a = self.write("a.dat", A)
        b = self.write("b.dat", A.replace("DA:2,0\n", ""))
        status, out = run([a, b])
        self.assertEqual(status, 1)
        self.assertIn("line 2: first 0, second None", out)

    def test_counts_that_differ_but_stay_covered_pass_unless_exact(self):
        a = self.write("a.dat", A)
        b = self.write("b.dat", A.replace("DA:1,5", "DA:1,9"))
        self.assertEqual(run([a, b])[0], 0)
        status, out = run(["--exact", a, b])
        self.assertEqual(status, 1)
        self.assertIn("line 1: first 5, second 9", out)

    def test_files_outside_the_prefixes_are_ignored(self):
        a = self.write("a.dat", A)
        b = self.write("b.dat", A.replace("SF:test/other.cc\nDA:1,1",
                                          "SF:test/other.cc\nDA:1,0"))
        self.assertEqual(run([a, b])[0], 0)
        self.assertEqual(run(["--prefix", "test/", a, b])[0], 1)

    def test_a_file_listed_twice_has_its_counts_added(self):
        twice = A + A.replace("DA:2,0", "DA:2,3")
        parsed = coverage_diff.parse(twice)
        self.assertEqual(parsed["dcfs/a.cc"][("line", 2)], 3)
        self.assertEqual(parsed["dcfs/a.cc"][("line", 1)], 10)

    def test_per_test_names_the_test_that_differs(self):
        for run_name, text in (("one", A),
                               ("two", A.replace("DA:2,0", "DA:2,1"))):
            self.write("%s/test/qemu/same_test/coverage.dat" % run_name, A)
            self.write("%s/test/qemu/flaky_test/coverage.dat" % run_name, text)
        status, out = run(["--per-test", os.path.join(self.dir, "one"),
                           os.path.join(self.dir, "two")])
        self.assertEqual(status, 1)
        self.assertIn("test/qemu/flaky_test: dcfs/a.cc: line 2", out)
        self.assertNotIn("same_test:", out)
        self.assertIn("2 tests compared, 1 differ, 1 differences", out)

    def test_per_test_reports_a_test_present_in_one_run_only(self):
        self.write("one/x_test/coverage.dat", A)
        os.makedirs(os.path.join(self.dir, "two"))
        status, out = run(["--per-test", os.path.join(self.dir, "one"),
                           os.path.join(self.dir, "two")])
        self.assertEqual(status, 1)
        self.assertIn("x_test: only in the first run", out)

    def test_a_bad_command_line_is_a_usage_error(self):
        with unittest.mock.patch("sys.stderr", io.StringIO()):
            self.assertEqual(coverage_diff.main(["--nonsense"]), 2)


if __name__ == "__main__":
    unittest.main()
