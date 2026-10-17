# SPDX-License-Identifier: MIT
"""Checks that the rendered Google style guides hold known section headings.

A bad render or an upstream restructuring fails here, not in a reviewer's
hands. argv: the rendered cpp, python, shell and markdown guides, in order.
"""
import re
import sys
import unittest

# Headings (text after the #s) that each guide has had for years and that
# docs/style.md and the reviewer prompts rely on.
EXPECTED = {
    "cpp": ["Google C++ Style Guide", "Header Files", "Naming",
            "Static and Global Variables", "Comments", "Exceptions"],
    "python": ["Google Python Style Guide", "2 Python Language Rules",
               "3 Python Style Rules", "3.8 Comments and Docstrings"],
    "shell": ["Shell Style Guide", "Which Shell to Use", "Naming Conventions",
              "Function Comments"],
    "markdown": ["Markdown style guide", "Document layout", "Capitalization"],
}


class RenderedGuidesTest(unittest.TestCase):
    def test_headings(self):
        for name, path in zip(EXPECTED, sys.argv[1:5], strict=True):
            with open(path) as f:
                text = f.read()
            headings = {h.strip() for h in
                        re.findall(r"^#{1,6} (.+)$", text, re.M)}
            for want in EXPECTED[name]:
                self.assertIn(want, headings, f"{name} guide, {path}")


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
