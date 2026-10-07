"""Repository-shape rules over the real source tree (plan step 26.13).

The tree is found through the real path of MODULE.bazel (a symlink into the
workspace). Bazel cannot declare "every directory of third_party" as inputs,
so the target is tagged `external` (never served from the test cache).
"""

import os
import sys
import unittest

import repo_shape

ROOT = {}


class RepoShapeTest(unittest.TestCase):

    def test_every_third_party_directory_has_a_readme(self):
        self.assertEqual([], repo_shape.third_party_readmes(ROOT["root"]))

    def test_every_guest_script_is_used_by_a_test(self):
        self.assertEqual([], repo_shape.guest_scripts_used(ROOT["root"]))

    def test_every_disabled_check_is_in_the_limitations(self):
        self.assertEqual([], repo_shape.disabled_checks_listed(ROOT["root"]))


def main():
    rest = []
    for arg in sys.argv[1:]:
        if arg.startswith("anchor="):
            ROOT["root"] = os.path.dirname(
                os.path.realpath(arg[len("anchor="):]))
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
