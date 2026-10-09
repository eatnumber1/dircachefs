# btrfs: WARNING in btrfs_destroy_inode after a failed read of a directory inode

A standalone reproducer (no dcfs in it) for a Linux kernel warning found by
dcfs's failure-injection tests (plan steps 11.3 and 26.15). The bug report's
draft is `docs/plan/notes/kernel-bugs-2026-10-09.md`.

## The bug

`btrfs_destroy_inode()` has `WARN_ON(inode->csum_bytes)` for inodes that are
not directories. Since 6.11 (`d9891ae28b0d`, "btrfs: unify index_cnt and
csum_bytes from struct btrfs_inode") `csum_bytes` shares its storage with
`index_cnt`, which is `(u64)-1` for a directory read through the delayed-inode
fast path (`btrfs_fill_inode()`: the directory's inode item has only been
updated in memory). When `btrfs_lookup_inode()` then fails with an I/O error,
`btrfs_read_locked_inode()` calls `iget_failed()`, whose `make_bad_inode()` sets
`i_mode = S_IFREG`; the directory is destroyed as a "regular file" and the
check reads `index_cnt` as `csum_bytes`: a spurious warning on an expected
error path.

## Run it

As root, on a machine (a VM) whose kernel you can lose:

    cc -static -o handle_helper handle_helper.c
    ./reproduce.sh /dev/vdb            # or a file: ./reproduce.sh disk.img (320 MiB)
    MKFS=0 ./reproduce.sh /dev/vdb     # the device already holds a btrfs filesystem

`DEVICE` is overwritten (`mkfs.btrfs`) unless `MKFS=0`. The script puts a
dm-linear device over it, mounts it, creates 6000 files (so that the metadata
spans several leaves and a lookup after the caches are dropped needs I/O),
makes two groups of six directories, lists one group (`ls`: the relatime atime
update leaves the directory's inode item in memory only), drops the caches,
switches the dm table to dm-flakey `error_reads` (every read fails), asks for
the clean group and then the listed group by file handle
(`handle_helper open`: `open_by_handle_at(2)`), and switches back. It prints
the warning counts per group and the kernel log from the first warning, and
last `kernel: <first line>`. Exit status: 0 the bug did not show, 1 the
warning, 2 the script could not do its work.

Stock tools: mkfs.btrfs, mount, dmsetup (with dm-flakey), blockdev, dmesg, ls,
seq, xargs, losetup (for a file). The helper issues `name_to_handle_at` and
`open_by_handle_at`.

## Kernels it was run on

| Kernel | Outcome |
|---|---|
| 6.18.55-0-virt (Alpine linux-virt 6.18.55-r0, x86-64, QEMU microvm, one vCPU) | 0 warnings for the six clean directories, 6 for the six listed ones: `WARNING: CPU: 0 PID: 546 at fs/btrfs/inode.c:8047 btrfs_destroy_inode+0x224/0x290 [btrfs]`, called from `btrfs_read_locked_inode+0x14a` (`iget_failed`) from `btrfs_iget`, `btrfs_get_dentry`, `exportfs_decode_fh_raw`, `do_handle_open` |

Controls, same kernel: with `FILES=1` (a small tree, whose lookups need no I/O)
no read fails and nothing warns; with no `ls` (the clean group) the reads fail
and nothing warns. Run in the repository by
`bazel test //test/qemu:kernel_bug_btrfs_failed_inode_read_test` (manual tag).
No other kernel was run. By reading the source: before 6.11 `index_cnt` and
`csum_bytes` are separate fields, so this cannot warn; mainline `af32da41b032`
(7.3-rc6 plus the net merge of 2026-10-09) has the union, the fast path, the
`make_bad_inode()` and the check unchanged (see the notes file for file:line).

## Guest config that matters

`CONFIG_BTRFS_FS=m`, `CONFIG_DM_FLAKEY=m`, `CONFIG_BLK_DEV_DM=m`,
`CONFIG_PREEMPT_NONE=y`, `CONFIG_SMP=y`; `CONFIG_BTRFS_DEBUG` and
`CONFIG_BTRFS_ASSERT` are not set.
