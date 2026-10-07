"""The //dcfs:syscalls_backing dependents golden (plan step 26.7).

The real graph against the golden must match; a graph with one more
dependent (a deliberate dependency added to a target outside the allowed
set) and a golden with one line removed must each fail, with the diff and
the sentence.
"""

import sys
import unittest

import syscalls_backing_users

ARGS = {}


def read(key):
    with open(ARGS[key], encoding="utf-8") as f:
        return f.read()


class SyscallsBackingUsersTest(unittest.TestCase):

    def test_dependents_match_the_golden(self):
        self.assertEqual(
            "", syscalls_backing_users.compare(read("graph"), read("golden")))

    def test_dir_cache_fs_is_not_allowed(self):
        # The request layer never names backing syscalls.
        self.assertNotIn("//dcfs:dir_cache_fs",
                         syscalls_backing_users.labels(read("golden")))
        self.assertNotIn("//dcfs:metadata_cache",
                         syscalls_backing_users.labels(read("golden")))

    def test_an_extra_dependent_fails_with_diff_and_sentence(self):
        problem = syscalls_backing_users.compare(
            read("graph") + "//dcfs:dir_cache_fs\n", read("golden"))
        self.assertIn("+//dcfs:dir_cache_fs", problem)
        self.assertIn("deliberate edit of dcfs/syscalls_backing_users.txt",
                      problem)

    def test_a_golden_missing_a_line_fails(self):
        names = syscalls_backing_users.labels(read("golden"))
        self.assertTrue(names)
        problem = syscalls_backing_users.compare(
            read("graph"), "\n".join(names[1:]) + "\n")
        self.assertIn("+" + names[0], problem)


def main():
    rest = []
    for arg in sys.argv[1:]:
        key, sep, value = arg.partition("=")
        if sep and key in ("graph", "golden"):
            ARGS[key] = value
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
