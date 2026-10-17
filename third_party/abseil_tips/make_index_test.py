# SPDX-License-Identifier: MIT
"""Tests of make_index: which posts are tips, and what the index says
(plan step 25.32).
"""
import os
import tempfile
import unittest

import make_index


def post(directory, name, title, published="true"):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write('---\ntitle: "%s"\nlayout: tips\npublished: %s\n---\n\n'
                "Body of %s.\n" % (title, published, name))
    return path


class MakeIndexTest(unittest.TestCase):

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.dir = self.temp_dir.name

    def test_a_tip_has_its_number_title_and_body(self):
        path = post(self.dir, "2017-10-20-totw-122.md",
                    "Tip of the Week #122: Test Fixtures")
        tips = make_index.read_tips([path])
        self.assertEqual({122: ("Test Fixtures", path)},
                         {n: (t, p) for n, (t, p, _) in tips.items()})
        self.assertEqual("Body of 2017-10-20-totw-122.md.\n",
                         tips[122][2])

    def test_html_in_a_title_is_dropped(self):
        path = post(self.dir, "2017-09-26-totw-1.md",
                    "Tip of the Week #1: <code>string_view</code>")
        self.assertEqual("string_view", make_index.read_tips([path])[1][0])

    def test_html_entities_in_a_title_are_unescaped(self):
        path = post(self.dir, "2018-05-18-totw-148.md",
                    "Tip of the Week #148: Lines &amp; Blocks")
        self.assertEqual("Lines & Blocks",
                         make_index.read_tips([path])[148][0])

    def test_an_unpublished_tip_is_left_out(self):
        path = post(self.dir, "2019-11-25-totw-132.md",
                    "Tip of the Week #132: Maps", published="false")
        self.assertEqual({}, make_index.read_tips([path]))

    def test_a_republished_tip_takes_the_latest_file(self):
        old = post(self.dir, "2018-01-25-totw-3.md", "Tip of the Week #3: Old")
        new = post(self.dir, "2022-11-16-totw-3.md", "Tip of the Week #3: New")
        for order in ([old, new], [new, old]):
            self.assertEqual(new, make_index.read_tips(order)[3][1])

    def test_outputs_are_in_number_order(self):
        a = post(self.dir, "2017-10-20-totw-122.md", "Tip of the Week #122: B")
        b = post(self.dir, "2017-09-26-totw-1.md", "Tip of the Week #1: A")
        index, text = make_index.render(make_index.read_tips([a, b]))
        self.assertEqual(
            "1\tA\t_posts/2017-09-26-totw-1.md\n"
            "122\tB\t_posts/2017-10-20-totw-122.md\n", index)
        self.assertEqual(
            "# TotW #1: A\n\nBody of 2017-09-26-totw-1.md.\n\n"
            "# TotW #122: B\n\nBody of 2017-10-20-totw-122.md.\n", text)


if __name__ == "__main__":
    unittest.main()
