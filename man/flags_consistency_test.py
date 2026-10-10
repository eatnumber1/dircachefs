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
        table = set(re.findall(r"^\| `dcfs\.(\w+)`", m.group(1), re.M))
        self.assertTrue(code, "no ABSL_FLAG found")
        self.assertEqual(code - table, set(), "in main.cc, not in README")
        self.assertEqual(table - code, set(), "in README, not in main.cc")
        # Every option of the wrapper (dcfs/mount_dcfs.cc: the names it
        # compares and its table of settable flags) is in the README's
        # Options or Flags tables.
        with open(sys.argv[3]) as f:
            wrapper = f.read()
        options = set(re.findall(r'name == "(\w+)"', wrapper))
        block = re.search(r"kSettableFlags\[\] = \{(.*?)\};", wrapper, re.S)
        self.assertTrue(block, "no kSettableFlags in mount_dcfs.cc")
        settable = set(re.findall(r'"(\w+)"', block.group(1)))
        options |= settable
        # allow_other is refused, not an option (README "allow_other is
        # always on", step 15.8).
        options.discard("allow_other")
        # An ABSL_FLAG the wrapper cannot set as dcfs.<flag> is unreachable.
        self.assertEqual(code - settable, set(),
                         "ABSL_FLAG in main.cc, not settable in mount_dcfs.cc")
        o = re.search(r"^### Options\n(.*?)^#{1,3} ", readme, re.S | re.M)
        self.assertTrue(o, "README has no Options section")
        documented = table | set(
            re.findall(r"^\| `dcfs\.(\w+)`", o.group(1), re.M))
        documented |= set(re.findall(r"^- `dcfs\.(\w+)`", m.group(1), re.M))
        self.assertTrue(options, "no wrapper options found")
        self.assertEqual(options - documented, set(),
                         "in mount_dcfs.cc, not in README")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
