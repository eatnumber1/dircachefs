"""Tests for mkfstools.py (step 26.17), on a synthetic Alpine tree."""

import gzip
import os
import tempfile
import unittest

import mkfstools


def read_cpio(data):
    """Returns {path: (mode, bytes)} of a gzip'd newc archive."""
    data = gzip.decompress(data)
    files, pos = {}, 0
    while True:
        assert data[pos:pos + 6] == b'070701'
        fields = [int(data[pos + 6 + 8 * i:pos + 14 + 8 * i], 16)
                  for i in range(13)]
        mode, size, namesize = fields[1], fields[6], fields[11]
        pos += 110
        name = data[pos:pos + namesize - 1].decode()
        pos += namesize
        pos += -pos % 4
        content = data[pos:pos + size]
        pos += size
        pos += -pos % 4
        if name == 'TRAILER!!!':
            return files
        files[name] = (mode, content)


class MkFstoolsTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = os.path.join(self.tmp.name, 'root')
        for d in ('sbin', 'lib', 'usr/lib'):
            os.makedirs(os.path.join(self.root, d))
        for path, content in [
            ('sbin/mke2fs', b'mke2fs'),
            ('sbin/mkfs.xfs', b'xfs'),
            ('sbin/mkfs.btrfs', b'btrfs'),
            ('sbin/e2fsck', b'not wanted'),
            ('lib/ld-musl-x86_64.so.1', b'musl'),
            ('usr/lib/libz.so.1.3.2', b'zlib'),
            ('usr/lib/libext2fs.so.2.4', b'ext2fs'),
        ]:
            with open(os.path.join(self.root, path), 'wb') as f:
                f.write(content)
        for link, target in [
            ('lib/libc.musl-x86_64.so.1', 'ld-musl-x86_64.so.1'),
            ('usr/lib/libz.so.1', 'libz.so.1.3.2'),
            ('usr/lib/libz.so', 'libz.so.1'),
            ('usr/lib/libext2fs.so.2', 'libext2fs.so.2.4'),
        ]:
            os.symlink(target, os.path.join(self.root, link))
        self.conf = os.path.join(self.tmp.name, 'mke2fs.conf')
        with open(self.conf, 'wb') as f:
            f.write(b'[defaults]\n')

    def files(self):
        return read_cpio(mkfstools.build(self.root, self.conf))

    def test_the_three_mkfs_tools_and_the_checked_in_profile(self):
        files = self.files()
        self.assertEqual(files['opt/fstools/sbin/mke2fs'][1], b'mke2fs')
        self.assertEqual(files['opt/fstools/sbin/mkfs.xfs'][1], b'xfs')
        self.assertEqual(files['opt/fstools/sbin/mkfs.btrfs'][1], b'btrfs')
        self.assertEqual(files['opt/fstools/etc/mke2fs.conf'][1],
                         b'[defaults]\n')

    def test_tools_are_executable_and_nothing_else_of_sbin_comes(self):
        files = self.files()
        self.assertEqual(files['opt/fstools/sbin/mke2fs'][0], 0o100755)
        self.assertNotIn('opt/fstools/sbin/e2fsck', files)

    def test_each_library_is_stored_once_under_its_real_name(self):
        files = self.files()
        self.assertEqual(files['opt/fstools/usr/lib/libz.so.1.3.2'][1], b'zlib')
        self.assertEqual(files['opt/fstools/usr/lib/libz.so.1'],
                         (0o120777, b'libz.so.1.3.2'))
        self.assertEqual(files['opt/fstools/lib/libc.musl-x86_64.so.1'],
                         (0o120777, b'ld-musl-x86_64.so.1'))
        self.assertEqual(files['opt/fstools/usr/lib/libext2fs.so.2'],
                         (0o120777, b'libext2fs.so.2.4'))

    def test_only_sonames_are_listed(self):
        self.assertNotIn('opt/fstools/usr/lib/libz.so', self.files())

    def test_a_missing_tool_is_an_error(self):
        os.unlink(os.path.join(self.root, 'sbin/mkfs.xfs'))
        with self.assertRaisesRegex(mkfstools.FstoolsError, 'mkfs.xfs'):
            mkfstools.build(self.root, self.conf)

    def test_command_line_writes_the_archive(self):
        out = os.path.join(self.tmp.name, 'f.cpio.gz')
        self.assertEqual(
            mkfstools.main(['--root', self.root, '--mke2fs-conf', self.conf,
                            '--out', out]), 0)
        self.assertIn('opt/fstools/sbin/mke2fs', read_cpio(open(out, 'rb').read()))


if __name__ == '__main__':
    unittest.main()
