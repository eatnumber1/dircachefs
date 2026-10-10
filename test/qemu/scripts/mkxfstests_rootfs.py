"""Assembles the xfstests guest's root file system, an ext4 image (step 17.1).

The generic tests need bash, GNU coreutils, util-linux's mount and a dozen
other tools, and about 100 MB of helper programs; none of it fits the
busybox initramfs every other test boots (it would sit in the guest's
tmpfs). The image is the guest's root instead: guest/init mounts it, chroots
into it and runs guest/xfstests.sh there, the way the NFS test runs in its
Debian image (qemu_test's `rootfs`). Contents:

  /                  the Alpine packages of @alpine_xfstests_tools (bash,
                     coreutils, mount, attr, acl, xfs_io, ...), their own
                     symlinks kept, at their Alpine paths
  /bin/busybox       Alpine's static busybox, with a link for every applet
                     that no Alpine package supplies (/bin/sh among them)
  /xfstests/         check, common/, tests/generic/ (with the group.list that
                     the Makefile would generate from each test's
                     _begin_fstest line), the helper programs of src/ and
                     ltp/ (built by Bazel: //third_party/xfstests) and the
                     per-filesystem lists of tests (--list)
  /etc               passwd and group with root, nobody and the users the
                     tests expect (fsgqa, 123456-fsgqa, fsgqa2), mtab
  /usr/local/bin/mount, umount
                     mount that gives every FUSE mount its dcfs options, and
                     umount that waits for the daemon's flock on its cache to go
  /usr/lib/glibc_strerror.so
                     LD_PRELOAD library: glibc's error messages for musl programs
  /mnt/test, /mnt/scratch, /cache, /tmp, /proc, /sys, /dev
                     the mount points the guest script uses

The image is made without privilege by `mke2fs -d` (Alpine's, through the
wrapper that runs it on this host), so every file belongs to the user that
ran the build: root reads and writes all of it, and the modes are what the
tests need (everything readable and traversable by the test users).

Bazel hands over each input either as the file or as a symlink to it. A
symlink that Alpine's package holds (/bin/ls -> coreutils) must stay a
symlink and one that is only Bazel's staging must be followed; the second
kind points at an absolute path, and the first kind's target is itself a
symlink, so that is how they are told apart (stage_alpine).

usage: mkxfstests_rootfs.py --out IMAGE --size 512M --mke2fs PROG
           --mke2fs-conf FILE --busybox FILE --strerror FILE
           --alpine FILE... --xfstests FILE... --helpers FILE...
           [--list FILE...]
"""

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ALPINE_MARK = '/root/'
XFSTESTS_MARK = '+http_archive+xfstests/'

# Users and groups the tests look up (xfstests' README, step 6).
PASSWD = [
    ('root', 0, 0, '/root'),
    ('nobody', 65534, 65534, '/'),
    ('daemon', 2, 2, '/'),
    ('fsgqa', 123456, 123456, '/home/fsgqa'),
    ('123456-fsgqa', 123457, 123457, '/home/123456-fsgqa'),
    ('fsgqa2', 123458, 123458, '/home/fsgqa2'),
]

MOUNT_POINTS = [
    'proc', 'sys', 'dev', 'tmp', 'mnt/test', 'mnt/scratch', 'cache', 'src',
    'root', 'home/fsgqa', 'home/123456-fsgqa', 'home/fsgqa2', 'run', 'var/tmp',
    'usr/local/bin', 'xfstests/results'
]

# A test file's group line: "<name> <group>..." from its _begin_fstest.
TEST_NAME = re.compile(r'^[0-9]{3}-?[A-Za-z0-9-]*$')
BEGIN = re.compile(r'^_begin_fstest(?:\s+(.*))?$')


class RootfsError(Exception):
    """The inputs cannot be turned into an image."""


def _after(path, mark):
    """The part of an input path after the first `mark`."""
    index = path.find(mark)
    if index < 0:
        raise RootfsError('%s: no %s in the path' % (path, mark))
    return path[index + len(mark):]


def _is_genuine_symlink(path):
    """Whether input `path` is a symlink of the package, not Bazel's staging."""
    if not os.path.islink(path):
        return False
    target = os.readlink(path)
    if not os.path.isabs(target):
        return True  # staging links are absolute
    return os.path.islink(target)


def _link_target(path):
    target = os.readlink(path)
    if os.path.isabs(target) and os.path.islink(target):
        return os.readlink(target)  # staged link to the package's link
    return target


def _put(root, rel, source, mode=None):
    """Copies the file `source` to root/rel with readable, traversable modes."""
    dest = os.path.join(root, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.lexists(dest):
        os.unlink(dest)
    shutil.copyfile(source, dest)
    if mode is None:
        mode = stat.S_IMODE(os.stat(source).st_mode)
        mode = 0o755 if mode & 0o111 else 0o644
    os.chmod(dest, mode)


def _symlink(root, rel, target):
    dest = os.path.join(root, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.lexists(dest):
        os.unlink(dest)
    os.symlink(target, dest)


def stage_alpine(root, files):
    for path in files:
        rel = _after(path, ALPINE_MARK)
        if _is_genuine_symlink(path):
            _symlink(root, rel, _link_target(path))
        else:
            _put(root, rel, path)


def stage_busybox(root, busybox):
    _put(root, 'bin/busybox', busybox, 0o755)
    listing = subprocess.run([busybox, '--list-full'], check=True,
                             capture_output=True, text=True).stdout.split()
    taken = set()
    for directory in ('bin', 'sbin', 'usr/bin', 'usr/sbin'):
        full = os.path.join(root, directory)
        if os.path.isdir(full):
            taken.update(os.listdir(full))
    for applet in sorted(listing):
        name = os.path.basename(applet)
        if name in taken or name == 'busybox':
            continue
        depth = applet.count('/')
        _symlink(root, applet, '../' * depth + 'bin/busybox')
        taken.add(name)


def group_list(tests_dir):
    """tools/mkgroupfile's output for tests/generic: one line per test."""
    lines = []
    for name in sorted(os.listdir(tests_dir)):
        if not TEST_NAME.match(name):
            continue
        with open(os.path.join(tests_dir, name), encoding='utf-8',
                  errors='replace') as f:
            for line in f:
                match = BEGIN.match(line.rstrip('\n'))
                if match:
                    lines.append(('%s %s' % (name, match.group(1) or '')).rstrip())
                    break
    return ('# QA groups control file, generated by mkxfstests_rootfs.py from\n'
            "# each test's _begin_fstest line (tools/mkgroupfile).\n\n" +
            '\n'.join(lines) + '\n')


def stage_xfstests(root, files, helpers):
    for path in files:
        rel = _after(path, XFSTESTS_MARK)
        _put(root, 'xfstests/' + rel, path)
    for path in helpers:
        name = os.path.basename(path)
        if name.startswith('helper_vfs_'):
            dest = 'xfstests/src/vfs/' + name[len('helper_vfs_'):]
        elif name.startswith('helper_'):
            dest = 'xfstests/src/' + name[len('helper_'):]
        elif name in ('fsstress', 'fsx'):
            dest = 'xfstests/ltp/' + name
        else:
            raise RootfsError('%s: not a helper program' % path)
        _put(root, dest, path, 0o755)
    generic = os.path.join(root, 'xfstests/tests/generic')
    with open(os.path.join(generic, 'group.list'), 'w', encoding='utf-8') as f:
        f.write(group_list(generic))
    os.chmod(os.path.join(root, 'xfstests/check'), 0o755)
    for directory, _, names in os.walk(os.path.join(root, 'xfstests/tests')):
        for name in names:
            # The test scripts are run as ./tests/generic/NNN by check.
            if TEST_NAME.match(name):
                os.chmod(os.path.join(directory, name), 0o755)


def stage_etc(root):
    etc = os.path.join(root, 'etc')
    os.makedirs(etc, exist_ok=True)
    with open(os.path.join(etc, 'passwd'), 'w') as f:
        for name, uid, gid, home in PASSWD:
            f.write('%s:x:%d:%d::%s:/bin/sh\n' % (name, uid, gid, home))
    with open(os.path.join(etc, 'group'), 'w') as f:
        for name, _, gid, _ in PASSWD:
            f.write('%s:x:%d:\n' % ('wheel' if name == 'root' else name, gid))
        f.write('root:x:0:\n')
    with open(os.path.join(etc, 'hostname'), 'w') as f:
        f.write('dcfs-xfstests\n')
    with open(os.path.join(etc, 'hosts'), 'w') as f:
        f.write('127.0.0.1 localhost\n::1 localhost\n')
    _symlink(root, 'etc/mtab', '/proc/self/mounts')


UMOUNT_WRAPPER = """#!/bin/sh
# Installed by mkxfstests_rootfs.py (step 17.1), ahead of /bin/umount in PATH.
#
# umount of a dcfs mount returns when the kernel has unmounted it, while the
# daemon is still shutting down: it holds an exclusive flock on its cache
# database (README.md: only one dcfs may use a database) until it exits, and
# xfstests mounts the same device again at once ("our local mount routine",
# _scratch_cycle_mount), so the new daemon finds the database in use and the
# mount fails. This waits for the lock to be free, which is the daemon's exit
# (an event, not a timer: `flock FILE true` returns when it holds the lock).
# It finds the database from the mount (the device's name, as the mount
# wrapper names it) before it unmounts. This is 15.6's interim recipe: the
# wrapper goes away when step 15.6b's umount.fuse.dcfs waits for the daemon,
# and xfstests then regression-tests that helper. A lazy unmount (-l) does not
# wait: the daemon exits only when the last user of the mount is gone.
lazy=0
db=""
for arg in "$@"; do
	case "$arg" in
	-l | --lazy | -lf | -fl) lazy=1 ;;
	-*) ;;
	*)
		target=$(readlink -f "$arg")
		src=$(awk -v t="$target" '$3 == "fuse.dcfs" && ($2 == t || $1 == t) { print $1 }' /proc/mounts | tail -n 1)
		[ -n "$src" ] && db=/cache/${src##*/}.db
		;;
	esac
done
/bin/umount "$@"
rc=$?
# Only when that was the last mount of the device: a bind mount of a file of
# the file system (generic/306) or a second mount point leaves the daemon
# serving, and the lock is held for as long.
if [ "$rc" -eq 0 ] && [ "$lazy" -eq 0 ] && [ -n "$db" ] && [ -e "$db" ] &&
	! awk -v s="$src" '$3 == "fuse.dcfs" && $1 == s { found = 1 } END { exit !found }' /proc/mounts; then
	flock "$db" true
fi
exit "$rc"
"""



MOUNT_WRAPPER = """#!/bin/sh
# Installed by mkxfstests_rootfs.py (step 17.1), ahead of /bin/mount in PATH.
#
# xfstests mounts a FUSE file system with `mount -t fuse.dcfs OPTIONS DEV DIR`
# (FSTYP=fuse, FUSE_SUBTYP=.dcfs), and a few tests mount it with the plain type
# of FSTYP (`mount -t fuse DEV DIR`, which has no helper) or with options of
# their own. dcfs needs a cache database for every mount (dcfs.cache_db, until
# plan step 15.3's default) and the tests run as other users (dcfs.allow_other)
# and make device nodes and setuid files, which libfuse mounts refuse by
# default (README.md: nosuid,nodev unless dcfs.fuse_opt=suid and =dev), so
# this adds all of them (the last two unless the test asks for nosuid), the
# database named after the device, and calls the
# helper that serves every FUSE mount of this guest.
# The glibc error messages (guest/xfstests.sh preloads them for the tests'
# tools) are not for the mount helper: the daemon is a glibc program that
# needs none, and the preloaded library is only for the musl tools.
unset LD_PRELOAD
type=""
prev=""
fuse_opts=",dcfs.fuse_opt=suid,dcfs.fuse_opt=dev"
dev=""
dir=""
remount=0
for arg in "$@"; do
	case "$prev" in
	-t) type=$arg ;;
	-o)
		case ",$arg," in *,remount,*) remount=1 ;; esac
		case ",$arg," in *,nosuid,*) fuse_opts="" ;; esac
		;;
	esac
	case "$arg" in
	-*) ;;
	*)
		if [ "$prev" != -t ] && [ "$prev" != -o ] && [ "$prev" != -O ]; then
			dev=$dir
			dir=$arg
		fi
		;;
	esac
	prev=$arg
done
case "$type" in
fuse | fuse.dcfs | dcfs)
	if [ "$remount" = 0 ] && [ -n "$dev" ]; then
		n=$#
		skip=0
		while [ "$n" -gt 0 ]; do
			a=$1
			shift
			n=$((n - 1))
			if [ "$skip" = 1 ]; then
				skip=0
			elif [ "$a" = -t ]; then
				skip=1
			else
				set -- "$@" "$a"
			fi
		done
		exec /bin/mount -t fuse.dcfs -o "dcfs.cache_db=/cache/${dev##*/}.db,dcfs.allow_other$fuse_opts" "$@"
	fi
	;;
esac
exec /bin/mount "$@"
"""


def stage_wrappers(root):
    for name, text in (('umount', UMOUNT_WRAPPER), ('mount', MOUNT_WRAPPER)):
        dest = os.path.join(root, 'usr/local/bin', name)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'w') as f:
            f.write(text)
        os.chmod(dest, 0o755)


def stage_strerror(root, library):
    """glibc's error messages for the musl programs, preloaded (shim/glibc_strerror.c).

    The library lists libm.so.6 as needed (the toolchain adds -lm); musl has
    its math functions in libc, so that name is a link to musl.
    """
    _put(root, 'usr/lib/glibc_strerror.so', library, 0o755)
    _symlink(root, 'lib/libm.so.6', 'ld-musl-x86_64.so.1')


def stage_lists(root, files):
    for path in files:
        _put(root, 'xfstests/dcfs/' + os.path.basename(path), path, 0o644)


def build(args):
    env = dict(os.environ, MKE2FS_CONFIG=os.path.abspath(args.mke2fs_conf),
               SOURCE_DATE_EPOCH='1791275242',
               E2FSPROGS_FAKE_TIME='1791275242')
    with tempfile.TemporaryDirectory() as work:
        root = os.path.join(work, 'root')
        os.mkdir(root)
        stage_alpine(root, args.alpine)
        stage_busybox(root, args.busybox)
        stage_xfstests(root, args.xfstests, args.helpers)
        stage_etc(root)
        stage_lists(root, args.list)
        stage_wrappers(root)
        stage_strerror(root, args.strerror)
        for rel in MOUNT_POINTS:
            os.makedirs(os.path.join(root, rel), exist_ok=True)
        for directory, dirs, _ in os.walk(root):
            for d in dirs:
                full = os.path.join(directory, d)
                if not os.path.islink(full):
                    os.chmod(full, 0o755)
        os.chmod(os.path.join(root, 'tmp'), 0o1777)
        image = os.path.join(work, 'rootfs.ext4')
        with open(image, 'wb') as f:
            f.truncate(_size(args.size))
        subprocess.run([
            args.mke2fs, '-q', '-t', 'ext4', '-L', 'dcfs-xfstests', '-U',
            '6dcf5000-0000-4000-8000-000000000003', '-E',
            'hash_seed=6dcf5000-0000-4000-8000-000000000004,root_owner=0:0',
            '-O', '^has_journal', '-d', root, '-F', image
        ], env=env, check=True)
        _sparse_copy(image, args.out)


def _sparse_copy(source, dest):
    """Copies `source` to `dest`, leaving a hole for every all-zero block.

    The image is mostly free space; a plain copy would write all of it, and
    run-qemu.sh copies the image again for every test.
    """
    block = 1 << 16
    zero = bytes(block)
    with open(source, 'rb') as src, open(dest, 'wb') as dst:
        while True:
            chunk = src.read(block)
            if not chunk:
                break
            if chunk == zero[:len(chunk)]:
                dst.seek(len(chunk), os.SEEK_CUR)
            else:
                dst.write(chunk)
        dst.truncate(src.tell())


def _size(text):
    units = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}
    if text[-1] in units:
        return int(text[:-1]) * units[text[-1]]
    return int(text)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--out', required=True)
    parser.add_argument('--size', required=True)
    parser.add_argument('--mke2fs', required=True)
    parser.add_argument('--mke2fs-conf', required=True)
    parser.add_argument('--busybox', required=True)
    parser.add_argument('--strerror', required=True)
    parser.add_argument('--alpine', nargs='+', required=True)
    parser.add_argument('--xfstests', nargs='+', required=True)
    parser.add_argument('--helpers', nargs='+', required=True)
    parser.add_argument('--list', nargs='*', default=[])
    args = parser.parse_args(argv)
    try:
        build(args)
    except (RootfsError, subprocess.CalledProcessError, OSError) as e:
        print('mkxfstests_rootfs.py: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
