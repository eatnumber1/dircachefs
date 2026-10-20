# SPDX-License-Identifier: MIT
"""The approved macros list and .clang-tidy agree (plan step 25.58).

Arguments: the list (tools/approved_macros.txt) and .clang-tidy.
"""

import sys
import unittest

import approved_macros

PATHS = {}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class ApprovedMacrosTest(unittest.TestCase):

    def test_config_is_generated_from_the_list(self):
        entries = approved_macros.parse(read(PATHS["list"]))
        problems = approved_macros.problems(entries, read(PATHS["config"]))
        if problems:
            self.fail("\n" + "\n".join(problems))

    def test_a_config_that_disagrees_is_reported(self):
        entries = approved_macros.parse(read(PATHS["list"]))
        config = read(PATHS["config"]).replace("RET_CHECK_OK|", "")
        config = config.replace("RETURN_IF_ERROR|", "")
        self.assertTrue(approved_macros.problems(entries, config))
        self.assertFalse(approved_macros.problems(
            entries, approved_macros.write(entries, config)))

    def test_no_guard_and_no_regex_is_listed(self):
        entries = approved_macros.parse(read(PATHS["list"]))
        self.assertEqual([], [e[0] for e in entries if not e[0].isidentifier()])
        self.assertEqual([], [e[0] for e in entries if e[0].endswith("_H_")])

    def test_header_guards_are_exempt_without_a_list_entry(self):
        options = approved_macros.options([])
        self.assertIn(approved_macros.GUARDS, options[approved_macros.IGNORED_KEY])

    def test_third_party_names_need_no_prefix_and_name_their_library(self):
        entries = approved_macros.parse(
            "NAME|third_party|2026-10-19|libfuse")
        options = approved_macros.options(entries)
        self.assertIn("NAME", options[approved_macros.IGNORED_KEY])
        self.assertIn("NAME", options[approved_macros.ALLOWED_KEY])
        with self.assertRaises(ValueError):
            approved_macros.parse("NAME|third_party|2026-10-19| ")
        kinds = {e[0]: e[1] for e in approved_macros.parse(read(PATHS["list"]))}
        for name in ("FUSE_USE_VERSION", "CHECK", "LOG"):
            self.assertEqual("third_party", kinds[name])

    def test_the_private_helper_of_assert_ok_and_assign_has_the_prefix(self):
        kinds = {e[0]: e[1] for e in approved_macros.parse(read(PATHS["list"]))}
        self.assertNotIn("ASSERT_OK_AND_ASSIGN_IMPL", kinds)
        self.assertEqual("prefixed", kinds["DCFS_ASSERT_OK_AND_ASSIGN_IMPL"])

    def test_bad_lists_are_refused(self):
        for line in ("NAME|prefixed|2026-10-19|words", "x y|exempt|2026-10-19|w",
                     "NAME|exempt|today|words", "NAME|exempt|2026-10-19|",
                     "NAME|exempt"):
            with self.assertRaises(ValueError, msg=line):
                approved_macros.parse(line)
        with self.assertRaises(ValueError):
            approved_macros.parse("A|exempt|2026-10-19|w\nA|exempt|2026-10-19|w")


def main():
    rest = [sys.argv[0]]
    for arg in sys.argv[1:]:
        if arg.endswith(".txt"):
            PATHS["list"] = arg
        elif arg.endswith(".clang-tidy"):
            PATHS["config"] = arg
        else:
            rest.append(arg)
    unittest.main(argv=rest)


if __name__ == "__main__":
    main()
