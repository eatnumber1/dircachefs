"""The flags defined in dcfs/main.cc and the README's Flags table agree."""
import re
import sys
import unittest


class FlagsTest(unittest.TestCase):
    def test_consistent(self):
        with open(sys.argv[1]) as f:
            code = set(re.findall(r"ABSL_FLAG\(\s*[^,]+,\s*(\w+),", f.read()))
        with open(sys.argv[2]) as f:
            readme = f.read()
        m = re.search(r"^### Flags\n(.*?)^#{1,3} ", readme, re.S | re.M)
        self.assertTrue(m, "README has no Flags section")
        table = set(re.findall(r"^\| `--(\w+)`", m.group(1), re.M))
        self.assertTrue(code, "no ABSL_FLAG found")
        self.assertEqual(code - table, set(), "in main.cc, not in README")
        self.assertEqual(table - code, set(), "in README, not in main.cc")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
