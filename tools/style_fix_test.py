"""Unit tests of style_fix.py: the changed-line ranges of a diff, the flags
file's parsing, and the choice of one flags file per source (25.23)."""

import os
import tempfile
import unittest

import style_fix


class ChangedLinesTest(unittest.TestCase):

    def test_a_single_changed_line_is_a_range_of_one(self):
        diff = "@@ -3 +3 @@ int x;\n-old\n+new\n"
        self.assertEqual(["--lines=3:3"], style_fix.changed_lines(diff))

    def test_a_hunk_with_a_count_covers_that_many_lines(self):
        diff = "@@ -10,2 +12,4 @@\n-a\n-b\n+c\n+d\n+e\n+f\n"
        self.assertEqual(["--lines=12:15"], style_fix.changed_lines(diff))

    def test_a_pure_deletion_has_no_new_lines_to_format(self):
        diff = "@@ -7,2 +6,0 @@\n-a\n-b\n"
        self.assertEqual([], style_fix.changed_lines(diff))

    def test_every_hunk_gives_its_own_range(self):
        diff = "@@ -1 +1 @@\n-a\n+b\n@@ -20,0 +21,2 @@\n+c\n+d\n"
        self.assertEqual(["--lines=1:1", "--lines=21:22"],
                         style_fix.changed_lines(diff))


class FlagsTest(unittest.TestCase):

    def test_one_flag_per_line_and_no_blank_lines(self):
        self.assertEqual(["-std=c++20", "-iquote", "."],
                         style_fix.read_flags("-std=c++20\n-iquote\n.\n\n"))


class FlagsFileTest(unittest.TestCase):

    def test_the_aspect_file_for_the_source_is_found(self):
        with tempfile.TemporaryDirectory() as root:
            out = os.path.join(root, "bazel-out", "k8-fastbuild", "bin", "dcfs",
                               "_style", "backing", "dcfs", "backing.cc.flags.txt")
            os.makedirs(os.path.dirname(out))
            open(out, "w").close()
            self.assertEqual(out, style_fix.flags_file(root, "dcfs/backing.cc"))

    def test_a_source_with_no_aspect_file_is_refused(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(SystemExit):
                style_fix.flags_file(root, "dcfs/missing.cc")


if __name__ == "__main__":
    unittest.main()
