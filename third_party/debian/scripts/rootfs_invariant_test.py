"""The real Debian rootfs image matches the headers of the tar it came from.

Step 24.3. For every member of @debian//:flat (its path resolved through the
tar's own symlinks, the later of two entries for one path winning), one
batched `debugfs -f` of `stat` commands over //third_party/debian:rootfs
gives the image's type, mode, owner, group and size, and the test compares
them with the member's header, and checks that the hard-link groups are the
same sets of paths. It does not use mkrootfs.py's code: the expectation is
computed here, from the tar alone.

usage: rootfs_invariant_test.py FLAT_TAR ROOTFS_IMAGE DEBUGFS
"""

import os
import re
import stat
import subprocess
import sys
import tarfile
import unittest

_TYPES = {
    tarfile.DIRTYPE: 'directory',
    tarfile.REGTYPE: 'regular',
    tarfile.AREGTYPE: 'regular',
    tarfile.SYMTYPE: 'symlink',
}


def resolve_dir(links, parts):
    """Resolves a directory path through the tar's symlinks (lexically)."""
    done, todo, hops = [], list(parts), 0
    while todo:
        name = todo.pop(0)
        if name in ('', '.'):
            continue
        if name == '..':
            if done:
                done.pop()
            continue
        path = '/'.join(done + [name])
        if path in links:
            hops += 1
            assert hops < 40, f'symlink loop at {path}'
            target = links[path]
            if target.startswith('/'):
                done = []
            todo = target.split('/') + todo
            continue
        done.append(name)
    return done


def expected_entries(tar_path):
    """Returns ({path: (type, mode, uid, gid, size)}, {path: link target path}).

    The second maps each hard-link path to the path of its target.
    """
    with tarfile.open(tar_path) as archive:
        members = list(archive)
    links = {}
    entries, hard = {}, {}
    for member in members:
        parts = [p for p in member.name.split('/') if p not in ('', '.')]
        if not parts:
            entries[''] = ('directory', member.mode & 0o7777, member.uid,
                           member.gid, None)
            continue
        parent = resolve_dir(links, parts[:-1])
        path = '/'.join(parent + [parts[-1]])
        if member.isdir():
            # A directory entry for a symlink to a directory is that
            # directory.
            path = '/'.join(resolve_dir(links, parts))
            entries[path] = ('directory', member.mode & 0o7777, member.uid,
                             member.gid, None)
        elif member.issym():
            links[path] = member.linkname
            entries[path] = ('symlink', 0o777, member.uid, member.gid,
                             len(member.linkname.encode()))
        elif member.islnk():
            tparts = [p for p in member.linkname.split('/')
                      if p not in ('', '.')]
            target = '/'.join(resolve_dir(links, tparts[:-1]) + [tparts[-1]])
            hard[path] = target
            entries[path] = entries[target]
        elif member.isreg():
            entries[path] = ('regular', member.mode & 0o7777, member.uid,
                             member.gid, member.size)
        else:
            raise AssertionError(f'{member.name}: unexpected type')
    return entries, hard


def stat_all(debugfs, image, paths):
    """Returns {path: {field: value}} from one batched debugfs run."""
    script = ''.join(f'stat "/{p}"\n' for p in paths)
    out = subprocess.run([debugfs, '-f', '-', image], input=script,
                         capture_output=True, text=True, check=True).stdout
    results = {}
    blocks = re.split(r'^debugfs: stat "/(.*)"$', out, flags=re.MULTILINE)
    # blocks: [banner, path1, text1, path2, text2, ...]
    for path, text in zip(blocks[1::2], blocks[2::2]):
        match = re.search(
            r'Inode: (\d+)\s+Type: (\w+)\s+Mode:\s+(\d+).*?'
            r'User:\s+(\d+)\s+Group:\s+(\d+)(?:\s+Project:\s+\d+)?\s+Size: '
            r'(\d+)', text, re.DOTALL)
        results[path] = None if not match else {
            'inode': int(match.group(1)), 'type': match.group(2),
            'mode': int(match.group(3), 8), 'uid': int(match.group(4)),
            'gid': int(match.group(5)), 'size': int(match.group(6))}
    return results


def header_problems(entries, got):
    """Returns one line per path whose image attributes differ from the tar's.

    ENTRIES is expected_entries()'s first result, GOT stat_all()'s. A path
    missing from the image is not reported here (another test does that).
    """
    problems = []
    for path, (kind, mode, uid, gid, size) in entries.items():
        if not path or not got.get(path):
            continue
        image = got[path]
        want = (kind, mode, uid, gid)
        have = (image['type'], image['mode'] & 0o7777, image['uid'],
                image['gid'])
        if kind in ('regular', 'symlink'):
            want += (size, )
            have += (image['size'], )
        if want != have:
            problems.append(f'/{path}: tar {want}, image {have}')
    return problems


class RootfsInvariantTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.entries, cls.hard = expected_entries(FLAT)
        paths = [p for p in sorted(cls.entries) if p]
        cls.got = stat_all(DEBUGFS, IMAGE, paths)

    def test_every_path_of_the_tar_is_in_the_image(self):
        missing = [p for p in self.entries if p and not self.got.get(p)]
        self.assertEqual(missing[:10], [])
        self.assertGreater(len(self.entries), 8000)

    def test_type_mode_owner_group_and_size_match_the_headers(self):
        self.assertEqual(header_problems(self.entries, self.got)[:10], [])

    def test_hard_link_groups_are_the_same_sets_of_paths(self):
        parent = {}

        def find(p):
            while parent.setdefault(p, p) != p:
                parent[p] = parent[parent[p]]
                p = parent[p]
            return p

        for link, target in self.hard.items():
            parent[find(link)] = find(target)
        want = {}
        for path in self.hard.keys() | set(self.hard.values()):
            want.setdefault(find(path), set()).add(path)
        by_inode = {}
        for path, image in self.got.items():
            if image and image['type'] == 'regular':
                by_inode.setdefault(image['inode'], set()).add(path)
        have = [s for s in by_inode.values() if len(s) > 1]
        self.assertEqual(sorted(map(sorted, want.values())),
                         sorted(map(sorted, have)))

    def test_the_modes_that_matter_are_there(self):
        self.assertEqual(self.got['bin/mount']['mode'] & 0o7777, 0o4755)
        shadow = [p for p, e in self.entries.items() if e[3] == 42]
        self.assertTrue(shadow)


if __name__ == '__main__':
    FLAT, IMAGE, DEBUGFS = sys.argv[1:4]
    unittest.main(argv=sys.argv[:1])
