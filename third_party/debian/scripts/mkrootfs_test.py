"""Tests mkrootfs.py with the real Alpine tools on a small synthetic tar.

Host-side: it needs neither root nor a kernel. Checks, through debugfs, that
the image has the ownership, modes, links and fixed-up paths the tar asks
for, that entries behind a symlinked directory land inside the image, and
(as a control) that without the ownership step everything belongs to the
user who built it.

usage: mkrootfs_test.py MKE2FS DEBUGFS MKE2FS_CONF
"""

import io
import os
import re
import sys
import tarfile
import tempfile
import unittest

import mkrootfs


def _entry(archive, name, kind, mode=0o644, uid=0, gid=0, data=b'', link=''):
    info = tarfile.TarInfo(name)
    info.mode, info.uid, info.gid, info.mtime = mode, uid, gid, 1700000000
    if kind == 'dir':
        info.type = tarfile.DIRTYPE
    elif kind == 'symlink':
        info.type, info.linkname = tarfile.SYMTYPE, link
    elif kind == 'hardlink':
        info.type, info.linkname = tarfile.LNKTYPE, link
    else:
        info.size = len(data)
    archive.addfile(info, io.BytesIO(data) if kind == 'file' else None)


def make_tar(path):
    with tarfile.open(path, 'w') as t:
        _entry(t, './', 'dir', 0o755)
        _entry(t, './usr/', 'dir', 0o755)
        _entry(t, './usr/bin/', 'dir', 0o755)
        _entry(t, './bin', 'symlink', 0o777, link='usr/bin')
        # Behind the symlink: lands in /usr/bin.
        _entry(t, './bin/mount', 'file', 0o4755, data=b'mount')
        _entry(t, './bin/chage', 'file', 0o2755, gid=42, data=b'chage')
        _entry(t, './bin/perl', 'file', 0o755, data=b'perl')
        _entry(t, './bin/perl5', 'hardlink', link='./usr/bin/perl')
        # An absolute symlink must be followed inside the image, never on
        # the host.
        _entry(t, './abs', 'symlink', 0o777, link='/usr/share')
        _entry(t, './abs/doc', 'file', data=b'doc')
        # No directory entries at all for var/lib/dpkg.
        _entry(t, './var/lib/dpkg/status', 'file', data=b'status')
        _entry(t, './var/local', 'dir', 0o2775, gid=50)
        _entry(t, './lib/system-a\\x2db.slice', 'file', data=b'slice')
        _entry(t, './etc/dir-secret/', 'dir', 0o700)
        _entry(t, './etc/dir-secret/key', 'file', 0o600, data=b'key')


class MkRootfsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.flat = os.path.join(cls.tmp.name, 'flat.tar')
        make_tar(cls.flat)
        cls.image = cls.build('rootfs.ext4', [])
        cls.control = cls.build('control.ext4', ['--skip-ownership'])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def build(cls, name, extra):
        out = os.path.join(cls.tmp.name, name)
        rc = mkrootfs.main([
            '--flat', cls.flat, '--out', out, '--size', '32M', '--mke2fs',
            MKE2FS, '--debugfs', DEBUGFS, '--mke2fs-conf', CONF
        ] + extra)
        assert rc == 0, 'mkrootfs.py failed'
        return out

    def debugfs(self, image, command):
        import subprocess
        return subprocess.run([DEBUGFS, '-R', command, image],
                              capture_output=True, text=True,
                              check=True).stdout

    def stat(self, path, image=None):
        text = self.debugfs(image or self.image, f'stat "{path}"')
        fields = {}
        match = re.search(r'Inode: (\d+)\s+Type: (\w+)\s+Mode:\s+(\d+)', text)
        self.assertTrue(match, f'{path}: {text}')
        fields['inode'], fields['type'], fields['mode'] = (
            int(match.group(1)), match.group(2), match.group(3))
        match = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', text)
        fields['uid'], fields['gid'] = int(match.group(1)), int(match.group(2))
        return fields

    def test_entries_are_root_owned(self):
        for path in ('/', '/usr/bin/mount', '/usr/bin/perl', '/etc'):
            got = self.stat(path)
            self.assertEqual((got['uid'], got['gid']), (0, 0), path)

    def test_modes_survive(self):
        self.assertEqual(self.stat('/usr/bin/mount')['mode'], '04755')
        self.assertEqual(self.stat('/usr/bin/chage')['mode'], '02755')
        self.assertEqual(self.stat('/etc/dir-secret')['mode'], '0700')
        self.assertEqual(self.stat('/etc/dir-secret/key')['mode'], '0600')
        self.assertEqual(self.stat('/var/local')['mode'], '02775')

    def test_other_groups_survive(self):
        self.assertEqual(self.stat('/usr/bin/chage')['gid'], 42)
        self.assertEqual(self.stat('/var/local')['gid'], 50)

    def test_parent_symlinks_are_followed_inside_the_image(self):
        self.assertEqual(self.stat('/usr/bin/mount')['type'], 'regular')
        self.assertEqual(self.stat('/usr/share/doc')['type'], 'regular')
        self.assertEqual(self.stat('/bin')['type'], 'symlink')
        self.assertEqual(self.stat('/abs')['type'], 'symlink')

    def test_hard_links_share_an_inode(self):
        self.assertEqual(self.stat('/usr/bin/perl5')['inode'],
                         self.stat('/usr/bin/perl')['inode'])

    def test_missing_parents_are_created_root_owned(self):
        got = self.stat('/var/lib/dpkg')
        self.assertEqual((got['type'], got['uid'], got['gid']),
                         ('directory', 0, 0))
        self.assertEqual(self.stat('/var/lib/dpkg/status')['uid'], 0)

    def test_awkward_names(self):
        got = self.stat('/lib/system-a\\x2db.slice')
        self.assertEqual((got['type'], got['uid']), ('regular', 0))

    def test_unshipped_paths_are_made(self):
        self.assertEqual(self.stat('/etc/mtab')['type'], 'symlink')
        self.assertIn('/proc/self/mounts',
                      self.debugfs(self.image, 'stat /etc/mtab'))
        self.assertEqual(self.stat('/bin/sh', None)['type'], 'symlink')
        self.assertEqual(self.stat('/etc/exports')['type'], 'regular')
        self.assertEqual(self.stat('/usr/local/bin')['type'], 'directory')

    def test_without_the_ownership_step_files_belong_to_the_builder(self):
        if os.getuid() == 0:
            self.skipTest('running as root: the builder is root')
        got = self.stat('/usr/bin/mount', self.control)
        self.assertEqual(got['uid'], os.getuid())
        self.assertNotEqual(got['uid'], 0)

    def test_the_image_is_reproducible(self):
        again = self.build('again.ext4', [])
        with open(again, 'rb') as a, open(self.image, 'rb') as b:
            self.assertEqual(a.read(), b.read())


if __name__ == '__main__':
    MKE2FS, DEBUGFS, CONF = sys.argv[1:4]
    unittest.main(argv=sys.argv[:1])
