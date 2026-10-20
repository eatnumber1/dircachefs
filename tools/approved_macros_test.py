# SPDX-License-Identifier: MIT
"""The approved macros list and .clang-tidy agree (plan step 25.58).

Arguments: the list (tools/approved_macros.txt) and .clang-tidy.
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

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
            "NAME|third_party|2026-10-19||libfuse")
        options = approved_macros.options(entries)
        self.assertIn("NAME", options[approved_macros.IGNORED_KEY])
        self.assertNotIn("NAME", options[approved_macros.ALLOWED_KEY])
        with self.assertRaises(ValueError):
            approved_macros.parse("NAME|third_party|2026-10-19|| ")
        kinds = {e[0]: e[1] for e in approved_macros.parse(read(PATHS["list"]))}
        for name in ("FUSE_USE_VERSION", "CHECK", "LOG", "RETURN_IF_ERROR"):
            self.assertEqual("third_party", kinds[name])

    def test_the_private_helper_of_assert_ok_and_assign_has_the_prefix(self):
        kinds = {e[0]: e[1] for e in approved_macros.parse(read(PATHS["list"]))}
        self.assertNotIn("ASSERT_OK_AND_ASSIGN_IMPL", kinds)
        self.assertEqual("prefixed", kinds["DCFS_ASSERT_OK_AND_ASSIGN_IMPL"])

    def test_macro_usage_allows_only_the_names_with_an_allowance(self):
        entries = approved_macros.parse(
            "KEPT|exempt|2026-10-19|returns from its caller|russ\n"
            "DCFS_GRANTED|prefixed|2026-10-19||russ\n")
        options = approved_macros.options(entries)
        self.assertEqual("'^(KEPT)$'", options[approved_macros.ALLOWED_KEY])
        self.assertIn("KEPT", options[approved_macros.IGNORED_KEY])

    def test_the_names_the_check_cannot_see_have_no_allowance(self):
        # macro-usage skips a body with # or ## and a non-literal constant, so
        # an allowance for these would be one the check never needs.
        by_name = {e[0]: e[3] for e in approved_macros.parse(
            read(PATHS["list"]))}
        for name in ("RET_CHECK", "DCFS_RET_CHECK_OP_",
                     "DCFS_RET_CHECK_ELSE_BLOCKER_",
                     "DCFS_CONCAT_INNER"):
            self.assertEqual("", by_name[name], name)
        for name in ("RET_CHECK_EQ", "ASSERT_OK_AND_ASSIGN"):
            self.assertNotEqual("", by_name[name], name)
        # What a function can express is not a macro: no list entry at all.
        for name in ("DCFS_INJECT", "EXPECT_HELPER"):
            self.assertNotIn(name, by_name)

    def test_bad_lists_are_refused(self):
        for line in ("NAME|prefixed|2026-10-19||words", "x y|exempt|2026-10-19||w",
                     "NAME|exempt|today||words", "NAME|exempt|2026-10-19||",
                     "NAME|exempt|2026-10-19|words", "NAME|exempt"):
            with self.assertRaises(ValueError, msg=line):
                approved_macros.parse(line)
        with self.assertRaises(ValueError):
            approved_macros.parse("A|exempt|2026-10-19||w\nA|exempt|2026-10-19||w")


class MainTest(unittest.TestCase):

    def test_write_regenerates_the_config_and_no_flag_is_usage(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "tools"))
            with open(os.path.join(root, "tools/approved_macros.txt"), "w") as f:
                f.write("NAME|exempt|2026-10-19||russ\n")
            config = os.path.join(root, ".clang-tidy")
            with open(config, "w") as f:
                f.write("Checks: '-*'\n  %s: 'drifted'\n  %s: 'drifted'\n"
                        % (approved_macros.ALLOWED_KEY,
                           approved_macros.IGNORED_KEY))
            entries = approved_macros.parse("NAME|exempt|2026-10-19||russ\n")
            self.assertTrue(approved_macros.problems(entries, read(config)))
            with mock.patch.dict(os.environ,
                                 {"BUILD_WORKSPACE_DIRECTORY": root}):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(2, approved_macros.main(["x"]))
                self.assertEqual(0, approved_macros.main(["x", "--write"]))
            self.assertEqual([], approved_macros.problems(entries,
                                                          read(config)))


LIST = approved_macros.parse(
    "KNOWN|exempt|2026-10-19||russ\nSTALE|exempt|2026-10-19||russ\n")


def scan(**files):
    return approved_macros.scan_problems(files, LIST)


class ScanTest(unittest.TestCase):
    """The scan of the sources: clang-tidy's macro-usage is silent on many
    macros, so every definition is compared with the list."""

    def test_a_macro_the_check_does_not_see_is_reported(self):
        for body in ("#define DCFS_PAREN (1 + 2)", "#define DCFS_EMPTY",
                     "#define DCFS_PASTE(x, y) x##y", "#define DCFS_STR(x) #x",
                     "#define DCFS_IDENT foo", "  #  define DCFS_TYPE long"):
            problems = scan(**{"a.cc": body + "\n#define KNOWN\n"})
            self.assertTrue(any("DCFS_" in p and "a.cc" in p
                                for p in problems), body)

    def test_a_stale_entry_is_reported(self):
        problems = scan(**{"a.cc": "#define KNOWN 1\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertIn("STALE", problems[0])

    def test_a_real_guard_is_not_reported(self):
        guard = ("// Comment.\n#ifndef DCFS_A_H_\n#define DCFS_A_H_\n"
                 "#define KNOWN 1\n#define STALE 1\n#endif\n")
        self.assertEqual([], scan(**{"a.h": guard}))

    def test_a_name_ending_in_h_that_is_not_a_guard_is_reported(self):
        not_first = ("#define KNOWN 1\n#define STALE 1\n#ifndef DCFS_B_H_\n"
                     "#define DCFS_B_H_\n#endif\n")
        mismatch = ("#ifndef DCFS_C_H_\n#define DCFS_D_H_\n#define KNOWN 1\n"
                    "#define STALE 1\n#endif\n")
        lone = "#define DCFS_E_H_\n#define KNOWN 1\n#define STALE 1\n"
        for text in (not_first, mismatch, lone):
            problems = scan(**{"a.h": text})
            self.assertTrue(any("_H_" in p for p in problems), text)

    def test_a_guard_with_a_body_or_no_closing_endif_is_reported(self):
        body = ("#ifndef DCFS_X\n#define DCFS_X 4096\n#define KNOWN 1\n"
                "#define STALE 1\n#endif\n")
        open_ended = ("#ifndef DCFS_Y_H_\n#define DCFS_Y_H_\n#define KNOWN 1\n"
                      "#define STALE 1\n#include <x>\n")
        for text, name in ((body, "DCFS_X"), (open_ended, "DCFS_Y_H_")):
            problems = scan(**{"a.h": text})
            self.assertTrue(any(name in p for p in problems), text)

    def test_the_tree_defines_what_the_list_names(self):
        files = {path: read(path) for path in PATHS["sources"]}
        problems = approved_macros.scan_problems(
            files, approved_macros.parse(read(PATHS["list"])))
        if problems:
            self.fail("\n" + "\n".join(problems))


def main():
    rest = [sys.argv[0]]
    for arg in sys.argv[1:]:
        if arg.endswith(".txt"):
            PATHS["list"] = arg
        elif arg.endswith(".clang-tidy"):
            PATHS["config"] = arg
        elif arg.endswith((".cc", ".h")):
            PATHS.setdefault("sources", []).append(arg)
        else:
            rest.append(arg)
    unittest.main(argv=rest)


if __name__ == "__main__":
    main()
