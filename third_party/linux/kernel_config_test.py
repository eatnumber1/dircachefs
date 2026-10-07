"""Checks Alpine's linux-virt kernel for every option the tests need.

Step 24.2. required_options.txt lists, each with the reason, the kernel
options the guests rely on (FUSE and its passthrough, the three backing
filesystems, namespaces, ...). The kernel is Alpine's, so we do not choose
its configuration; this test fails when a branch of Alpine's kernel does not
offer one: every listed option must be built in (=y) or a module (=m) in the
package's /boot/config-*, never off. A module still has to be listed in a
test's `modules` (test/qemu/qemu_test.bzl) to be loaded.
"""

import re
import sys
import unittest


def parse_options(text):
    """Returns the CONFIG_X names a required-options file asks for."""
    return re.findall(r'^CONFIG_([A-Z0-9_]+)=y$', text, re.MULTILINE)


def parse_config(text):
    """Returns {option: value} of a kernel .config (unset options absent)."""
    return dict(re.findall(r'^CONFIG_([A-Z0-9_]+)=(.*)$', text, re.MULTILINE))


def missing(options, config):
    """Returns the options that are neither built in nor modules."""
    return [o for o in options if config.get(o) not in ('y', 'm')]


class KernelConfigTest(unittest.TestCase):

    def test_alpine_kernel_has_every_required_option(self):
        with open(OPTIONS_FILE, encoding='utf-8') as f:
            options = parse_options(f.read())
        with open(CONFIG_FILE, encoding='utf-8') as f:
            config = parse_config(f.read())
        self.assertGreater(len(options), 50)
        self.assertEqual(
            missing(options, config), [],
            "these options of third_party/linux/required_options.txt are "
            "off in Alpine's linux-virt")


class CheckerTest(unittest.TestCase):

    def test_module_and_builtin_are_accepted(self):
        self.assertEqual(missing(['A', 'B'], {'A': 'y', 'B': 'm'}), [])

    def test_off_and_absent_options_are_reported(self):
        config = parse_config('# CONFIG_B is not set\nCONFIG_C=n\n')
        self.assertEqual(missing(['A', 'B', 'C'], config), ['A', 'B', 'C'])

    def test_option_lines_only(self):
        self.assertEqual(
            parse_options('# CONFIG_X=y\nCONFIG_A=y\nCONFIG_L="-x"\n'), ['A'])


if __name__ == '__main__':
    OPTIONS_FILE, CONFIG_FILE = sys.argv[1:3]
    unittest.main(argv=sys.argv[:1])
