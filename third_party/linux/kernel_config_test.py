"""Checks Alpine's linux-virt kernel for every option the tests need.

Step 24.2. required_options.txt lists, each with the reason, the kernel
options the guests rely on (FUSE and its passthrough, the three backing
filesystems, namespaces, ...). The kernel is Alpine's, so we do not choose
its configuration; this test fails when a branch of Alpine's kernel does not
offer one: every option listed `=y` must be built in or a module in the package's
/boot/config-*, never off; every option listed `=builtin` must be built in,
because nothing can load a module before it is needed (the console, the
virtio-mmio devices the kernel learns of from its command line, the
initramfs, module loading itself). A module still has to be listed in a
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


def parse_builtin_options(text):
    """Returns the options an option list says must be built in (=builtin)."""
    return re.findall(r'^CONFIG_([A-Z0-9_]+)=builtin$', text, re.MULTILINE)


def missing_builtin(options, config):
    """Returns the options that are not built in (a module is not enough)."""
    return [o for o in options if config.get(o) != 'y']


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
        with open(OPTIONS_FILE, encoding='utf-8') as f:
            builtin = parse_builtin_options(f.read())
        self.assertEqual(
            missing_builtin(builtin, config), [],
            "these options of third_party/linux/required_options.txt must "
            "be built in and are modules or off in Alpine's linux-virt")


class BuiltinTest(unittest.TestCase):
    """Options that must be built in: a module there is as good as off."""

    def test_a_module_does_not_satisfy_a_builtin_option(self):
        # insmod runs after the kernel found its console, its virtio-mmio
        # devices (insmod does not read /proc/cmdline) and its initramfs.
        config = parse_config('CONFIG_VIRTIO_MMIO=m\nCONFIG_SERIAL_8250=y\n')
        self.assertEqual(
            missing_builtin(['VIRTIO_MMIO', 'SERIAL_8250'], config),
            ['VIRTIO_MMIO'])

    def test_off_and_absent_builtin_options_are_reported(self):
        config = parse_config('# CONFIG_B is not set\n')
        self.assertEqual(missing_builtin(['A', 'B'], config), ['A', 'B'])

    def test_the_two_kinds_are_told_apart_in_the_option_list(self):
        text = 'CONFIG_A=y\nCONFIG_B=builtin\n# CONFIG_C=builtin\nCONFIG_D=n\n'
        self.assertEqual(parse_options(text), ['A'])
        self.assertEqual(parse_builtin_options(text), ['B'])

    def test_the_real_list_has_the_boot_path_as_builtin(self):
        with open(OPTIONS_FILE, encoding='utf-8') as f:
            builtin = parse_builtin_options(f.read())
        for option in ('VIRTIO_MMIO', 'VIRTIO_MMIO_CMDLINE_DEVICES',
                       'SERIAL_8250', 'SERIAL_8250_CONSOLE', 'BLK_DEV_INITRD',
                       'MODULES'):
            self.assertIn(option, builtin)


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
