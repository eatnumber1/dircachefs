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


def readme(flags, options=('fstype',)):
    rows = ''.join(f'| `dcfs.{f}` | string | help |\n' for f in flags)
    opts = ''.join(f'| `dcfs.{o}` | string | help |\n' for o in options)
    return ('# dcfs\n\n### Options\n\n| Option | Type | Meaning |\n'
            f'{opts}\n### Flags\n\n| Flag | Type | Meaning |\n{rows}\n'
            '## Next\n')


def wrapper_cc(options):
    return ''.join(f'  if (name == "{o}") {{}}\n' for o in options)


def run_gate(code_flags, readme_flags, wrapper_options=('fstype',),
             readme_options=('fstype',)):
    """Returns the real gate's CompletedProcess over the canned pair."""
    with tempfile.TemporaryDirectory() as tmp:
        code, doc = os.path.join(tmp, 'main.cc'), os.path.join(tmp, 'README')
        wrapper = os.path.join(tmp, 'mount_dcfs.cc')
        with open(code, 'w') as f:
            f.write(main_cc(code_flags))
        with open(doc, 'w') as f:
            f.write(readme(readme_flags, readme_options))
        with open(wrapper, 'w') as f:
            f.write(wrapper_cc(wrapper_options))
        return subprocess.run([sys.executable, GATE, code, doc, wrapper],
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

    def test_wrapper_option_missing_from_readme_is_rejected(self):
        result = run_gate(['source'], ['source'],
                          wrapper_options=('fstype', 'newopt'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'newopt' : in mount_dcfs.cc, not in README",
                      result.stderr)


if __name__ == '__main__':
    GATE = sys.argv.pop(1)
    unittest.main()
