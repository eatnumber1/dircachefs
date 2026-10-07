"""Tests for mkmodules.py (step 24.2), on a synthetic module tree."""

import gzip
import os
import tempfile
import unittest

import mkmodules


def read_cpio(data):
    """Returns {path: bytes} of a gzip'd newc archive."""
    data = gzip.decompress(data)
    files, pos = {}, 0
    while True:
        assert data[pos:pos + 6] == b'070701'
        fields = [int(data[pos + 6 + 8 * i:pos + 14 + 8 * i], 16)
                  for i in range(13)]
        size, namesize = fields[6], fields[11]
        pos += 110
        name = data[pos:pos + namesize - 1].decode()
        pos += namesize
        pos += -pos % 4
        content = data[pos:pos + size]
        pos += size
        pos += -pos % 4
        if name == 'TRAILER!!!':
            return files
        files[name] = content


class MkModulesTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.release = '6.18.1-0-virt'
        self.dir = os.path.join(self.tmp.name, 'lib/modules', self.release)
        os.makedirs(os.path.join(self.dir, 'kernel/fs/fs-a'))
        os.makedirs(os.path.join(self.dir, 'kernel/crypto'))
        for path, content in [
            ('kernel/fs/fs-a/fs-a.ko.gz', b'fs-a'),
            ('kernel/fs/lib.ko.gz', b'lib'),
            ('kernel/fs/jbd.ko.gz', b'jbd'),
            ('kernel/crypto/sum-impl.ko.gz', b'sum'),
        ]:
            with open(os.path.join(self.dir, path), 'wb') as f:
                f.write(gzip.compress(content))
        self.write('modules.dep',
                   'kernel/fs/fs-a/fs-a.ko.gz: kernel/fs/lib.ko.gz '
                   'kernel/fs/jbd.ko.gz\n'
                   'kernel/fs/lib.ko.gz:\n'
                   'kernel/fs/jbd.ko.gz: kernel/fs/lib.ko.gz\n'
                   'kernel/crypto/sum-impl.ko.gz:\n')
        self.write('modules.builtin', 'kernel/crypto/sha256.ko\n')
        self.write('modules.alias', 'alias crypto-sum sum_impl\n'
                   'alias sum sum_impl\n')
        self.write('modules.softdep', 'softdep fs_a pre: sum sha256\n')

    def write(self, name, text):
        with open(os.path.join(self.dir, name), 'w') as f:
            f.write(text)

    def paths(self, *modules):
        tree = mkmodules.find_tree(self.tmp.name)
        return tree.load_order(modules)

    def test_dependencies_load_first_and_softdeps_by_alias(self):
        self.assertEqual(self.paths('fs-a'), [
            'kernel/crypto/sum-impl.ko.gz',
            'kernel/fs/lib.ko.gz',
            'kernel/fs/jbd.ko.gz',
            'kernel/fs/fs-a/fs-a.ko.gz',
        ])

    def test_names_accept_dash_or_underscore(self):
        self.assertEqual(self.paths('fs_a'), self.paths('fs-a'))

    def test_shared_dependency_is_loaded_once(self):
        self.assertEqual(self.paths('jbd', 'fs-a').count('kernel/fs/lib.ko.gz'),
                         1)

    def test_builtin_module_is_skipped(self):
        self.assertEqual(self.paths('sha256'), [])

    def test_a_name_that_is_an_alias_of_a_builtin_module_is_skipped(self):
        self.write('modules.alias', 'alias crypto-sum sum_impl\n'
                   'alias sum sum_impl\n'
                   'alias crypto-hash sha256\n')
        self.assertEqual(self.paths('hash'), [])

    def test_unknown_module_is_refused(self):
        with self.assertRaisesRegex(mkmodules.ModulesError, "'nonesuch'"):
            self.paths('nonesuch')

    def test_archive_holds_decompressed_modules_and_the_load_list(self):
        files = read_cpio(mkmodules.build(self.tmp.name, ['jbd']))
        base = f'lib/modules/{self.release}/kernel/fs/'
        self.assertEqual(files[base + 'jbd.ko'], b'jbd')
        self.assertEqual(files[base + 'lib.ko'], b'lib')
        self.assertEqual(
            files['etc/dcfs-modules'].decode().split(), [
                f'/lib/modules/{self.release}/kernel/fs/lib.ko',
                f'/lib/modules/{self.release}/kernel/fs/jbd.ko',
            ])

    def test_two_kernel_releases_are_refused(self):
        os.makedirs(os.path.join(self.tmp.name, 'lib/modules/other'))
        with self.assertRaisesRegex(mkmodules.ModulesError, 'exactly 1'):
            mkmodules.find_tree(self.tmp.name)

    def test_command_line_reports_the_error(self):
        out = os.path.join(self.tmp.name, 'm.cpio.gz')
        self.assertEqual(
            mkmodules.main(['--root', self.tmp.name, '--out', out, 'zzz']), 1)
        self.assertFalse(os.path.exists(out))


if __name__ == '__main__':
    unittest.main()
