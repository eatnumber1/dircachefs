"""Self-check of rootfs_invariant_test.py (step 26.1).

Builds a small synthetic tar and its image with the real mkrootfs.py and
Alpine tools (as mkrootfs_test.py does), then runs the real
expected_entries(), stat_all() and header_problems() of
rootfs_invariant_test.py: over the tar the image was made from they report
nothing; over a tar whose headers were altered after the image was made (a
mode, an owner) they report exactly those paths with the tar's and the
image's attributes.

usage: rootfs_invariant_self_check.py MKE2FS DEBUGFS MKE2FS_CONF
"""

import io
import os
import sys
import tarfile
import tempfile
import unittest

import mkrootfs
import rootfs_invariant_test as gate


def make_tar(path, mount_mode=0o4755, file_uid=0):
    with tarfile.open(path, 'w') as t:
        def add(name, kind, mode, uid=0, data=b''):
            info = tarfile.TarInfo(name)
            info.mode, info.uid, info.gid = mode, uid, 0
            info.mtime = 1700000000
            if kind == 'dir':
                info.type = tarfile.DIRTYPE
            else:
                info.size = len(data)
            t.addfile(info, io.BytesIO(data) if kind == 'file' else None)

        add('./', 'dir', 0o755)
        add('./usr/', 'dir', 0o755)
        add('./usr/bin/', 'dir', 0o755)
        add('./usr/bin/mount', 'file', mount_mode, data=b'mount')
        add('./usr/bin/other', 'file', 0o644, uid=file_uid, data=b'other')


class SelfCheck(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.flat = os.path.join(cls.tmp.name, 'flat.tar')
        make_tar(cls.flat)
        cls.image = os.path.join(cls.tmp.name, 'rootfs.ext4')
        rc = mkrootfs.main([
            '--flat', cls.flat, '--out', cls.image, '--size', '32M',
            '--mke2fs', MKE2FS, '--debugfs', DEBUGFS, '--mke2fs-conf', CONF,
            '--source-date-epoch', '1791275242'
        ])
        assert rc == 0, 'mkrootfs.py failed'
        entries, _ = gate.expected_entries(cls.flat)
        cls.got = gate.stat_all(DEBUGFS, cls.image,
                                [p for p in sorted(entries) if p])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def problems(self, **altered):
        flat = os.path.join(self.tmp.name, 'altered.tar')
        make_tar(flat, **altered)
        entries, _ = gate.expected_entries(flat)
        return gate.header_problems(entries, self.got)

    def test_matching_tar_has_no_problems(self):
        self.assertEqual(self.problems(), [])

    def test_altered_mode_is_reported(self):
        self.assertEqual(self.problems(mount_mode=0o755), [
            "/usr/bin/mount: tar ('regular', 493, 0, 0, 5), "
            "image ('regular', 2541, 0, 0, 5)"
        ])

    def test_altered_owner_is_reported(self):
        self.assertEqual(self.problems(file_uid=1000), [
            "/usr/bin/other: tar ('regular', 420, 1000, 0, 5), "
            "image ('regular', 420, 0, 0, 5)"
        ])


if __name__ == '__main__':
    MKE2FS, DEBUGFS, CONF = sys.argv[1:4]
    unittest.main(argv=sys.argv[:1])
