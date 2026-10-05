# Phase 21 — Real-hardware checks (manual, russ)

QEMU cannot show that a real disk stays spun down. These checks are run by
russ on real hardware, from a written procedure, and recorded in `log.md`.

**21.1 Trial on a spare disk (after Phase 11).** On a disk holding nothing
precious:
1. Mount dcfs over it (command-line form current at that point) with the
   cache on an always-on disk; warm the cache (`find` over the tree).
2. Spin the disk down (`hdparm -y`) and confirm standby (`hdparm -C`, or
   `smartctl -n standby -i`).
3. For an hour, run a metadata workload: `ls -R`, `find`, `stat`,
   `updatedb` restricted to the mount, `df`, browsing over NFS from
   another machine. Check standby every few minutes; the disk must stay
   in standby throughout.
4. Read a file: the disk spins up (expected); let it spin down again and
   repeat step 3 briefly.
5. Unmount and remount; repeat step 3 briefly (warm restart).
Any spin-up during metadata-only activity is a bug: reproduce it in QEMU
as a failing test (Phase 10's idle test or a new one) before fixing.

**21.2 Final check before deployment (after every other phase).** The
same procedure on the target setup, through the `mount.dcfs` wrapper and
fstab, including submounts or btrfs subvolumes as used there, and an
unclean shutdown (power off during idle) followed by a restart.

**Target server** (russ, 2026-10-05): x86_64 (the architecture all tests
use). Its kernel is upgraded to at least 6.9 before 21.2 (dcfs needs FUSE
passthrough and `FS_IOC_GETFSUUID`); the same applies to the machine used
for 21.1.

**Deployment rule (russ):** dcfs goes on the server with precious data
only after all phases are done, all tests pass (including the slow tier
and a soak run), and 21.2 passes.
