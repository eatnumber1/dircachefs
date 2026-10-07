"""Self-check of man/flags_consistency_test.py (step 26.1).

Runs the real flags_consistency_test.py, as a subprocess with this
interpreter, over canned main.cc / README pairs: a matching pair must pass, a
flag only in main.cc and a flag only in the README must each fail with the
gate's own message.

usage: flags_consistency_test_self_check.py FLAGS_CONSISTENCY_TEST.PY
"""
import os
import subprocess
import sys
import tempfile
import unittest

GATE = None


def main_cc(flags):
    return ''.join(f'ABSL_FLAG(std::string, {f}, "", "help");\n'
                   for f in flags)


def readme(flags):
    rows = ''.join(f'| `--{f}` | string | help |\n' for f in flags)
    return f'# dcfs\n\n### Flags\n\n| Flag | Type | Meaning |\n{rows}\n## Next\n'


def run_gate(code_flags, readme_flags):
    """Returns the real gate's CompletedProcess over the canned pair."""
    with tempfile.TemporaryDirectory() as tmp:
        code, doc = os.path.join(tmp, 'main.cc'), os.path.join(tmp, 'README')
        with open(code, 'w') as f:
            f.write(main_cc(code_flags))
        with open(doc, 'w') as f:
            f.write(readme(readme_flags))
        return subprocess.run([sys.executable, GATE, code, doc],
                              capture_output=True, text=True, check=False)


class SelfCheck(unittest.TestCase):
    def test_matching_pair_passes(self):
        result = run_gate(['source', 'cache_db'], ['source', 'cache_db'])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_flag_missing_from_readme_is_rejected(self):
        result = run_gate(['source', 'cache_db'], ['source'])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'cache_db' : in main.cc, not in README",
                      result.stderr)

    def test_flag_missing_from_main_cc_is_rejected(self):
        result = run_gate(['source'], ['source', 'stale'])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'stale' : in README, not in main.cc",
                      result.stderr)


if __name__ == '__main__':
    GATE = sys.argv.pop(1)
    unittest.main()
