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

    def test_no_guest_script_waits_on_a_timer(self):
        self.assertEqual([], repo_shape.guest_sleeps(ROOT["root"]))

    def test_local_buffers_sized_once_are_fixed_arrays(self):
        self.assertEqual([], repo_shape.fixed_arrays(ROOT["root"]))

    def test_status_macros_are_the_short_names(self):
        self.assertEqual([], repo_shape.absl_prefixed_status_macros(ROOT["root"]))

    def test_no_identifier_begins_with_the_project_name(self):
        self.assertEqual([], repo_shape.project_prefix(ROOT["root"]))

    def test_every_disabled_check_is_in_the_limitations(self):
        self.assertEqual([], repo_shape.disabled_checks_listed(ROOT["root"]))

    def test_no_production_comment_gives_tests_as_the_reason(self):
        self.assertEqual([], repo_shape.no_test_only_comments(ROOT["root"]))

    def test_no_production_class_befriends_a_testonly_class(self):
        self.assertEqual([], repo_shape.no_testonly_friends(ROOT["root"]))


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
