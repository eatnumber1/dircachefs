"""The banned-symbols gate over //dcfs:main_static (plan step 26.8)."""

import sys
import unittest

import banned_symbols

ARGS = {}  # nm, deny_list, objects, binary: filled from the command line


class BannedSymbolsTest(unittest.TestCase):

    def test_shipped_binary_references_no_banned_symbol(self):
        with open(ARGS["deny_list"], encoding="utf-8") as f:
            deny_text = f.read()
        problems = banned_symbols.check(
            ARGS["nm"], banned_symbols.read_objects(ARGS["objects"]),
            ARGS["binary"], deny_text)
        self.assertEqual([], problems, "\n" + "\n".join(problems))

    def test_every_allow_line_is_used(self):
        # A stale allowance would silently permit a future reference.
        with open(ARGS["deny_list"], encoding="utf-8") as f:
            deny_text = f.read()
        used = set()
        banned_symbols.check(
            ARGS["nm"], banned_symbols.read_objects(ARGS["objects"]),
            ARGS["binary"], deny_text, used)
        _, allows = banned_symbols.parse_deny_list(deny_text)
        unused = [a for a in allows if a not in used]
        self.assertEqual([], unused, "allow lines that match nothing")


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
