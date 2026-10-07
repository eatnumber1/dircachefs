"""Builds the kernel-module part of a QEMU guest's initramfs (step 24.2).

The test kernel is Alpine's linux-virt, whose drivers are mostly modules.
Each test declares the modules it needs; this script writes a gzip'd newc
cpio archive holding those modules, everything they depend on, and
/etc/dcfs-modules, the list guest/init loads with insmod, in load order. The
kernel unpacks several concatenated archives into one initramfs, so
run-qemu.sh appends this one to the shared base archive.

Modules are decompressed here (the guest's busybox insmod reads plain .ko
files, and the guest does not pay for gunzip at every boot). The order and
the closure come from the kernel's own modules.dep, plus modules.softdep
(btrfs wants its checksum algorithms loaded before it mounts; nothing in
the guest runs modprobe to fetch them on demand). A requested name that the
kernel has built in is skipped, so an Alpine config change inside a branch
that moves a driver into the kernel never fails a test.

usage: mkmodules.py --root DIR --out FILE.cpio.gz [MODULE...]

DIR is the unpacked linux-virt package (it holds lib/modules/<release>/).
"""

import argparse
import gzip
import os
import re
import sys


class ModulesError(Exception):
    """A requested module does not exist in the kernel package."""


def _name(path):
    """Returns the module name of a .ko or .ko.gz path."""
    base = os.path.basename(path)
    return re.sub(r'\.ko(\.gz)?$', '', base).replace('-', '_')


def _words(line):
    return line.split()


class ModuleTree:
    """The module index files of one kernel release."""

    def __init__(self, directory):
        self.directory = directory
        self.release = os.path.basename(directory)
        self.deps = {}  # path -> [dependency paths]
        for line in self._lines('modules.dep'):
            path, _, rest = line.partition(':')
            self.deps[path] = _words(rest)
        self.by_name = {_name(path): path for path in self.deps}
        self.builtin = {_name(line) for line in self._lines('modules.builtin')}
        self.aliases = {}
        for line in self._lines('modules.alias'):
            words = _words(line)
            if len(words) == 3 and words[0] == 'alias':
                self.aliases.setdefault(words[1], []).append(words[2])
        self.softdeps = {}  # module name -> [names it wants loaded first]
        for line in self._lines('modules.softdep'):
            words = _words(line)
            if len(words) < 2 or words[0] != 'softdep':
                continue
            wanted = []
            mode = None
            for word in words[2:]:
                if word in ('pre:', 'post:'):
                    mode = word
                elif mode == 'pre:':
                    wanted.append(word)
            self.softdeps.setdefault(words[1].replace('-', '_'),
                                     []).extend(wanted)

    def _lines(self, name):
        path = os.path.join(self.directory, name)
        if not os.path.exists(path):
            return []
        with open(path, encoding='utf-8') as f:
            return [line.strip() for line in f if line.strip()]

    def _resolve(self, name):
        """Returns the module paths that provide `name` (a name or alias)."""
        key = name.replace('-', '_')
        if key in self.by_name:
            return [self.by_name[key]]
        for alias in (name, 'crypto-' + name):
            if alias in self.aliases:
                return [
                    self.by_name[m.replace('-', '_')]
                    for m in self.aliases[alias]
                    if m.replace('-', '_') in self.by_name
                ]
        return []

    def _is_builtin(self, name):
        """Whether `name`, a module name or an alias, is built into the kernel.
        """
        key = name.replace('-', '_')
        if key in self.builtin:
            return True
        for alias in (name, 'crypto-' + name):
            targets = self.aliases.get(alias)
            if targets and all(
                    t.replace('-', '_') in self.builtin for t in targets):
                return True
        return False

    def load_order(self, requested):
        """Returns module paths in the order insmod must load them.

        Raises:
            ModulesError: a requested module is neither a module nor built in.
        """
        order = []
        seen = set()

        def visit(path):
            if path in seen:
                return
            seen.add(path)
            for name in self.softdeps.get(_name(path), []):
                for soft in self._resolve(name):
                    visit(soft)
            for dep in self.deps[path]:
                visit(dep)
            order.append(path)

        for name in requested:
            paths = self._resolve(name)
            if not paths:
                if self._is_builtin(name):
                    continue
                raise ModulesError(
                    f'module {name!r} is neither a module nor built in '
                    f'in kernel {self.release}')
            for path in paths:
                visit(path)
        return order

    def read(self, path):
        """Returns the decompressed bytes of a module file."""
        with open(os.path.join(self.directory, path), 'rb') as f:
            data = f.read()
        return gzip.decompress(data) if path.endswith('.gz') else data


def find_tree(root):
    """Returns the ModuleTree of the one kernel release under root."""
    base = os.path.join(root, 'lib', 'modules')
    releases = sorted(os.listdir(base)) if os.path.isdir(base) else []
    if len(releases) != 1:
        raise ModulesError(
            f'{base} holds {len(releases)} kernel releases, want exactly 1')
    return ModuleTree(os.path.join(base, releases[0]))


def _newc(entries):
    """Returns a newc cpio archive of (path, mode, data) entries."""
    out = bytearray()

    def pad():
        while len(out) % 4:
            out.append(0)

    for inode, (path, mode, data) in enumerate(
            entries + [('TRAILER!!!', 0, b'')], start=1):
        name = path.encode() + b'\0'
        fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0]
        out += b'070701' + ''.join(f'{f:08X}' for f in fields).encode()
        out += name
        pad()
        out += data
        pad()
    return bytes(out)


def build(root, requested):
    """Returns the gzip'd cpio archive for the requested modules."""
    tree = find_tree(root)
    order = tree.load_order(requested)
    entries = []
    made = set()

    def mkdir(path):
        if path and path not in made:
            mkdir(os.path.dirname(path))
            made.add(path)
            entries.append((path, 0o040755, b''))

    destinations = []
    for path in order:
        dest = f'lib/modules/{tree.release}/' + re.sub(r'\.gz$', '', path)
        mkdir(os.path.dirname(dest))
        entries.append((dest, 0o100644, tree.read(path)))
        destinations.append('/' + dest)
    mkdir('etc')
    listing = ''.join(f'{d}\n' for d in destinations).encode()
    entries.append(('etc/dcfs-modules', 0o100644, listing))
    return gzip.compress(_newc(entries), compresslevel=1, mtime=0)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--root', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('modules', nargs='*')
    args = parser.parse_args(argv)
    try:
        archive = build(args.root, args.modules)
    except ModulesError as e:
        print(f'mkmodules.py: {e}', file=sys.stderr)
        return 1
    with open(args.out, 'wb') as f:
        f.write(archive)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
