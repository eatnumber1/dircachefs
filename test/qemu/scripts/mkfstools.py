"""Builds the mkfs part of a RAM-disk guest's initramfs (step 26.17).

A test whose disks are in the guest's RAM (qemu_test's `ram_disks`) has them
formatted in the guest, by the same Alpine tools run-qemu.sh formats a
virtio disk with: this script writes a gzip'd newc cpio archive holding mke2fs,
mkfs.xfs and mkfs.btrfs, the musl loader and the shared libraries they need
(each stored once under its real name, the sonames as symlinks), and the
checked-in mke2fs profile, all under /opt/fstools. The tools are linked
against musl, so guest/init runs them through that loader with
--library-path, as the wrappers in @alpine_fstools do on the host. The kernel
unpacks several concatenated archives into one initramfs, so run-qemu.sh
appends this one to the shared base archive, as it does the modules'.

usage: mkfstools.py --root DIR --mke2fs-conf FILE --out FILE.cpio.gz

DIR is the unpacked @alpine_fstools package (it holds sbin/, lib/, usr/lib/).
"""

import argparse
import gzip
import os
import re
import sys

import mkmodules

PREFIX = 'opt/fstools'
TOOLS = ('mke2fs', 'mkfs.xfs', 'mkfs.btrfs')
LIBRARY_DIRS = ('lib', 'usr/lib')
SONAME = re.compile(r'\.so\.[0-9]+$')


class FstoolsError(Exception):
    """A tool the guest formats with is missing from the package."""


def build(root, mke2fs_conf):
    """Returns the gzip'd cpio archive of the tools, libraries and profile."""
    entries = []
    made = set()

    def mkdir(path):
        if path and path not in made:
            mkdir(os.path.dirname(path))
            made.add(path)
            entries.append((path, 0o040755, b''))

    def add(path, mode, data):
        mkdir(os.path.dirname(path))
        entries.append((path, mode, data))

    def read(path):
        with open(path, 'rb') as f:
            return f.read()

    for tool in TOOLS:
        path = os.path.join(root, 'sbin', tool)
        if not os.path.isfile(path):
            raise FstoolsError(f'{path} is not in the package')
        add(f'{PREFIX}/sbin/{tool}', 0o100755, read(path))
    for directory in LIBRARY_DIRS:
        stored = set()
        for name in sorted(os.listdir(os.path.join(root, directory))):
            if not SONAME.search(name):
                continue
            path = os.path.join(root, directory, name)
            real = os.path.basename(os.path.realpath(path))
            if real not in stored:
                stored.add(real)
                add(f'{PREFIX}/{directory}/{real}', 0o100755, read(path))
            if name != real:
                add(f'{PREFIX}/{directory}/{name}', 0o120777, real.encode())
    add(f'{PREFIX}/etc/mke2fs.conf', 0o100644, read(mke2fs_conf))
    return gzip.compress(mkmodules.newc(entries), compresslevel=1, mtime=0)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--root', required=True)
    parser.add_argument('--mke2fs-conf', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args(argv)
    try:
        archive = build(args.root, args.mke2fs_conf)
    except FstoolsError as e:
        print(f'mkfstools.py: {e}', file=sys.stderr)
        return 1
    with open(args.out, 'wb') as f:
        f.write(archive)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
