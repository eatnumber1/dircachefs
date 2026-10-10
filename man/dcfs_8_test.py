"""Checks the generated dcfs(8) page, read back by pandoc as plain text."""
import re
import sys
import unittest

# Flags whose default the page must state, with the README's spelling.
DEFAULTS = {
    "attr_timeout_sec": "3600", "entry_timeout_sec": "3600",
    "sync_interval_sec": "5", "ro": "off",
    "foreground": "off",
}

# The ABSL_FLAGs of dcfs/main.cc and the options of the mount.dcfs wrapper,
# each set as the mount option dcfs.<name> (man/flags_consistency_test.py
# keeps this list and the README honest).
FLAGS = [
    "attr_timeout_sec", "entry_timeout_sec", "sync_interval_sec",
    "fstype", "cache_db", "ro", "foreground", "fuse_opt",
]
SECTIONS = [
    "NAME", "SYNOPSIS", "OPTIONS", "FLAGS", "EXAMPLE",
    "THE BIND FORM AND THE NATIVE BIND RECIPE", "RUNNING UNDER SYSTEMD",
    "EXPORTING OVER NFS", "SHUTDOWN, CRASHES AND RESTARTS",
    "CHECKING A DCFS FILESYSTEM", "LIMITATIONS",
    "SEE ALSO",
]


class ManPageTest(unittest.TestCase):
    def setUp(self):
        with open(sys.argv[1]) as f:
            self.text = f.read()

    def test_sections(self):
        headings = set(re.findall(r"^([A-Z][A-Z ,]+)$", self.text, re.M))
        for s in SECTIONS:
            self.assertIn(s, headings)

    def test_flags(self):
        for f in FLAGS:
            self.assertIn("dcfs." + f, self.text)

    def test_defaults(self):
        for f, d in DEFAULTS.items():
            self.assertIn("dcfs.%s (default: %s)" % (f, d), self.text)
        self.assertIn("dcfs.cache_db (required)", self.text)

    def test_no_table(self):
        self.assertIsNone(re.search(r"^[-+─═ ]{10,}$", self.text, re.M))
        self.assertNotIn("|", self.text)

    def test_nothing_unrendered(self):
        self.assertNotIn("<?", self.text)
        self.assertNotIn("|---", self.text)
        self.assertIsNone(re.search(r"^\s*\|", self.text, re.M))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
