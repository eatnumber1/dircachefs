"""Assembles the NFS test's Debian rootfs ext4 image (step 24.3).

Input is @debian//:flat, the tar rules_distroless's `flatten` rule makes of
every resolved package's files. The image is made without privilege:

  1. The tar is unpacked into a scratch directory, entry by entry and in
     order, resolving every parent directory through the symlinks the tar
     itself holds (usr-merge: bin -> usr/bin) lexically and inside the
     scratch directory, never through the host's filesystem, so no entry
     can reach outside it. Directories a tar lacks are created (the tar
     does not hold ./var/lib/dpkg).
  2. `mke2fs -d <directory>` makes the image. Alpine's mke2fs has no tar
     support (it is built without libarchive), and the directory carries the
     unpacking user's ownership and temporary modes.
  3. `debugfs -w -f` then sets owner, group and mode of every inode to what
     the tar says (and the five paths no package ships, below), so the image
     has real root:root ownership, the setuid bit of /bin/mount and the
     shadow group of the few files that have one, none of which a
     non-root user can create in a directory.

The image is reproducible: fixed UUID and hash seed, the packages' own
mtimes, and e2fsprogs's clock (SOURCE_DATE_EPOCH, which clamps every inode
time later than it) set to --source-date-epoch, which must be later than
every package's mtime: only times made here (directories the tar lacks, the
fixed-up paths, the superblock) are clamped. A package mtime later than the
epoch is an error, because it would be clamped silently: the epoch moves
with the snapshot pin in MODULE.bazel.

usage: mkrootfs.py --flat TAR --out IMAGE --size 768M --mke2fs PROG
           --debugfs PROG --mke2fs-conf FILE --source-date-epoch SECONDS
"""

import argparse
import os
import stat
import subprocess
import sys
import tarfile
import tempfile

_MAX_LINKS = 40

# Paths no package's data.tar ships; the .deb maintainer scripts that would
# make them never run here (see third_party/debian/README.md, "Image
# assembly"): (path, kind, argument).
FIXUPS = (
    # rpc.mountd's crossmnt handling dereferences /etc/mtab unconditionally.
    ('etc/mtab', 'symlink', '/proc/self/mounts'),
    # guest/init chroots in and runs /bin/sh.
    ('bin/sh', 'symlink', '/bin/bash'),
    ('usr/bin/awk', 'symlink', 'mawk'),
    # exportfs refuses to start without it.
    ('etc/exports', 'file', b''),
    # guest/init copies dcfs, fhtest and testutil here.
    ('usr/local/bin', 'dir', None),
)


class RootfsError(Exception):
    """The tar cannot be turned into an image."""


class Tree:
    """A directory tree being filled from tar entries, and its fixups."""

    def __init__(self, root):
        self.root = root
        # image path -> (mode with type bits, uid, gid)
        self.attrs = {}
        self.dirs = {}  # directory path -> mtime, for set_directory_times

    def _real(self, parts):
        return os.path.join(self.root, *parts)

    def _resolve(self, parts):
        """Resolves a directory path through the tree's own symlinks.

        Components that do not exist are created as directories, as tar
        does. Returns the components of the real path (no symlinks).
        """
        done = []
        todo = list(parts)
        links = 0
        while todo:
            name = todo.pop(0)
            if name in ('', '.'):
                continue
            if name == '..':
                if done:
                    done.pop()
                continue
            path = self._real(done + [name])
            if os.path.islink(path):
                links += 1
                if links > _MAX_LINKS:
                    raise RootfsError(f'too many symlinks resolving {parts}')
                target = os.readlink(path)
                if target.startswith('/'):
                    done = []
                todo = target.split('/') + todo
                continue
            if not os.path.lexists(path):
                os.mkdir(path)
                self.attrs['/'.join(done + [name])] = (stat.S_IFDIR | 0o755, 0,
                                                       0)
            elif not os.path.isdir(path):
                raise RootfsError(f'{"/".join(done + [name])} is not a '
                                  'directory')
            done.append(name)
        return done

    def _place(self, name):
        """Returns (parent components, base name) of an entry's path."""
        parts = [p for p in name.split('/') if p not in ('', '.')]
        if not parts:
            return None, None
        return self._resolve(parts[:-1]), parts[-1]

    def set_directory_times(self):
        """Gives every directory its tar mtime, after its children changed it.

        Deepest first, so a child directory's change does not touch a parent
        already done.
        """
        for rel in sorted(self.dirs, key=lambda r: r.count('/'), reverse=True):
            os.utime(self._real(rel.split('/')), (self.dirs[rel], ) * 2)

    def add(self, name, kind, mode, uid, gid, mtime, data=None, target=None):
        """Adds one entry: kind is dir, file, symlink or hardlink."""
        if kind == 'dir':
            # A directory entry for a symlink to a directory is that
            # directory (as in tar), so resolve the whole path.
            parts = [p for p in name.split('/') if p not in ('', '.')]
            done = self._resolve(parts)
            rel = '/'.join(done)
            os.chmod(self._real(done), 0o755)
            self.attrs[rel] = (stat.S_IFDIR | mode, uid, gid)
            if done:
                self.dirs[rel] = mtime
            return
        parent, base = self._place(name)
        if base is None:
            raise RootfsError(f'{name}: a {kind} entry for the root')
        if base == '..':
            raise RootfsError(f'{name}: ends in a .. component')
        rel = '/'.join(parent + [base])
        path = self._real(parent + [base])
        if os.path.isdir(path) and not os.path.islink(path):
            raise RootfsError(f'{name}: a {kind} over a directory')
        if os.path.lexists(path):
            os.unlink(path)
        if kind == 'file':
            with open(path, 'wb') as f:
                f.write(data)
            os.chmod(path, 0o644)
            self.attrs[rel] = (stat.S_IFREG | mode, uid, gid)
        elif kind == 'symlink':
            os.symlink(target, path)
            self.attrs[rel] = (stat.S_IFLNK | 0o777, uid, gid)
        elif kind == 'hardlink':
            tparent, tbase = self._place(target)
            source = self._real(tparent + [tbase])
            # lexists: the target may be a symlink (dangling, or with an
            # absolute target that means the host's, never ours); the link is
            # to the symlink itself.
            if not os.path.lexists(source):
                raise RootfsError(f'hard link {name} to missing {target}')
            os.link(source, path, follow_symlinks=False)
            self.attrs[rel] = self.attrs['/'.join(tparent + [tbase])]
        else:
            raise RootfsError(f'{name}: unsupported entry kind {kind}')
        os.utime(path, (mtime, mtime), follow_symlinks=False)


def unpack(tar_path, tree, epoch):
    """Fills the tree from the tar; returns the number of entries.

    Raises:
        RootfsError: an entry the image cannot hold, or a package mtime later
            than epoch (e2fsprogs would clamp it).
    """
    count = 0
    with tarfile.open(tar_path) as archive:
        for member in archive:
            count += 1
            if member.mtime > epoch:
                raise RootfsError(
                    f'{member.name}: mtime {member.mtime} is later than '
                    f'--source-date-epoch {epoch}, which e2fsprogs would '
                    'clamp it to; move the epoch with the snapshot pin')
            common = (member.mode & 0o7777, member.uid, member.gid,
                      member.mtime)
            if member.isdir():
                tree.add(member.name, 'dir', *common)
            elif member.isreg():
                tree.add(member.name, 'file', *common,
                         data=archive.extractfile(member).read())
            elif member.issym():
                tree.add(member.name, 'symlink', *common,
                         target=member.linkname)
            elif member.islnk():
                tree.add(member.name, 'hardlink', *common,
                         target=member.linkname)
            else:
                raise RootfsError(f'{member.name}: unsupported tar entry type '
                                  f'{member.type!r}')
    return count


def add_fixups(tree):
    for path, kind, argument in FIXUPS:
        if kind == 'symlink':
            tree.add(path, 'symlink', 0o777, 0, 0, 0, target=argument)
        elif kind == 'file':
            tree.add(path, 'file', 0o644, 0, 0, 0, data=argument)
        else:
            tree.add(path, 'dir', 0o755, 0, 0, 0)


def _quote(path):
    try:
        path.encode('utf-8')
    except UnicodeEncodeError as e:
        raise RootfsError(f'{path!r} is not valid UTF-8; debugfs is scripted '
                          'in text and the image needs UTF-8 names') from e
    if '"' in path or '\n' in path:
        raise RootfsError(f'cannot script the path {path!r} for debugfs')
    return '"/' + path + '"'


def ownership_script(tree):
    """Returns the debugfs commands that set mode, uid and gid everywhere."""
    lines = []
    for rel in sorted(tree.attrs):
        mode, uid, gid = tree.attrs[rel]
        path = _quote(rel) if rel else '/'
        lines.append(f'sif {path} mode 0{mode:o}')
        lines.append(f'sif {path} uid {uid}')
        lines.append(f'sif {path} gid {gid}')
    return '\n'.join(lines) + '\n'


def run_debugfs(debugfs, image, script):
    """Runs the script; any output other than the echoed commands is an error.

    debugfs exits 0 after a failed command, so its output is the only sign
    (its version banner and the echoed commands start with 'debugfs').
    """
    result = subprocess.run([debugfs, '-w', '-f', '-', image],
                            input=script, capture_output=True, text=True,
                            check=False)
    problems = [
        line for line in (result.stdout + result.stderr).splitlines()
        if line.strip() and not line.startswith('debugfs')
    ]
    if result.returncode != 0 or problems:
        raise RootfsError('debugfs failed:\n' + '\n'.join(problems[:20]))


def build(args):
    env = dict(os.environ,
               MKE2FS_CONFIG=os.path.abspath(args.mke2fs_conf),
               SOURCE_DATE_EPOCH=str(args.source_date_epoch),
               E2FSPROGS_FAKE_TIME=str(args.source_date_epoch))
    with tempfile.TemporaryDirectory() as work:
        root = os.path.join(work, 'root')
        os.mkdir(root)
        tree = Tree(root)
        unpack(args.flat, tree, args.source_date_epoch)
        add_fixups(tree)
        # After the fixups: they add entries to directories the tar dated.
        tree.set_directory_times()
        image = os.path.join(work, 'rootfs.ext4')
        with open(image, 'wb') as f:
            f.truncate(_size(args.size))
        subprocess.run([
            args.mke2fs, '-q', '-t', 'ext4', '-L', 'dcfs-rootfs', '-U',
            '6dcf5000-0000-4000-8000-000000000001', '-E',
            'hash_seed=6dcf5000-0000-4000-8000-000000000002', '-d', root,
            '-F', image
        ], env=env, check=True)
        if not args.skip_ownership:
            run_debugfs(args.debugfs, image, ownership_script(tree))
        # A copy, not a rename: the output is on another filesystem in Bazel.
        with open(image, 'rb') as src, open(args.out, 'wb') as dst:
            while True:
                chunk = src.read(1 << 24)
                if not chunk:
                    break
                dst.write(chunk)


def _size(text):
    units = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}
    if text[-1] in units:
        return int(text[:-1]) * units[text[-1]]
    return int(text)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--flat', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--size', required=True)
    parser.add_argument('--mke2fs', required=True)
    parser.add_argument('--debugfs', required=True)
    parser.add_argument('--mke2fs-conf', required=True)
    parser.add_argument('--source-date-epoch', required=True, type=int,
                        help='e2fsprogs clock; later than every package mtime')
    parser.add_argument('--skip-ownership', action='store_true',
                        help='for the test of the test: leave the ownership '
                        'step out')
    args = parser.parse_args(argv)
    try:
        build(args)
    except (RootfsError, subprocess.CalledProcessError) as e:
        print(f'mkrootfs.py: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
