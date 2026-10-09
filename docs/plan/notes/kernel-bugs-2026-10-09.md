# Kernel bug reports: ext4 casefold tune oops and btrfs failed inode read (step 26.15, 2026-10-09)

Prepared for russ to send; nothing here was sent or posted. Each bug has a
dcfs-free reproducer under `tools/kernel_bugs/<name>/` (script, a small C
helper, README, BUILD), run in the pinned test kernel by a manual
`//test/qemu:kernel_bug_<name>_test`, which reuses the repository's DISABLED_
mechanism (`guest/kernel_bug_repro.sh`: the check "would FAIL (kernel: ...)",
and run-qemu.sh tolerates exactly that kernel failure).

| | ext4 | btrfs |
|---|---|---|
| Reproducer | `tools/kernel_bugs/ext4_casefold_tune/reproduce.sh` | `tools/kernel_bugs/btrfs_failed_inode_read/reproduce.sh` |
| Guest test | `//test/qemu:kernel_bug_ext4_casefold_tune_test` | `//test/qemu:kernel_bug_btrfs_failed_inode_read_test` |
| Pinned kernel (6.18.55-0-virt) | oops, every run | WARNING, every run (6 of 6 directories) |
| Mainline `af32da41b032` (7.3-rc6 + net merge, 2026-10-09) | bug present (source) | bug present (source) |
| Existing report | none found | syzbot, 2024-09-12, no reply, no fix |
| Real bug? | yes: NULL dereference, any root user | spurious WARN_ON on an error path (a real accounting-check bug, harmless in effect) |
| Draft | new report | reply to the syzbot report |

## How the archives were searched, and what that cannot show

lore.kernel.org answers the fetch tool with HTTP 403 (its Anubis bot check),
which was not worked around. The searches used the `ratatoskr.run` mirror of
linux-ext4 and linux-btrfs (its search matches subject and sender only, not
message bodies), the lkml.iu.edu / lkml.rescloud.iu.edu mirrors of LKML (read
through a general web search) and the syzbot dashboard link in a report. A
report whose subject does not contain the words below, or that lives in a
message body, would be missed. Before sending either mail, russ should run the
two searches on lore itself (`lore.kernel.org/linux-ext4/?q=casefold+tune`,
`lore.kernel.org/linux-btrfs/?q=btrfs_destroy_inode+csum_bytes`; `q=` also
searches bodies there).

Searched (ext4): `casefold` (100 results, the newest 2026-09-29), `tune` (100),
`tune_sb` (19), `s_encoding` (6), and web searches for the ioctl name with
"casefold", "s_encoding", "NULL pointer", "ext4fs_dirhash". Nothing reports
this oops or this missing step. The related threads: the ioctl's introduction
(Ted Ts'o, "ext4: implemet new ioctls to set and get superblock parameters",
2025-09-16, `04a91570ac67`), "ext4: fix ext4_tune_sb_params padding" (Arnd
Bergmann, 2025-12-04, `cd16edba1c6a`, which changed the struct's size and so
the ioctl numbers), "ext4: check if mount_opts is NUL-terminated in
ext4_ioctl_set_tune_sb()" (2025-11-01) and "ext4: only update tune mount
options when requested" (guzebing, 2026-08-10, applied by Ted 2026-10-09 as
`981eec6c300b`: it touches `ext4_sb_setparams` and does not change this). The
invariant the oops breaks ("the casefold feature implies `sb->s_encoding`")
is the one "[PATCH 2/3] ext4: remove redundant checks of s_encoding" (2023-08-14,
https://ratatoskr.run/linux-ext4/2023/08/258305) relied on to delete the NULL
checks.

Searched (btrfs): `destroy_inode` (55 results back to 2013), `index_cnt` (8),
`iget_failed` (2), `bad inode` (1), and web searches for
"btrfs_destroy_inode csum_bytes iget_failed make_bad_inode index_cnt".

## Common: how the reproducers were run

Every run was in the QEMU test guest (the repository's harness, one vCPU, 256
MiB, microvm) on the pinned kernel only: Alpine `linux-virt` 6.18.55-r0
(`6.18.55-0-virt`), built from `linux-lts`, x86-64, `CONFIG_PREEMPT_NONE=y`,
`CONFIG_SMP=y`, `CONFIG_UNICODE=y`, `CONFIG_EXT4_FS=m`, `CONFIG_BTRFS_FS=m`,
`CONFIG_DM_FLAKEY=m`, `CONFIG_BTRFS_DEBUG` and `CONFIG_BTRFS_ASSERT` not set,
no KASAN. The line numbers match vanilla v6.18.55 (checked for btrfs against the
disassembly of the shipped `btrfs.ko`: the warning at `btrfs_destroy_inode+0x224`
is the third check of the `!S_ISDIR` block, at offset 0xe0 of `struct
btrfs_inode`, the field `btrfs_alloc_inode` zeroes after `ro_flags`, i.e.
`csum_bytes` / `index_cnt`). The host kernel (6.8) was not used, and no other
kernel version was run (each reproducer: 3 of 3 runs reproduced with `--runs_per_test=3`; btrfs: 6 warnings in each): the reproducers should not be run on a machine that
cannot lose its kernel.

## Bug 1: ext4, EXT4_IOC_SET_TUNE_SB_PARAM turns casefold on without loading the encoding

### Kernel log (reproducer, pinned kernel)

Serial log of `//test/qemu:kernel_bug_ext4_casefold_tune_test`:

    kernel_bug_repro.sh: kernel 6.18.55-0-virt, bug 'ext4_casefold_tune'
    + casefold_helper enable /tmp/tmp.XXXXXX   (EXT4_IOC_SET_TUNE_SB_PARAM, casefold on)
    + casefold_helper mark /tmp/tmp.XXXXXX/d   (chattr +F /tmp/tmp.XXXXXX/d)
    + ls /tmp/tmp.XXXXXX/d   (readdir of the casefold directory)
    ...
    kernel: BUG: kernel NULL pointer dereference, address: 0000000000000018
    TEST DISABLED_casefold-tune-online-oops DISABLED (kernel: EXT4_IOC_SET_TUNE_SB_PARAM enables the casefold feature without loading sb->s_encoding, ...)
      would FAIL (kernel: BUG: kernel NULL pointer dereference, address: 0000000000000018)

The oops:

    BUG: kernel NULL pointer dereference, address: 0000000000000018
    #PF: supervisor read access in kernel mode
    #PF: error_code(0x0000) - not-present page
    PGD 0 P4D 0 
    Oops: Oops: 0000 [#1] SMP PTI
    CPU: 0 UID: 0 PID: 452 Comm: ls Not tainted 6.18.55-0-virt #1-Alpine PREEMPT(none) 
    RIP: 0010:utf8nlookup+0x14/0x240
    Code: cc cc cc 0f 1f 00 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 4d 85 c0 0f 84 e5 01 00 00 48 89 f8 89 f6 48 89 d7 49 89 ca <48> 8b 50 18
    RSP: 0018:ffffb2ddc01a3a38 EFLAGS: 00010202
    RAX: 0000000000000000 RBX: ffffb2ddc01a3a58 RCX: ffff8fe644af8008
    RDX: ffffb2ddc01a3a94 RSI: 0000000000000001 RDI: ffffb2ddc01a3a94
    RBP: ffffb2ddc01a3a94 R08: 0000000000000001 R09: 0000000000000000
    R10: ffff8fe644af8008 R11: 0000000000000000 R12: 0000000000001000
    R13: ffff8fe644424000 R14: ffff8fe6416220d0 R15: ffff8fe644af8ff8
    FS:  00007fdcae639738(0000) GS:ffff8fe695757000(0000) knlGS:0000000000000000
    CS:  0010 DS: 0000 ES: 0000 CR0: 0000000080050033
    CR2: 0000000000000018 CR3: 000000000f9a4001 CR4: 0000000000370eb0
    Call Trace:
     <TASK>
     utf8byte+0x69/0x410
     utf8_casefold+0x6f/0xc0
     ext4fs_dirhash+0xe1/0x130 [ext4]
     htree_dirblock_to_tree+0x11b/0x390 [ext4]
     ext4_htree_fill_tree+0xef/0x3e0 [ext4]
     ext4_readdir+0x8e6/0xc20 [ext4]
     iterate_dir+0xb9/0x2a0
     __x64_sys_getdents64+0x75/0x110
     ? __pfx_filldir64+0x10/0x10
     do_syscall_64+0x88/0xfa0
     ? count_memcg_events+0xf4/0x1c0
     ? handle_mm_fault+0x159/0x260
     ? do_user_addr_fault+0x224/0x6c0
     ? clear_bhb_loop+0x40/0x90
     ? clear_bhb_loop+0x40/0x90
     ? clear_bhb_loop+0x40/0x90
     entry_SYSCALL_64_after_hwframe+0x76/0x7e
     </TASK>
    Modules linked in: virtio_blk fuse ext4 jbd2 mbcache crc16
    CR2: 0000000000000018
    ---[ end trace 0000000000000000 ]---

### Code path, 6.18.55 (read from vanilla v6.18.55; mainline `af32da41b032` in brackets)

1. `fs/ext4/ioctl.c:1388` [1390] `ext4_ioctl_set_tune_sb()`: for
   `set_feature_incompat_mask & EXT4_FEATURE_INCOMPAT_CASEFOLD`
   (`enabling_casefold = 1`, line 1474 [1476]) it fills in the default encoding
   `EXT4_ENC_UTF8_12_1` and the flags, checks them, and calls
   `ext4_update_superblocks_fn(sb, ext4_sb_setparams, &params)` (line 1527
   [1529]). Nothing else is done for casefold (no mount check, no encoding load).
2. `ext4_sb_setparams()` (line 1317 [1319]) writes `s_encoding`,
   `s_encoding_flags` and ORs the feature bits into the in-memory and on-disk
   superblock (line 1364-1367 [1366-1369]). `ext4_has_feature_casefold(sb)` is
   now true.
3. `sb->s_encoding` (the `struct unicode_map *`) is set only by
   `ext4_encoding_init()` (`fs/ext4/super.c:4585` [4647], early return at 4591
   [4653] if the feature is off or `s_encoding` is already set), which
   `ext4_fill_super()` calls once, at mount (line 5324 [5426]). So it stays NULL.
4. `ext4_ioctl_setflags()` (`ioctl.c:621-622` [622-623]) lets `EXT4_CASEFOLD_FL`
   be set on an empty directory if `ext4_has_feature_casefold(sb)`; it does
   not look at `s_encoding`.
5. The next readdir of the directory: `ext4_readdir` -> `ext4_htree_fill_tree`
   -> `htree_dirblock_to_tree` -> `ext4fs_dirhash()` (`fs/ext4/hash.c:298`
   [318]: `um = dir->i_sb->s_encoding`; line 309 [329]: `utf8_casefold(um, ...)`
   for `IS_CASEFOLDED(dir)`), `utf8_casefold()` (`fs/unicode/utf8-core.c`),
   `utf8byte()`, `utf8nlookup()`, which dereferences `um->...` (address 0x18).
   `utf8ncursor()` only stores `um`; neither it nor `utf8_casefold()` checks it
   (mainline `fs/unicode/utf8-norm.c:420-442`).

Still present in mainline `af32da41b032` by reading: the same four places
(`ioctl.c:1390/1476/1529`, `super.c:4647-4683` with the one call at 5426,
`hash.c:318/329`) and no NULL check in the unicode code. No reproducer run on
mainline (it was not built: the step forbids building kernels).

### Regression? No: the ioctl is new in 6.18

`grep TUNE_SB fs/ext4/ioctl.c` finds nothing in v6.17; v6.18 has it. There is
no known-good version of the same feature, so no `#regzbot` line.

### Suggested fix (prose only)

The invariant "casefold feature on a mounted filesystem implies `s_encoding` is
loaded" is what the rest of ext4 (and the 2023 removal of the redundant NULL
checks) relies on. Either (a) refuse to set the casefold feature on a mounted
filesystem in `ext4_ioctl_set_tune_sb()` (return `-EOPNOTSUPP`, as the ioctl
already does for other things it cannot do online); or (b) load the encoding
first, i.e. call the loading part of `ext4_encoding_init()` for the requested
encoding before committing the superblock change and fail the ioctl if it
fails (and unload on a later failure); and, as defense in depth, (c) have
`ext4_ioctl_setflags()` (and, if wanted, `ext4fs_dirhash()`) test
`sb->s_encoding` rather than only the feature bit. (a) is the smallest. Any
root process can issue the ioctl (the kernel's own `set_feature_incompat_mask`
advertises casefold as settable).

### Draft mail

    From: <russ>
    To: Theodore Ts'o <tytso@mit.edu>, Andreas Dilger <adilger.kernel@dilger.ca>
    Cc: linux-ext4@vger.kernel.org, linux-fsdevel@vger.kernel.org,
        linux-kernel@vger.kernel.org,
        Gabriel Krisman Bertazi <krisman@kernel.org>
    Subject: [BUG] ext4: NULL pointer dereference in utf8nlookup() after EXT4_IOC_SET_TUNE_SB_PARAM enables casefold on a mounted filesystem

    (Recipients: fs/ext4 maintainers and the list from the pinned v6.18 MAINTAINERS;
    Gabriel Krisman Bertazi is the UNICODE SUBSYSTEM maintainer, who owns
    fs/unicode where the oops lands. Mainline MAINTAINERS now also lists
    reviewers Baokun Li <libaokun@linux.alibaba.com>, Jan Kara <jack@suse.cz>,
    Ojaswin Mujoo <ojaswin@linux.ibm.com>, Ritesh Harjani (IBM)
    <ritesh.list@gmail.com> and Zhang Yi <yi.zhang@huawei.com> as R:; add them or
    run scripts/get_maintainer.pl on a current tree. Attach reproduce.sh and
    casefold_helper.c.)

    Hi,

    On a mounted ext4 filesystem, EXT4_IOC_SET_TUNE_SB_PARAM (6.18+) can set the
    casefold feature. It writes the feature bit and the default encoding to the
    superblock, but sb->s_encoding is only loaded at mount, so afterwards
    ext4_has_feature_casefold() is true and sb->s_encoding is NULL. chattr +F
    only tests the feature bit, so an empty directory can be made casefolded,
    and listing it oopses.

    (dcfs, a caching FUSE filesystem I work on, runs its tests over ext4, xfs
    and btrfs in a QEMU guest; this came out of that.)

    Kernel: Linux 6.18.55 (Alpine linux-virt 6.18.55-r0, x86-64). Relevant config:
    CONFIG_UNICODE=y, CONFIG_EXT4_FS=m, CONFIG_FS_ENCRYPTION=y,
    CONFIG_PREEMPT_NONE=y, CONFIG_SMP=y, no KASAN. The code in mainline (I read
    af32da41b032, v7.3-rc6 plus the net merge of 2026-10-09) is the same, but I
    have not run it. The ioctl is new in 6.18, so there is no good version.

    Steps (as root, in a VM; the attached reproduce.sh does all of it, and
    casefold_helper is a 20-line wrapper for the two ioctls):

        mkfs.ext4 -q -F /dev/vdb              # no casefold feature
        mount /dev/vdb /mnt
        mkdir /mnt/d
        casefold_helper enable /mnt           # EXT4_IOC_SET_TUNE_SB_PARAM,
                                              # set_feature_incompat_mask = CASEFOLD
        casefold_helper mark /mnt/d           # FS_IOC_SETFLAGS +FS_CASEFOLD_FL,
                                              # i.e. chattr +F /mnt/d
        ls /mnt/d                             # oops

    Result:

        BUG: kernel NULL pointer dereference, address: 0000000000000018
        #PF: supervisor read access in kernel mode
        #PF: error_code(0x0000) - not-present page
        PGD 0 P4D 0 
        Oops: Oops: 0000 [#1] SMP PTI
        CPU: 0 UID: 0 PID: 452 Comm: ls Not tainted 6.18.55-0-virt #1-Alpine PREEMPT(none) 
        RIP: 0010:utf8nlookup+0x14/0x240
        Code: cc cc cc 0f 1f 00 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 4d 85 c0 0f 84 e5 01 00 00 48 89 f8 89 f6 48 89 d7 49 89 ca <48> 8b 50 18
        RSP: 0018:ffffb2ddc01a3a38 EFLAGS: 00010202
        RAX: 0000000000000000 RBX: ffffb2ddc01a3a58 RCX: ffff8fe644af8008
        RDX: ffffb2ddc01a3a94 RSI: 0000000000000001 RDI: ffffb2ddc01a3a94
        RBP: ffffb2ddc01a3a94 R08: 0000000000000001 R09: 0000000000000000
        R10: ffff8fe644af8008 R11: 0000000000000000 R12: 0000000000001000
        R13: ffff8fe644424000 R14: ffff8fe6416220d0 R15: ffff8fe644af8ff8
        FS:  00007fdcae639738(0000) GS:ffff8fe695757000(0000) knlGS:0000000000000000
        CS:  0010 DS: 0000 ES: 0000 CR0: 0000000080050033
        CR2: 0000000000000018 CR3: 000000000f9a4001 CR4: 0000000000370eb0
        Call Trace:
         <TASK>
         utf8byte+0x69/0x410
         utf8_casefold+0x6f/0xc0
         ext4fs_dirhash+0xe1/0x130 [ext4]
         htree_dirblock_to_tree+0x11b/0x390 [ext4]
         ext4_htree_fill_tree+0xef/0x3e0 [ext4]
         ext4_readdir+0x8e6/0xc20 [ext4]
         iterate_dir+0xb9/0x2a0
         __x64_sys_getdents64+0x75/0x110
         ? __pfx_filldir64+0x10/0x10
         do_syscall_64+0x88/0xfa0
         ? count_memcg_events+0xf4/0x1c0
         ? handle_mm_fault+0x159/0x260
         ? do_user_addr_fault+0x224/0x6c0
         ? clear_bhb_loop+0x40/0x90
         ? clear_bhb_loop+0x40/0x90
         ? clear_bhb_loop+0x40/0x90
         entry_SYSCALL_64_after_hwframe+0x76/0x7e
         </TASK>
        Modules linked in: virtio_blk fuse ext4 jbd2 mbcache crc16
        CR2: 0000000000000018
        ---[ end trace 0000000000000000 ]---

    Analysis: ext4_ioctl_set_tune_sb() (fs/ext4/ioctl.c:1388 in 6.18.55) handles
    EXT4_FEATURE_INCOMPAT_CASEFOLD by filling in the default encoding, and
    ext4_sb_setparams() (line 1317) writes it with the feature bit. Nothing
    calls ext4_encoding_init() (fs/ext4/super.c:4585), which is the only place
    that sets sb->s_encoding and is called once, from ext4_fill_super()
    (line 5324). ext4_ioctl_setflags() (ioctl.c:621) accepts EXT4_CASEFOLD_FL on
    an empty directory when the feature bit is set, and ext4fs_dirhash()
    (fs/ext4/hash.c:298, 309) passes the NULL sb->s_encoding to
    utf8_casefold(); neither it nor utf8ncursor() checks it.

    A fix could be one of: refuse to set the casefold feature on a mounted
    filesystem in ext4_ioctl_set_tune_sb() (-EOPNOTSUPP), the smallest change;
    or load the encoding (the loading half of ext4_encoding_init()) before
    committing the superblock change and fail the ioctl if it cannot; and in
    either case make ext4_ioctl_setflags() check sb->s_encoding instead of only
    the feature bit.

    I searched the linux-ext4 archive (a mirror, as lore refused my fetches) for
    "casefold", "tune", "tune_sb" and "s_encoding" and found no report of this.
    I can test a patch on 6.18.55 in the same guest.

    Thanks,
    <russ>

## Bug 2: btrfs, WARN_ON(csum_bytes) in btrfs_destroy_inode when a directory inode read fails

### What happens

On 6.18.55 a directory whose inode item has only been updated in memory (an
atime update by `ls`; any update that is still in the delayed-inode node) is
evicted, the device then fails reads, and `open_by_handle_at(2)` asks for the
directory. `btrfs_iget()` allocates the inode, `btrfs_read_locked_inode()`
takes the delayed-inode fast path (`btrfs_fill_inode()`), which for a directory
sets `index_cnt = (u64)-1`; then `btrfs_lookup_inode()` fails with EIO, and the
error path `iget_failed()` -> `make_bad_inode()` sets `i_mode = S_IFREG`, and
`iput()` -> `btrfs_destroy_inode()` sees a non-directory whose `csum_bytes`
(the same 8 bytes as `index_cnt`, since 6.11) is `0xffffffffffffffff`.

The evidence:

- the warning line is the `csum_bytes` check (source line match for v6.18.55
  and the disassembly of the shipped module: the third check of the `!S_ISDIR`
  block, offset 0xe0 in `struct btrfs_inode`, the slot `btrfs_alloc_inode`
  zeroes after `ro_flags`);
- `RAX` is `0x8000` at the warning: `i_mode & 0xf000` is `S_IFREG`, which
  `make_bad_inode()` sets (`fs/bad_inode.c:210`) and a freshly allocated
  inode that failed before its mode was read would have 0;
- `R13` is `0xfffffffb` (-EIO), the `ret` of `btrfs_lookup_inode()`;
- the call trace puts `destroy_inode` directly under `btrfs_read_locked_inode+0x14a`,
  the `iget_failed()` call;
- the reproducer's controls (6.18.55, same boot): the six directories not
  read by `ls` (clean inode items, the slow path, `i_mode` still 0 at the
  failure) give 0 warnings; the six listed ones give 6; with a small tree
  (`FILES=1`: no I/O needed for the lookups) no read fails and nothing warns;
- dcfs's own failure test (`fault_recover_btrfs_unpinned_test`, instrumented
  for this analysis and restored) showed the same: the warnings' `R14` values
  ran 0x1904-0x190b and 0x191c-0x1923, which are the inode numbers of the
  directories `cr`, `mk`, `ul`, `rn`, `sa`, `sx`, `ln`, `sy` of two of the test's
  trees (checked against `ls -i` for the second tree, 6428-6435; the first by
  the same sequential numbering), the directories the test lists before it
  injects the failure; not `lk` and `rd` (stat only), nor any file. That the
  value in `R14` is the inode number is inferred from this match.

The earlier attribution in this repository (plan log and the comment in
`guest/fault_recover.sh`: "btrfs_read_locked_inode's error path") was right
about the path and wrong about why csum_bytes is non-zero (a dirty write is
not involved: error-reads mode alone, which fails no write, produced 19
warnings in one run). The candidate "an inode evicted with csum accounting left
after failed writes" is ruled out by the same run.

### Code path (v6.18.55; mainline `af32da41b032` in brackets)

- `fs/btrfs/btrfs_inode.h:284/291` [275/282]: `index_cnt` and `csum_bytes` in
  one `union` ("unify index_cnt and csum_bytes from struct btrfs_inode", Filipe
  Manana, `d9891ae28b0d`, 2024-04-30, first in v6.11: v6.10 has two fields).
- `fs/btrfs/inode.c:3985` [4088] `btrfs_read_locked_inode()` calls
  `btrfs_fill_inode()` (`fs/btrfs/delayed-inode.c:1895` [1875]), which sets
  `inode->index_cnt = (u64)-1` for a directory (line 1944 [1924]) and returns 0
  when the inode has a dirty delayed node.
- `inode.c:3993` [4096] `btrfs_lookup_inode()` fails (EIO); `goto out`, line
  4188 [4292] `iget_failed(vfs_inode)` (comment above it: "release the path
  before iget_failed()").
- `fs/bad_inode.c:206-216, 245-250` (v6.18 and mainline): `iget_failed()` calls
  `make_bad_inode()`, `i_mode = S_IFREG`, then `iput()`.
- `inode.c:8044-8048` [8157-8161] `btrfs_destroy_inode()`: `if (!S_ISDIR(i_mode))
  { WARN_ON(delalloc_bytes); WARN_ON(new_delalloc_bytes); WARN_ON(csum_bytes); }`.
  Line 8047 [8160] is the one that fires.

In the not-filled path (`inode.c:4039` [4142]) `index_cnt = (u64)-1` is set
after the inode item is read, so a failure after that point in the same
function (`btrfs_init_file_extent_tree()`, `btrfs_add_inode_to_root()`) would
warn the same way; I did not test it. `delalloc_bytes` and `new_delalloc_bytes`
are also unions for directories (`first_dir_index_to_log`,
`last_dir_index_offset`) but are 0 at these points.

Present in mainline by reading (`inode.c:8157-8160`, `btrfs_inode.h:275/282`,
`delayed-inode.c:1924`, `inode.c:4142/4292`). Not run on mainline.

### Real bug or expected diagnostic?

An error path that works as designed (the read fails, `-EIO`/`-ESTALE` goes back
to the caller, the inode is dropped) then reports an accounting leak that does
not exist. It is a bug in the diagnostic: harmless to data, but a `WARN_ON`
(not `_ONCE`) per failed directory read, and `panic_on_warn` machines and
syzbot treat it as a kernel failure. dcfs's test harness fails a boot on it.

### Existing report

syzbot, "[syzbot] [btrfs?] WARNING in btrfs_destroy_inode (2)", 2024-09-12,
6.11.0-rc6-syzkaller-00363-g89f5e14d05b4: `WARNING at fs/btrfs/inode.c:7729`.
In v6.11-rc6 that line is `WARN_ON(inode->csum_bytes);`, and its trace is
`destroy_inode <- evict <- btrfs_iget_path <- btrfs_iget_logging <-
add_conflicting_inode <- copy_inode_items_to_log <- btrfs_log_inode ...
btrfs_sync_file`: an `iget` that failed, the same shape. No reproducer, no
reply on the list, no fix found; dashboard
https://syzkaller.appspot.com/bug?extid=3f149babf28b57cee242 (not fetched),
archive https://ratatoskr.run/linux-btrfs/2024/09/2745825 and
https://lkml.rescloud.iu.edu/2409.1/06268.html. Its trace has no registers, so
I cannot prove it is the same instance, only the same warning at the same
condition on the same failure path (v6.11-rc6 is the first release with the
union). The draft is therefore a reply to it. The other recent syzbot report,
"(3)" (2025-10-26, lines 7942/7943/7948 during balance), is a different
warning and not this.

Related, not a fix: "btrfs: release path before iget_failed() in
btrfs_read_locked_inode()" (Filipe Manana, 2025-12-19,
https://ratatoskr.run/linux-btrfs/2025/12/7622784) is in 6.18.55 (it is the
comment above `iget_failed()`, `inode.c:4180-4188`) and does not address this.
No fix for the warning found, so 6.18.55 lacks nothing upstream has.

`#regzbot`: not included. By reading, 6.10 cannot warn this way (separate
fields) and 6.11 can, but no 6.10 kernel was run, so there is no *tested*
known-good version. If russ wants the tag: `#regzbot introduced: d9891ae28b0d`.

### Suggested fix (prose only)

Any of: (a) in `btrfs_destroy_inode()` skip the three data-accounting checks
for a bad inode (`is_bad_inode(vfs_inode)`), since `make_bad_inode()` has
changed `i_mode` under it; (b) in `btrfs_read_locked_inode()`'s error path
clear `index_cnt` (the shared 8 bytes) before `iget_failed()`, or in
`btrfs_fill_inode()` and the slow path set `index_cnt = (u64)-1` only after the
last step that can fail; (c) test the original mode, not `i_mode`, in
`btrfs_destroy_inode()` (e.g. remember "was a directory" in `runtime_flags`).
(a) is one line and also covers the not-filled path.

### Draft mail (a reply to the syzbot report)

    From: <russ>
    To: Chris Mason <clm@fb.com>, David Sterba <dsterba@suse.com>
    Cc: linux-btrfs@vger.kernel.org, linux-fsdevel@vger.kernel.org,
        linux-kernel@vger.kernel.org, Filipe Manana <fdmanana@kernel.org>,
        syzbot+3f149babf28b57cee242@syzkaller.appspotmail.com
    In-Reply-To: <Message-ID of the syzbot mail of 2024-09-12: take it from the lore
        thread; the mirrors I could read do not show it>
    Subject: Re: [syzbot] [btrfs?] WARNING in btrfs_destroy_inode (2)

    (Recipients: fs/btrfs maintainers and list from the pinned v6.18 MAINTAINERS;
    mainline now lists David Sterba as M: and Chris Mason <mason@kernel.org> as
    R:. Filipe Manana wrote the commit that made the field a union and the
    iget_failed() change. If the thread is not found on lore, send it as a fresh
    "[BUG] btrfs: ..." with a Link: to the syzbot dashboard. Attach
    reproduce.sh and handle_helper.c.)

    Hi,

    This may be the warning syzbot reported on 2024-09-12 (6.11.0-rc6, inode.c:7729,
    which is WARN_ON(inode->csum_bytes), through btrfs_iget_path() ->
    evict() -> destroy_inode()); I have a reproducer for the same warning on an
    iget failure path, and an explanation. I cannot prove it is the same instance
    (the report has no registers).

    (I hit it testing dcfs, a caching FUSE filesystem that opens backing
    inodes with open_by_handle_at(), with a failing device under btrfs.)

    Kernel: Linux 6.18.55 (Alpine linux-virt 6.18.55-r0, x86-64), CONFIG_BTRFS_FS=m,
    CONFIG_BTRFS_DEBUG and CONFIG_BTRFS_ASSERT not set, CONFIG_PREEMPT_NONE=y,
    CONFIG_DM_FLAKEY=m. Mainline (read at af32da41b032, v7.3-rc6 plus the net
    merge of 2026-10-09) has the same code; I have not run it.

    Steps (as root, in a VM; the attached reproduce.sh does all of it, and
    handle_helper is a small wrapper for name_to_handle_at / open_by_handle_at):

        mkfs.btrfs -f /dev/vdb; dmsetup create kbug --table "0 $SECTORS linear /dev/vdb 0"
        mount /dev/mapper/kbug /mnt
        touch /mnt/pad/file-with-a-longish-name-to-fill-leaves-{1..6000}  # metadata spans leaves
        mkdir /mnt/clean{1..6} /mnt/listed{1..6}                          # one file in each
        handle_helper handle /mnt/<dir> ...        # name_to_handle_at of the 12 directories
        sync; ls /mnt/listed{1..6} >/dev/null      # relatime: atime updated, in memory only
        echo 3 > /proc/sys/vm/drop_caches
        dmsetup suspend --nolockfs kbug; dmsetup load kbug --table "0 $SECTORS flakey /dev/vdb 0 0 1 1 error_reads"; dmsetup resume kbug
        handle_helper open /mnt <the clean handles>   # ESTALE, no warning
        handle_helper open /mnt <the listed handles>  # ESTALE, one WARNING each

    Result (6 warnings for the 6 listed directories, 0 for the clean ones;
    the first one):

        ------------[ cut here ]------------
        WARNING: CPU: 0 PID: 557 at fs/btrfs/inode.c:8047 btrfs_destroy_inode+0x224/0x290 [btrfs]
        Modules linked in: virtio_blk fuse dm_flakey dm_mod btrfs raid6_pq xor crc32c_cryptoapi xxhash_generic blake2b_generic
        CPU: 0 UID: 0 PID: 557 Comm: handle_helper Not tainted 6.18.55-0-virt #1-Alpine PREEMPT(none) 
        RIP: 0010:btrfs_destroy_inode+0x224/0x290 [btrfs]
        Code: bb 78 ff ff ff 00 0f 84 49 fe ff ff 0f 0b 8b 93 e0 fe ff ff 85 d2 0f 84 47 fe ff ff 0f 0b e9 40 fe ff ff 0f 0b e9 62 fe ff ff <0f> 0b e9 69 fe ff ff 0f 0b e9 46 fe ff ff 0f 0b e9 
        RSP: 0018:ffffa75800213b60 EFLAGS: 00010286
        RAX: 0000000000008000 RBX: ffff922cc5916980 RCX: 0000000000000000
        RDX: 0000000000000000 RSI: 0000000000000000 RDI: ffff922cc5916980
        RBP: ffff922cc4850800 R08: 0000000000000000 R09: 0000000000000000
        R10: 0000000000000000 R11: 0000000000000000 R12: ffff922cc4f9f540
        R13: 00000000fffffffb R14: 0000000000001874 R15: ffff922cc59167f0
        FS:  000000002b6643c0(0000) GS:ffff922d16757000(0000) knlGS:0000000000000000
        CS:  0010 DS: 0000 ES: 0000 CR0: 0000000080050033
        CR2: 00000000002093b0 CR3: 0000000004842002 CR4: 0000000000370eb0
        Call Trace:
         <TASK>
         destroy_inode+0x36/0x80
         btrfs_read_locked_inode+0x14a/0x6a0 [btrfs]
         btrfs_iget+0xc2/0x100 [btrfs]
         btrfs_get_dentry+0x5d/0xd0 [btrfs]
         exportfs_decode_fh_raw+0x8d/0x420
         ? __pfx_vfs_dentry_acceptable+0x10/0x10
         ? __lruvec_stat_mod_folio+0x80/0xd0
         do_handle_open+0x276/0x550
         ? do_syscall_64+0x88/0xfa0
         do_syscall_64+0x88/0xfa0
         ? do_user_addr_fault+0x224/0x6c0
         ? clear_bhb_loop+0x40/0x90
         ? clear_bhb_loop+0x40/0x90
         ? clear_bhb_loop+0x40/0x90
         entry_SYSCALL_64_after_hwframe+0x76/0x7e
        RSP: 002b:00007ffec5305d30 EFLAGS: 00000202 ORIG_RAX: 0000000000000130
        RAX: ffffffffffffffda RBX: 000000002b6643c0 RCX: 0000000000269317
        RDX: 0000000000000000 RSI: 00007ffec5305df8 RDI: 0000000000000004
        RBP: 00007ffec5306290 R08: 0000000000000000 R09: 0000000000000000
        R10: 0000000000000000 R11: 0000000000000202 R12: 00007ffec53063c8
        R13: 0000000000000002 R14: 00000000002a8ce0 R15: 0000000000000002
         </TASK>
        ---[ end trace 0000000000000000 ]---

    Analysis: the warning is the csum_bytes check (inode.c:8047 in 6.18.55; the
    third check of the !S_ISDIR block). csum_bytes shares its storage with
    index_cnt since d9891ae28b0d ("btrfs: unify index_cnt and csum_bytes from
    struct btrfs_inode", first in v6.11). For a directory with a dirty delayed
    inode, btrfs_read_locked_inode() takes the btrfs_fill_inode() fast path
    (inode.c:3985), which sets index_cnt = (u64)-1 (delayed-inode.c:1944);
    btrfs_lookup_inode() then fails with EIO (R13 = -5 above), and the error
    path (inode.c:4188) calls iget_failed(). make_bad_inode() sets i_mode to
    S_IFREG (RAX = 0x8000 above; fs/bad_inode.c:210), so btrfs_destroy_inode()
    takes the !S_ISDIR branch and reads index_cnt as csum_bytes. The controls: the
    six directories that were not touched in memory (the slow path, where
    index_cnt is only set after the item is read) do not warn, and with a small
    tree the lookups need no I/O and nothing fails.

    The same would happen for a failure after index_cnt is set in the slow path
    (inode.c:4039), which I did not test.

    A fix could skip the three checks for a bad inode in btrfs_destroy_inode(),
    or clear index_cnt in btrfs_read_locked_inode()'s error path before
    iget_failed(), or set it only after the last step that can fail. I can test
    a patch on 6.18.55 in the same guest.

    I searched the linux-btrfs archive (a mirror, as lore refused my fetches)
    for "destroy_inode", "index_cnt", "iget_failed" and "bad inode"; this is the
    only matching report I found.

    Thanks,
    <russ>

## Not done, and things for russ to know

- lore.kernel.org could not be fetched (HTTP 403, Anubis); the mirrors' search
  is subject-only. Search lore by hand before sending (above).
- Only the pinned kernel 6.18.55 was run; mainline is "present by reading".
  The step forbids building kernels, and the host kernel was not touched.
- No Message-ID for the 2024 syzbot mail was obtainable; the In-Reply-To
  header is a placeholder.
- The two guest tests reuse the existing DISABLED_ names
  (`casefold-tune-online-oops`, `BTRFS_FAILED_INODE_READ_WARNS`) because the
  repo-shape check wants every `disabled` name in the README's Limitations
  section and the README is outside this step's files. The older
  `casefold_tune_oops_test` and `fault_recover_btrfs_unpinned_test` are
  unchanged; the comment in `guest/fault_recover.sh` (pin_inodes: "from
  btrfs_read_locked_inode's error path") and the log's explanation are now
  superseded by "Bug 2" above (the orchestrator owns those edits).
- Relevant to dcfs: the btrfs warning needs a directory with only in-memory
  inode updates, which dcfs creates by listing it (atime). Pinning inodes
  (`dcfs_pin=1`) avoids the cold read; reading directories with `noatime`
  or `lazytime` on the backing filesystem should avoid the warning too (not
  tested).
