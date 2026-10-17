# SPDX-License-Identifier: MIT
"""Checks that the index of the pinned Tips of the Week holds known tips.

A bad pin or an upstream restructuring fails here, not in a reviewer's
hands. argv: index.tsv and tips.md.
"""
import re
import sys
import unittest

# Tips docs/style.md cites or the reviewer prompts rely on: number, a word of
# the title.
EXPECTED = {42: "Factory Functions", 103: "Flags Are Globals",
            122: "Test Fixtures"}


class IndexTest(unittest.TestCase):

    def setUp(self):
        with open(sys.argv[1]) as f:
            rows = [line.rstrip("\n").split("\t") for line in f]
        self.titles = {int(n): title for n, title, _ in rows}
        with open(sys.argv[2]) as f:
            self.headings = set(re.findall(r"^# TotW #(\d+): ", f.read(), re.M))

    def test_known_tips_are_indexed(self):
        for number, word in EXPECTED.items():
            self.assertIn(word, self.titles[number])

    def test_every_indexed_tip_has_a_heading(self):
        self.assertEqual({str(n) for n in self.titles}, self.headings)

    def test_the_index_is_not_truncated(self):
        self.assertGreater(len(self.titles), 80)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
