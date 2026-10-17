# SPDX-License-Identifier: MIT
"""Tests of the Tip of the Week citation check (plan step 25.32).

Unit tests run the check over fixture trees; the real-tree test runs it over
the repository's own documents and C++ against the pinned archive's index.
argv: `anchor=<MODULE.bazel>` and `index=<index.tsv>`, then unittest's.
"""

import os
import subprocess
import sys
import tempfile
import unittest

import totw_citations

CONFIG = {}
PUBLISHED = {42, 103, 122}


class CitationsTest(unittest.TestCase):

    def make_tree(self, files):
        """A git repository holding `files` (path -> text); returns its root."""
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = temp_dir.name
        for path, text in files.items():
            full = os.path.join(root, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(text)
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "add", "-A"], cwd=root, check=True)
        return root

    def test_a_cited_tip_that_does_not_exist_is_a_finding(self):
        root = self.make_tree({"docs/style.md": "See TotW #9999 for why.\n"})
        self.assertEqual(
            ["docs/style.md:1: TotW 9999 is not a published tip"],
            totw_citations.findings(root, PUBLISHED))

    def test_published_tips_pass_in_every_form(self):
        root = self.make_tree({
            "docs/style.md": (
                "TotW #103, TotW 42, TOTW 122, totw/42,\n"
                "https://abseil.io/tips/122 and Tip of the Week #42.\n"),
            "docs/design.md": "TotW #42\n",
            "docs/abseil-utilities.md": "TotW #103\n",
        })
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_cpp_comments_are_checked_and_the_line_is_reported(self):
        root = self.make_tree({"dcfs/a.cc": "int x;\n// per TotW #7 do this\n"})
        self.assertEqual(
            ["dcfs/a.cc:2: TotW 7 is not a published tip"],
            totw_citations.findings(root, PUBLISHED))

    def test_other_files_and_the_plan_are_not_checked(self):
        root = self.make_tree({
            "docs/plan/log.md": "TotW #9999\n",
            "README.md": "TotW #9999\n",
        })
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_a_number_that_is_not_a_citation_passes(self):
        root = self.make_tree({
            "docs/style.md": "third_party/abseil_tips/ 25.32, "
                             "bare tips/9999 and tips for 2026\n",
        })
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_a_citation_wrapped_across_a_line_break_is_checked(self):
        root = self.make_tree({"docs/style.md": "See TotW\n#9999 for why.\n"})
        self.assertEqual(
            ["docs/style.md:1: TotW 9999 is not a published tip"],
            totw_citations.findings(root, PUBLISHED))

    def test_version_or_plan_step_number_is_not_a_citation(self):
        root = self.make_tree({
            "docs/style.md": "TotW 25.32 is not a citation\n",
        })
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_two_citations_on_one_line_are_both_checked(self):
        root = self.make_tree({
            "docs/style.md": "TotW #9998 and TotW #9999\n",
            "docs/design.md": "TotW #42 and TotW #103\n",
        })
        self.assertEqual(
            ["docs/style.md:1: TotW 9998 is not a published tip",
             "docs/style.md:1: TotW 9999 is not a published tip"],
            totw_citations.findings(root, PUBLISHED))

    def test_published_numbers_come_from_the_index(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        path = os.path.join(temp_dir.name, "index.tsv")
        with open(path, "w", encoding="utf-8") as f:
            f.write("1\tstring_view\t_posts/a.md\n42\tFactories\t_posts/b.md\n")
        self.assertEqual({1, 42}, totw_citations.published(path))


class RepositoryTest(unittest.TestCase):

    def test_every_cited_tip_is_published(self):
        self.assertEqual([], totw_citations.findings(
            CONFIG["root"], totw_citations.published(CONFIG["index"])))


def main():
    rest = []
    for arg in sys.argv[1:]:
        if arg.startswith("anchor="):
            CONFIG["root"] = os.path.dirname(
                os.path.realpath(arg[len("anchor="):]))
        elif arg.startswith("index="):
            CONFIG["index"] = arg[len("index="):]
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
