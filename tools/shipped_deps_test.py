"""The shipped-dependency golden (plan step 26.9)."""

import sys
import unittest

import shipped_deps

ARGS = {}


class ShippedDepsTest(unittest.TestCase):

    def test_linked_repositories_match_the_golden(self):
        with open(ARGS["graph"], encoding="utf-8") as f:
            graph = f.read()
        with open(ARGS["golden"], encoding="utf-8") as f:
            golden = f.read()
        self.assertEqual("", shipped_deps.compare(graph, golden))


def main():
    rest = []
    for arg in sys.argv[1:]:
        key, sep, value = arg.partition("=")
        if sep and key in ("graph", "golden"):
            ARGS[key] = value
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
