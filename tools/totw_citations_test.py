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


def tree(files):
    """A git repository holding `files` (path -> text); returns its root."""
    root = tempfile.mkdtemp()
    for path, text in files.items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    return root


class CitationsTest(unittest.TestCase):

    def test_a_cited_tip_that_does_not_exist_is_a_finding(self):
        root = tree({"docs/style.md": "See TotW #9999 for why.\n"})
        self.assertEqual(
            ["docs/style.md:1: TotW 9999 is not a published tip"],
            totw_citations.findings(root, PUBLISHED))

    def test_published_tips_pass_in_every_form(self):
        root = tree({
            "docs/style.md": (
                "TotW #103, TotW 42, TOTW 122, tips/103, totw/42,\n"
                "https://abseil.io/tips/122 and Tip of the Week #42.\n"),
            "docs/design.md": "TotW #42\n",
            "docs/abseil-utilities.md": "TotW #103\n",
        })
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_cpp_comments_are_checked_and_the_line_is_reported(self):
        root = tree({"dcfs/a.cc": "int x;\n// per TotW #7 do this\n"})
        self.assertEqual(
            ["dcfs/a.cc:2: TotW 7 is not a published tip"],
            totw_citations.findings(root, PUBLISHED))

    def test_other_files_and_the_plan_are_not_checked(self):
        root = tree({
            "docs/plan/log.md": "TotW #9999\n",
            "README.md": "TotW #9999\n",
        })
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_a_number_that_is_not_a_citation_passes(self):
        root = tree({"docs/style.md": "third_party/abseil_tips/ 25.32 "
                                      "and tips for 2026\n"})
        self.assertEqual([], totw_citations.findings(root, PUBLISHED))

    def test_published_numbers_come_from_the_index(self):
        path = os.path.join(tempfile.mkdtemp(), "index.tsv")
        with open(path, "w") as f:
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
