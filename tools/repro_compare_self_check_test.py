"""Self-check of the reproducible-build gate (step 26.12).

repro_compare.py must accept identical builds and reject two builds of a
program that embeds the build date and time (toolchains_llvm redacts
__DATE__ and __TIME__, so the fixture gets them from a genrule running
`date`), and its report must name the string that differs.
"""

import os
import re
import sys
import tempfile
import unittest

import repro_compare

FIXTURE = ""


def read(path):
    with open(path, "rb") as f:
        return f.read()


class ReproCompareSelfCheck(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=os.environ.get("TEST_TMPDIR"))
        self.binary = read(FIXTURE)
        self.stamp = re.search(rb"built [A-Z][a-z]{2} [ 0-9]{2} [0-9]{4} [0-9:]{8}",
                               self.binary)

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_the_fixture_really_embeds_the_compile_date(self):
        self.assertIsNotNone(self.stamp, "the fixture has no 'built <date> <time>' string")

    def test_identical_files_pass(self):
        a = self.write("a", self.binary)
        b = self.write("b", self.binary)
        self.assertTrue(repro_compare.compare(a, b))

    def test_two_builds_at_different_times_fail_and_name_the_string(self):
        later = self.binary.replace(self.stamp.group(0), b"built Jan  1 1999 00:00:00")
        a = self.write("a", self.binary)
        b = self.write("b", later)
        with open(os.path.join(self.dir, "report"), "w") as report:
            self.assertFalse(repro_compare.compare(a, b, out=report))
        text = open(os.path.join(self.dir, "report")).read()
        self.assertIn("NOT REPRODUCIBLE", text)
        self.assertIn(self.stamp.group(0).decode(), text)
        self.assertIn("built Jan  1 1999 00:00:00", text)

    def test_files_of_different_length_fail(self):
        a = self.write("a", b"abc")
        b = self.write("b", b"abcd")
        with open(os.path.join(self.dir, "report"), "w") as report:
            self.assertFalse(repro_compare.compare(a, b, out=report))


if __name__ == "__main__":
    FIXTURE = sys.argv.pop(1)
    unittest.main()
