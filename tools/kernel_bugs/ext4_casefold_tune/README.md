# ext4: NULL pointer dereference after EXT4_IOC_SET_TUNE_SB_PARAM turns casefold on

A standalone reproducer (no dcfs in it) for a Linux kernel bug found by dcfs's
test suite (plan step 26.15). The bug report's draft is
`docs/plan/notes/kernel-bugs-2026-10-09.md`.

## The bug

`EXT4_IOC_SET_TUNE_SB_PARAM` (Linux 6.18+) can set the `casefold` feature on a
mounted ext4 filesystem. It writes the feature bit and the default encoding
into the superblock (`ext4_sb_setparams`, `fs/ext4/ioctl.c`), but the in-memory
`sb->s_encoding` is loaded only at mount (`ext4_encoding_init`,
`fs/ext4/super.c`), so afterwards `ext4_has_feature_casefold(sb)` is true and
`sb->s_encoding` is NULL. `chattr +F` (`ext4_ioctl_setflags`) tests only the
feature bit, so an empty directory can be made casefolded, and the next
readdir of it calls `ext4fs_dirhash()`, which passes the NULL `s_encoding` to
`utf8_casefold()` and oopses in `utf8nlookup()`.

## Run it

As root, on a machine (a VM) whose kernel you can lose:

    cc -static -o casefold_helper casefold_helper.c
    ./reproduce.sh /dev/vdb            # or a file: ./reproduce.sh disk.img
    MKFS=0 ./reproduce.sh /dev/vdb     # the device already holds an ext4 filesystem
                                       # made without the casefold feature

`DEVICE` is overwritten (`mkfs.ext4`) unless `MKFS=0`. The script mounts it,
makes a directory, calls the ioctl through `casefold_helper enable`, sets the
casefold flag with `casefold_helper mark` (what `chattr +F` does), lists the
directory, prints the kernel log from the oops, and last prints
`kernel: <first line>`. Exit status: 0 the bug did not show (including: the
kernel refuses the ioctl or the flag), 1 the oops, 2 the script could not do
its work. The helper copies the ioctl's struct from the UAPI header and tries
both sizes the struct has had (6.18.0's `pad[64]`, later `pad[68]`).

`stock tools`: mke2fs/mkfs.ext4, mount, dmesg, ls. Nothing else.

## Kernels it was run on

| Kernel | Outcome |
|---|---|
| 6.18.55-0-virt (Alpine linux-virt 6.18.55-r0, x86-64, QEMU microvm, one vCPU) | oops: `BUG: kernel NULL pointer dereference, address: 0000000000000018`, `RIP: utf8nlookup+0x14/0x240`, task `ls` |

Run in the repository by `bazel test //test/qemu:kernel_bug_ext4_casefold_tune_test`
(manual tag; the guest runs this very script with `MKFS=0` on a 64 MiB disk the
harness formatted without casefold) and, as a DISABLED_ check,
`//test/qemu:casefold_tune_oops_test`. No other kernel was run. By reading the
source: 6.17 has no such ioctl (`grep TUNE_SB fs/ext4/ioctl.c` finds nothing),
and mainline `af32da41b032` (7.3-rc6 plus the net merge of 2026-10-09) has the
same code (see the notes file for file:line).

## Guest config that matters

`CONFIG_UNICODE=y`, `CONFIG_EXT4_FS=m`, `CONFIG_FS_ENCRYPTION=y`,
`CONFIG_PREEMPT_NONE=y`, `CONFIG_SMP=y`; no KASAN. Without `CONFIG_UNICODE`
the casefold code is compiled out and there is nothing to dereference.
