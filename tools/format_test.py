"""Every tracked C and C++ file is formatted (plan step 7.6a).

The check runs over the real source tree (found through MODULE.bazel's
symlink, as repo_shape_test does), so it sees the files `bazel run
//tools:format` sees. The test is tagged `external`: it is never served from
the test cache.
"""

import os
import sys
import unittest

import format

ROOT = {}


class FormatTest(unittest.TestCase):

    def test_every_tracked_c_and_cxx_file_is_formatted(self):
        files = format.tracked_files(ROOT["root"])
        self.assertTrue(files)
        rc, bad, diagnostics = format.check(
            ROOT["clang_format"], ROOT["style"], ROOT["root"], files)
        self.assertEqual(
            [], bad,
            f"{len(bad)} of {len(files)} files need formatting; "
            "run bazel run //tools:format")

    def test_the_check_fails_on_the_known_bad_fixture(self):
        rc, bad, diagnostics = format.check(
            ROOT["clang_format"], ROOT["style"], ROOT["root"],
            [format.FIXTURE])
        self.assertEqual(1, rc, diagnostics)
        self.assertEqual([format.FIXTURE], bad)


def main():
    rest = []
    for arg in sys.argv[1:]:
        if arg.startswith("anchor="):
            ROOT["root"] = os.path.dirname(
                os.path.realpath(arg[len("anchor="):]))
        elif arg.startswith("clang_format="):
            ROOT["clang_format"] = os.path.abspath(
                arg[len("clang_format="):])
        elif arg.startswith("style="):
            ROOT["style"] = os.path.abspath(arg[len("style="):])
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
