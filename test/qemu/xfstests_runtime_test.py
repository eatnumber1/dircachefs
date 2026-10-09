"""Self-check of the xfstests guest's runtime (step 17.1).

The Alpine packages of @alpine_xfstests_tools are listed by name, not
resolved with their dependencies (xfsprogs-extra would pull in python3), so a
new build of a package inside the branch that needs one more library would
only show up as a program that does not start in the guest. This test reads
the dynamic section of every ELF file in the tree and fails if a needed
library is not in it, and checks that each program xfstests' common/config
cannot do without (and the ones the generic tests call most) is there.

usage: xfstests_runtime_test.py <resolved.json of @alpine_xfstests_tools>
"""

import json
import os
import struct
import sys

# Programs common/config makes fatal when missing, and the tools the generic
# tests call (Alpine paths, relative to the tree).
REQUIRED = [
    'bin/bash', 'bin/mount', 'bin/umount', 'usr/bin/perl', 'usr/bin/awk',
    'bin/sed', 'usr/bin/bc', 'bin/df', 'usr/sbin/xfs_io', 'sbin/mkfs',
    'usr/bin/getfattr', 'usr/bin/setfattr', 'usr/bin/getfacl',
    'usr/bin/setfacl', 'bin/chattr', 'bin/lsattr', 'bin/findmnt',
    'usr/bin/find', 'usr/bin/xargs', 'usr/bin/diff', 'bin/grep',
    'usr/bin/md5sum', 'bin/stat', 'usr/bin/timeout', 'usr/bin/fio',
    'sbin/mkfs.ext4', 'sbin/mkfs.xfs', 'sbin/mkfs.btrfs',
    'lib/ld-musl-x86_64.so.1',
]


def needed(path):
    """The DT_NEEDED names of the ELF64 file `path`, [] if it is not one."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2:
        return []
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3a)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
                for i in range(shnum)]
    names = []
    for (_, kind, _, _, offset, size, link, _, _, entsize) in sections:
        if kind != 6:  # SHT_DYNAMIC
            continue
        strtab = sections[link]
        for i in range(size // entsize):
            tag, value = struct.unpack_from('<qQ', data, offset + i * entsize)
            if tag == 1:  # DT_NEEDED
                start = strtab[4] + value
                names.append(data[start:data.index(b'\0', start)].decode())
    return names


def main(argv):
    root = os.path.join(os.path.dirname(os.path.realpath(argv[1])), 'root')
    present = set()
    elves = []
    for directory, _, files in os.walk(root):
        for name in files:
            present.add(name)
            elves.append(os.path.join(directory, name))
    problems = []
    for path in sorted(elves):
        if os.path.islink(path) and not os.path.exists(path):
            problems.append('dangling link: ' + os.path.relpath(path, root))
            continue
        for lib in needed(path):
            if lib not in present:
                problems.append('%s needs %s, which is not in the tree' %
                                (os.path.relpath(path, root), lib))
    for rel in REQUIRED:
        if not os.path.exists(os.path.join(root, rel)):
            problems.append('missing: ' + rel)
    resolved = json.load(open(argv[1]))
    print('%d packages, %d files' % (len(resolved['packages']), len(elves)))
    for problem in problems:
        print('FAIL:', problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
