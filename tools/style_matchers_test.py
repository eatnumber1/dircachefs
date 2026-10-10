"""The fixtures of the clang-query matchers and of .clang-tidy (step 25.21).

Each matcher in tools/style_matchers/ must report every line its known-bad
fixture marks with `// HIT` and nothing in its known-good one; the
configuration of clang-tidy must report the marked lines of tidy_bad.cc and
nothing in tidy_good.cc; and every matcher file names the style section it
enforces. See style_checks.py (fixture_problems, matcher_file_problems).

Arguments: the matcher files (*.query), the fixtures (*.cc) and the output
files of //tools/style_matchers:matcher_findings and :tidy_findings.
"""

import os
import sys
import unittest

import style_checks

ARGS = {"outputs": [], "queries": [], "sources": []}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class StyleMatchersTest(unittest.TestCase):

    def test_matchers_report_their_bad_fixture_and_not_their_good_one(self):
        findings, _, failures = style_checks.read_outputs(ARGS["outputs"])
        if failures:
            self.fail("\n".join(failures))
        sources = {
            style_checks.normalize(path): read(path) for path in ARGS["sources"]
        }
        problems = style_checks.fixture_problems(findings, sources)
        if problems:
            self.fail("\n" + "\n".join(problems))

    def test_every_matcher_has_a_file_naming_its_style_section(self):
        texts = {
            os.path.basename(path)[:-len(".query")]: read(path)
            for path in ARGS["queries"]
        }
        problems = style_checks.matcher_file_problems(texts)
        if problems:
            self.fail("\n" + "\n".join(problems))


def main():
    rest = [sys.argv[0]]
    for arg in sys.argv[1:]:
        if arg.endswith((".tidy.txt", ".query.txt")):
            ARGS["outputs"].append(arg)
        elif arg.endswith(".query"):
            ARGS["queries"].append(arg)
        elif arg.endswith(".cc"):
            ARGS["sources"].append(arg)
        else:
            rest.append(arg)
    unittest.main(argv=rest)


if __name__ == "__main__":
    main()
