"""The style checks over our C++ (plan step 25.21).

Reads the clang-tidy and clang-query outputs that //tools:style_checks.bzl
produced for dcfs/, dcfs/testonly/, bench/ and tools/ and fails on a finding
that tools/style_checks_allow.txt does not list, and on a listed one that is
gone (the list only shrinks). The report of every rule, report-only ones
included, is printed. See style_checks.py.

Arguments: allowlist=PATH tidy_config=PATH, then the output files.
"""

import sys
import unittest

import style_checks

ARGS = {"outputs": []}


class StyleChecksTest(unittest.TestCase):

    def test_no_new_finding_and_no_stale_allowlist_line(self):
        with open(ARGS["allowlist"], encoding="utf-8") as f:
            allowlist = f.read()
        with open(ARGS["tidy_config"], encoding="utf-8") as f:
            tidy_config = f.read()
        problems, report = style_checks.run(ARGS["outputs"], allowlist,
                                            tidy_config)
        print("\n".join(report))
        if problems:
            self.fail("\n" + "\n".join(problems))


def main():
    rest = [sys.argv[0]]
    for arg in sys.argv[1:]:
        key, sep, value = arg.partition("=")
        if sep and key in ("allowlist", "tidy_config"):
            ARGS[key] = value
        elif arg.endswith((".tidy.txt", ".query.txt")):
            ARGS["outputs"].append(arg)
        elif arg.endswith(".flags.txt"):
            # The compile flags of each file, for tools/style_fix.py (25.23).
            continue
        else:
            rest.append(arg)
    unittest.main(argv=rest)


if __name__ == "__main__":
    main()
