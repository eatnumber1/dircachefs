"""Self-check of the shipped-dependency golden (plan steps 26.1 and 26.9).

The real comparison over the real graph and a golden with one line removed
(or one added) must fail, with the diff and the sentence.
"""

import sys
import unittest

import shipped_deps

ARGS = {}


def read(key):
    with open(ARGS[key], encoding="utf-8") as f:
        return f.read()


class ShippedDepsSelfCheckTest(unittest.TestCase):

    def test_control_real_golden_passes(self):
        self.assertEqual("", shipped_deps.compare(read("graph"), read("golden")))

    def test_golden_missing_a_line_fails_with_diff_and_sentence(self):
        names = shipped_deps.golden_repos(read("golden"))
        self.assertTrue(names)
        truncated = "\n".join(names[1:]) + "\n"
        problem = shipped_deps.compare(read("graph"), truncated)
        self.assertIn("+" + names[0], problem)
        self.assertIn("a new dependency in the shipped binary is a deliberate "
                      "edit of dcfs/shipped_deps.txt", problem)

    def test_golden_with_an_extra_line_fails(self):
        problem = shipped_deps.compare(
            read("graph"), read("golden") + "left_over+\n")
        self.assertIn("-left_over+", problem)

    def test_excluded_repositories_do_not_count(self):
        graph = read("graph") + "@@platforms//os:linux\n@@rules_cc+//x:y\n"
        self.assertEqual("", shipped_deps.compare(graph, read("golden")))


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
