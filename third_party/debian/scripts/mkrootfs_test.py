"""Tests mkrootfs.py with the real Alpine tools on a small synthetic tar.

Host-side: it needs neither root nor a kernel. Checks, through debugfs, that
the image has the ownership, modes, links and fixed-up paths the tar asks
for, that entries behind a symlinked directory land inside the image, and
(as a control) that without the ownership step everything belongs to the
user who built it.

usage: mkrootfs_test.py MKE2FS DEBUGFS MKE2FS_CONF
"""

import contextlib
import io
import os
import re
import subprocess
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
            MKE2FS, '--debugfs', DEBUGFS, '--mke2fs-conf', CONF,
            '--source-date-epoch', EPOCH
        ] + extra)
        assert rc == 0, 'mkrootfs.py failed'
        return out

    def debugfs(self, image, command):
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

    def mtime(self, path, image=None):
        text = self.debugfs(image or self.image, f'stat "{path}"')
        return int(re.search(r'mtime: 0x([0-9a-f]+)', text).group(1), 16)

    def test_package_mtimes_survive(self):
        # make_tar gives every entry the mtime 1700000000; the image's
        # clock (SOURCE_DATE_EPOCH) is later, so nothing is clamped.
        for path in ('/usr/bin/mount', '/bin', '/usr/bin', '/var/local',
                     '/etc/dir-secret/key'):
            self.assertEqual(self.mtime(path), 1700000000, path)

    def test_times_made_here_are_clamped_not_the_build_time(self):
        # A directory the tar lacks, and the fixed-up paths, get a time made
        # at build, which the epoch clamps: the image is reproducible.
        self.assertLessEqual(self.mtime('/var/lib/dpkg'), int(EPOCH))
        self.assertLessEqual(self.mtime('/etc/exports'), int(EPOCH))

    def test_the_image_is_reproducible(self):
        again = self.build('again.ext4', [])
        with open(again, 'rb') as a, open(self.image, 'rb') as b:
            self.assertEqual(a.read(), b.read())


def run_main(entries, extra=()):
    """Builds a tar of entries (a function of the archive) and runs main.

    Returns (exit code, stderr text).
    """
    with tempfile.TemporaryDirectory() as tmp:
        flat = os.path.join(tmp, 'flat.tar')
        with tarfile.open(flat, 'w') as archive:
            entries(archive)
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            rc = mkrootfs.main([
                '--flat', flat, '--out', os.path.join(tmp, 'out.ext4'),
                '--size', '16M', '--mke2fs', MKE2FS, '--debugfs', DEBUGFS,
                '--mke2fs-conf', CONF, '--source-date-epoch', EPOCH, *extra
            ])
        return rc, stderr.getvalue(), os.path.join(tmp, 'out.ext4')


class HostileTarTest(unittest.TestCase):
    """What a tar may not do, and what it must be refused for, cleanly."""

    def test_a_hard_link_to_an_absolute_symlink_stays_in_the_tree(self):
        with tempfile.TemporaryDirectory() as host:
            secret = os.path.join(host, 'secret')
            with open(secret, 'w') as f:
                f.write('host data')

            def entries(t):
                _entry(t, './link', 'symlink', 0o777, link=secret)
                _entry(t, './hl', 'hardlink', link='./link')

            with tempfile.TemporaryDirectory() as tmp:
                flat = os.path.join(tmp, 'f.tar')
                with tarfile.open(flat, 'w') as t:
                    entries(t)
                out = os.path.join(tmp, 'o.ext4')
                self.assertEqual(mkrootfs.main([
                    '--flat', flat, '--out', out, '--size', '16M',
                    '--mke2fs', MKE2FS, '--debugfs', DEBUGFS,
                    '--mke2fs-conf', CONF, '--source-date-epoch', EPOCH
                ]), 0)
                text = subprocess.run([DEBUGFS, '-R', 'stat /hl', out],
                                      capture_output=True, text=True).stdout
                self.assertIn('Type: symlink', text)
                dump = subprocess.run([DEBUGFS, '-R', 'cat /hl', out],
                                      capture_output=True).stdout
                self.assertNotIn(b'host data', dump)

    def test_a_hard_link_to_a_dangling_symlink_is_made(self):
        def entries(t):
            _entry(t, './link', 'symlink', 0o777, link='/nowhere/at/all')
            _entry(t, './hl', 'hardlink', link='./link')
        rc, err, _ = run_main(entries)
        self.assertEqual(rc, 0, err)

    def test_a_hard_link_to_nothing_is_refused(self):
        rc, err, _ = run_main(
            lambda t: _entry(t, './hl', 'hardlink', link='./missing'))
        self.assertEqual(rc, 1)
        self.assertIn('missing', err)

    def test_a_final_dotdot_component_is_refused(self):
        rc, err, _ = run_main(lambda t: _entry(t, './a/..', 'file'))
        self.assertEqual(rc, 1)
        self.assertIn('mkrootfs.py:', err)

    def test_a_file_over_a_directory_is_refused(self):
        def entries(t):
            _entry(t, './d/', 'dir')
            _entry(t, './d', 'file', data=b'x')
        rc, err, _ = run_main(entries)
        self.assertEqual(rc, 1)
        self.assertIn('mkrootfs.py:', err)

    def test_a_name_that_is_not_utf8_is_refused_clearly(self):
        def entries(t):
            info = tarfile.TarInfo('./caf\udce9')  # a lone 0xE9 byte
            info.size = 1
            t.addfile(info, io.BytesIO(b'x'))
        rc, err, _ = run_main(entries)
        self.assertEqual(rc, 1)
        self.assertIn('UTF-8', err)

    def test_a_package_newer_than_the_epoch_is_refused(self):
        # The clock must be later than every package's mtime, or e2fsprogs
        # would clamp it: the snapshot date moved and the epoch did not.
        rc, err, _ = run_main(lambda t: _entry(t, './f', 'file'),
                              ['--source-date-epoch', '1600000000'])
        self.assertEqual(rc, 1)
        self.assertIn('source-date-epoch', err)


if __name__ == '__main__':
    # 1700000000 is every test entry's mtime: later, but before any real
    # build, so what the host makes is clamped to it.
    MKE2FS, DEBUGFS, CONF = sys.argv[1:4]
    EPOCH = '1700000100'
    unittest.main(argv=sys.argv[:1])
