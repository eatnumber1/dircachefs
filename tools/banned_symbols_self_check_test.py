"""Self-check of the banned-symbols gate (plan steps 26.1 and 26.8).

Runs the real checker and the real deny list over a tiny program that calls
realpath(3): the gate must fail, name the symbol and its rule, and an `allow`
line must silence exactly the origin and symbol it names.
"""

import sys
import unittest

import banned_symbols

ARGS = {}  # nm, deny_list, objects, binary: filled from the command line


def deny_text():
    with open(ARGS["deny_list"], encoding="utf-8") as f:
        return f.read()


def run_check(text):
    return banned_symbols.check(
        ARGS["nm"], banned_symbols.read_objects(ARGS["objects"]),
        ARGS["binary"], text)


class BannedSymbolsSelfCheckTest(unittest.TestCase):

    def test_program_calling_realpath_fails_naming_symbol_and_rule(self):
        problems = run_check(deny_text())
        self.assertTrue(
            any("realpath" in p and "no paths after startup" in p
                for p in problems), problems)

    def test_program_that_sleeps_fails_naming_the_no_timers_rule(self):
        problems = run_check(deny_text())
        self.assertTrue(
            any("sleep" in p and "no timers" in p for p in problems),
            problems)

    def test_symbol_defined_in_binary_but_referenced_by_no_object_fails(self):
        problems = run_check(deny_text())
        self.assertTrue(
            any("nftw64 is in the linked binary" in p for p in problems),
            problems)
        allowed = run_check(deny_text() + "allow <runtime> nftw64 | test\n")
        self.assertFalse([p for p in allowed if "nftw64" in p], allowed)

    def test_empty_deny_list_passes(self):
        # Control: the failure above comes from the deny list.
        self.assertEqual([], run_check(""))

    def test_allow_line_silences_its_origin_only(self):
        text = deny_text()
        origin = banned_symbols.read_objects(ARGS["objects"])[0][0]
        allowed = run_check(
            text + "allow %s realpath | test\n" % origin)
        self.assertFalse([p for p in allowed if "realpath" in p], allowed)
        other = run_check(text + "allow nowhere/* realpath | test\n")
        self.assertTrue([p for p in other if "realpath" in p], other)

    def test_malformed_lines_are_rejected(self):
        for bad in ("ban realpath\n", "allow realpath | why\n", "x | y\n"):
            with self.assertRaises(banned_symbols.DenyListError):
                banned_symbols.parse_deny_list(bad)


def main():
    rest = []
    for arg in sys.argv[1:]:
        key, sep, value = arg.partition("=")
        if sep and key in ("nm", "deny_list", "objects", "binary"):
            ARGS[key] = value
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
