"""Self-check of man/dcfs_8_test.py (step 26.1).

Runs the real dcfs_8_test.py, as a subprocess with this interpreter, over
canned plain-text pages: a complete one must pass, one without the LIMITATIONS
section and one without a flag must fail with the gate's own assertion.

usage: dcfs_8_test_self_check.py DCFS_8_TEST.PY
"""
import os
import subprocess
import sys
import tempfile
import unittest

GATE = None

GOOD = """\
NAME
SYNOPSIS
OPTIONS
  dcfs.fstype (default: (autodetect))
  dcfs.cache_db (required)
  dcfs.ro (default: off)
  dcfs.foreground (default: off)
  dcfs.fuse_opt (default: (empty))
FLAGS
  dcfs.attr_timeout_sec (default: 3600)
  dcfs.entry_timeout_sec (default: 3600)
  dcfs.sync_interval_sec (default: 5)
EXAMPLE
MOUNTING OVER THE SOURCE DIRECTORY
RUNNING UNDER SYSTEMD
EXPORTING OVER NFS
SHUTDOWN, CRASHES AND RESTARTS
CHECKING A DCFS FILESYSTEM
LIMITATIONS
SEE ALSO
"""


def run_gate(page):
    """Returns the real gate's CompletedProcess over the text PAGE."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'dcfs.8.txt')
        with open(path, 'w') as f:
            f.write(page)
        return subprocess.run([sys.executable, GATE, path],
                              capture_output=True, text=True, check=False)


class SelfCheck(unittest.TestCase):
    def test_complete_page_passes(self):
        result = run_gate(GOOD)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_section_is_rejected(self):
        result = run_gate(GOOD.replace('LIMITATIONS\n', ''))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FAIL: test_sections', result.stderr)
        self.assertIn("'LIMITATIONS' not found in", result.stderr)

    def test_missing_flag_is_rejected(self):
        result = run_gate(GOOD.replace('  dcfs.attr_timeout_sec (default: 3600)\n',
                                       ''))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FAIL: test_flags', result.stderr)
        self.assertIn("'dcfs.attr_timeout_sec' not found in", result.stderr)


if __name__ == '__main__':
    GATE = sys.argv.pop(1)
    unittest.main()
