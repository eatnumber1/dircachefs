# dcfs plan

The plan for dircachefs (dcfs) after the original plan was completed on
2026-10-04. Phases are numbered in execution order; each has its own file
under `phases/`. Every phase writes its tests first and shows them failing
before the fix (see `process.md` and `/AGENTS.md`).

- `execution.md`: the schedule: waves of at most two parallel lanes,
  the model for each lane, and the sync points between waves.
- `process.md`: how phases are executed (conventions, subagent waves,
  models, review, budget).
- `history.md`: the original plan (its Phases 0-7, all done) and
  amendments 1-20. Its phase numbers are unrelated to the ones below.
- `log.md`: the execution log, including items waiting on russ.
- `decisions.md`: what was decided while planning these phases, what was
  rejected, and why.
- `notes/`: background verified while planning (kernel facts, the LKML
  discussion and libfuse_passthrough).
- `review-fixes.md`: fix steps R1-R3 from the 2026-10-06 review of waves
  1-2 (in progress).
- `audits/`: the race, crash, tri-state, style and test-coverage audits
  from the original plan.

To resume: read `/AGENTS.md`, this file, `decisions.md`, then the next
phase's file; `log.md` says where things stand.

## Order

| # | Phase | Status |
|---|-------|--------|
| 1 | [AGENTS.md](phases/01-agents-md.md) | done 2026-10-05 (reviewed by russ) |
| 2 | [Fix: world-readable cache database](phases/02-fix-cache-permissions.md) | done 2026-10-05 |
| 3 | [Drop the kernel patch; download and build the test kernel](phases/03-drop-kernel-patch-and-build-test-kernel.md) | done 2026-10-05 (deprecated patched-kernel path removed with Phase 4's wiring) |
| 4 | [Pinned host tools; `third_party/` convention](phases/04-pinned-host-tools.md) | done 2026-10-06 (R3: scratch filesystems from pinned mkfs tools, 4 KiB blocks) |
| 5 | [CI runs the QEMU suite](phases/05-ci.md) | in progress (5.1, 5.2 done 2026-10-07: workflow green under act for fast/presubmit, not yet published; ASan tier red: see log; 5.3 OSV done 2026-10-07; 5.3c narrowed the gate to shipped dependencies (abseil, gloop, libfuse, liburing, numactl, SQLite, matched by git commit): `osv` job is green under act, the 101 test-only Debian findings are informational; 5.4a test cache trusted, no forced rerun, 2026-10-07; 5.5 minimum-kernel 6.9 runner scheduled 2026-10-12) |
| 6 | [Test tiers and test speed](phases/06-test-tiers-and-test-speed.md) | in progress (6.1 tiers done 2026-10-06; 6.3 first fix merged, 6.3b tool builds config-independent; 6.2 guest memory right-sized 2026-10-07; 6.4a-d 2026-10-08 56980ad: host-only tests skipped under sanitizer/coverage configs (100 of 226), destroy_test 20k, suites sharded 3 ways in CI; 6.5 scrub merged 2026-10-09 6bf6bdb: nfsd grace/lease (nfs_test 221 s to 48 s), mmapwrite usr1, release_leak on the warning, the ASan bench OOM explained, `testutil waitmount`/`waitline` event waits replacing the polls, pjdfstest chmod moved for CI to judge) |
| 7 | [Pinned toolchain, coverage, warnings, UBSan, clang-tidy](phases/07-toolchain-coverage-warnings-ubsan.md) | in progress (7.1 pinned clang done 2026-10-07 f218d45: LLVM 22.1.8 via toolchains_llvm, static lld/libc++, hermeticity build; 7.1b sysroot-or-Alpine-clang follow-up; 7.2 coverage done 96c53e1 (baseline dcfs/*.cc 92.4% lines, 73.3% branches; CI coverage job); warnings audit merged 55caee3 (-Werror scoped to our sources, links fail on warnings: 7.3's scoping half); 7.4 UBSan done 1a65836 + 7.4b 56980ad (memory tests sized by reclaim, asan/ubsan as sharded jobs, osv restores the repo cache); 7.1b sysroot done in lane-6 pending merge; 7.3/7.5-7.7 planned); 7.1b merged 2026-10-08 508ad34: no host compiler, libc or headers (LLVM 22.1.8 + Debian 12 sysroot in @dcfs_llvm, 1.9 of 11.6 GB extracted, clang.cfg pins sysroot/lld/compiler-rt/libunwind/libc++ and reaches sandboxed actions, hermeticity gate + self-check, extract_test, glibc in the shipped SBOM; host still provides glibc >= 2.36 to run clang, bash/mktemp/realpath/rm, /bin/sh + dirname for the Alpine wrappers, rules_distroless tar/grep for the test rootfs); 7.6a merged 2026-10-11 033618e (Google-rules reformat of 87 files, //tools:format and format_test; 7.6b pins buildifier/shfmt/shellcheck after the reset); 7.3/7.7 next; 7.1c merged 2026-10-09 dea789b (sysroot glibc to trixie 2.41-12+deb13u4 from the same snapshot, merged-usr links added by extract.py, four `<runtime>` banned-symbol allows for libc.a members, the shipped-debs OSV scan gates with 21 ignores expiring 2027-01-06; hermetic, reproducible and fast tiers green); 7.1d merged 2026-10-09 704f11e (fsuuid_compat.h and an FS_CASEFOLD_FL shim gone; FS_IOC_SHUTDOWN stays, the sysroot lacks it); 7.2/7.4 coverage under sanitizers active merged 2026-10-12 db7f38b (coverage_asan_build, coverage_ubsan_build, +256 MiB RAM in QEMU, coverage:asan/coverage:ubsan in .bazelrc, ForkSplit fixed) |
| 8 | [Coverage close to 100%](phases/08-coverage-to-100.md) | in progress (8.1 gate merged 6788fc6: baseline 92.40/73.30, ratchets up; cheap gaps in lane-5; 8.2 survivors #1/#3/#4 killed 4af0981; 8.2b merged 55d0128 (two more survivors killed; a 60-mutant sweep: 51 killed, 7 survived -> 8.2c); 8.1b merged 803ecab (the gate fails on drops only; a rise is a note); 8.2c merged 6757e55 (nine survivors killed, one equivalent listed in tools/mutation/README.md; the harness got a fake kernel passthrough via --wrap of fuse_passthrough_open/close); 8.3 cheap gaps merged 56980ad; 8.4 merged 2026-10-08 1b2fb6d: default_permissions required (INIT refused without it, ACCESS fails closed), dead openat2 wrapper and FuseRequest move/default constructors deleted, read fallback covered in the harness; the dcfs-over-dcfs variant is impossible, see the phase file); 8.2e + 8.2f merged 2026-10-09 fd8f607 (11 of 12 survivors killed by tests under the 25.4 rules, the 12th already listed equivalent); 8.5 merged c6b8e20: deficit recovered (lines 96.50% / branches 81.25%), baseline raised to 96.50/81.24; refinement aad3117: Finding 1 AbslStringify on Op and Request in protocol_events.h, fuse_ops.h declarations dropped) |
| 9 | [File names are bytes](phases/09-file-names-are-bytes.md) | done 2026-10-06 (mountinfo/fstab/exports escaping in Phase 15; flag paths in main.cc deferred) |
| 10 | [Benchmarks](phases/10-benchmarks.md) | done 2026-10-06 (baseline recorded; readdir was 27x slower than the backing, 8x after the v3 indexes: 916 ms vs 114 ms for 10k entries, 2026-10-07; reduction plan in log) |
| 11 | [Crash, stress and failure testing](phases/11-crash-stress-failure-testing.md) | in progress (11.1 harness + first fault/power-cut tests merged 2026-10-08 ba7687d, no violations; 11.1b merged 461edcf (a cache-disk I/O error is EIO, not EAGAIN); 11.2 merged f3a2d2b (fault tests over ext4/xfs/btrfs, real power cuts with a verdict on both boots and a kill-mode self-check, ACE sequences a/b/fs on the checking build with negative fixtures, `testutil fsync`/`syncfs`; CI: asan limit 240 min); 11.6 fsfreeze running, lane-5; 11.2b merged 0beb46e (fsstress + fsx from a pinned xfstests, short/long/random modes, three comparisons against the backing, per-op success floors, self-check of the comparison; CI's shard lister now excludes `manual`); 11.6 fsfreeze merged 5840bf0 (what blocks and what does not; finding: a mutation held in a backing syscall blocks the whole single-threaded daemon, cached reads included; `testutil sql` read-only queries of the live cache); 11.3 merged 18c077a (fault_recover_test: 60 cells, outcomes marked asserted/truth-only/skip; bounded flakey window with a self-check; unpinned btrfs reproducer with an expected kernel WARNING); 11.5 + 11.4 merged 9374050 (shutdown ioctl tests over three filesystems; read-only-superblock guard at sync points and start; out-of-space tests; a create that reached the backing but could not be recorded replies EEXIST when the probe found it); 11.5b merged ff5e35b; 11.3b merged 2301c18 (ESTALE from a handle open is checked by name, present and unknown names, EIO when the object is still there or the check cannot run); 11.4c asan_mem for the enospc/shutdown tests merged; 11.5 + 11.4 merged; 11.7 identity oracle after every crash + row-durability checker rule approved 2026-10-09, first free lane) |
| — | **Trial point:** [21.1 trial on a spare disk](phases/21-real-hardware-checks.md) (russ, manual) | after 11 |
| 12 | [Formal model (TLA+) and trace validation](phases/12-formal-model-and-trace-validation.md) | 12.1-12.2 done 2026-10-07 (two review rounds; russ to read formal/README.md); 12.3 revalidation model done 2026-10-07 (2c3d79c); 12.4 inode lifetime done 2026-10-07 (def2352); 12.4b fixes merged 2026-10-08 (7136ac2: schema v5, stub ids monotonic, recovery probe, sweep at every start); 12.5 identity model merged 2026-10-08 (e3fe0aa; Phase 14 finding recorded); 12.6 + 12.7 merged 2026-10-08 5776861 (RecoveryIdempotent/CrashSafe through recovery, crash during recovery, effect points; 12.6b: recovery keeps rows dirty until a sync point, daemon crash vs power loss in the model, two known_bugs variants; large_test eternal: 9.16M states); 12.7b reply observability merged 2026-10-08 5094b31 (ReplyObservable over the backing's states, errno classes, traces carry the answer and the errno sent, two known_bugs, MC_large 10.24M states ~2000 s on the runner); 12.7c + 12.11 merged e05b345 (attribute-change End as a model step and trace event for directories; file-level changes still rely on the harness tests: 12.11b per-file trace); 12.8 merged 2026-10-09 638590d (bSeq: an ordered sequence of backing states since the last sync, `Reorder` seq/metaprefix/ext4, directory-fsync switch, Ferrite litmus configs under limitations/ and MC_litmus_*, `CrashRefines` for D; after the 23.8 merge F stamps from bSeq and `CrashRefines` covers D only; MC_crash_ext4 357k / metaprefix 292k states; three Opus rounds); 12.9/12.10 planned; formal expansion approved 2026-10-09: 12.12 audit done 2026-10-09 (notes/formal-coverage-audit-2026-10-09.md: G1-G24, a latent crash gap G5 in create vs a concurrent parent fill, order for the whole formal queue); 12.12a oracle hygiene merged 2026-10-10 20da809 (20 of 22 unbitten properties now have a premise test, two hold by construction; Trace.cfg checks TriState/CacheNeverWrong/CrashSafe/DurableSetSound on traces; MC_nolock_small in the medium tier; negative logs for the guard-decision checks; TLC coverage reports gated; a two-name CrashRefines variant); 12.13 tool merged 2026-10-09 9f5d17f (TLA+ mode of the mutation tool; first run 11 killed / 9 survived of 20 on the small tier; weekly mutation-tla job; the sweep follows 23.11); 12.14 merged 2026-10-10 73ed7e5 (the durability abstraction holds on ext4/xfs/btrfs under dm-log-writes replay incl. the torn WAL writeback: 48 reordered states per run judged, 0 violations; gaps listed), then 12.13 model mutation, 12.11b/c per-file and cross-directory trace, 12.10 generated tests (never checked in); 12.14-12.17 approved 2026-10-09 ("Yes to all"): SQLite durability abstraction tested, runtime checks derived from the model, a refinement target, the wrapper handoff modelled; 12.18 spike done 2026-10-11 849f372 (SibylFS trace checker discarded as oracle due to format impedance, synthetic metadata, and lack of crash semantics; retained as 12.16 reading reference) |
| 23 | [Semantics gaps: mmap/FORGET reconcile, removed objects, relatime, O_TMPFILE/copy_file_range/reflinks/ioctls, boundary stubs](phases/23-semantics-gaps.md) | done 2026-10-07 (23.1-23.5 e5c27cc; 23.6 held fd + 23.7 review fixes a64e666, two review rounds; one open investigation, see phase file); 23.8 atime from the held fd + cache-only directory/symlink atimes and 23.9 LINK of a removed object answering as the backing merged 2026-10-09 8ea5743 (schema v6, model file F with three known_bugs and two limitations, checker rule open-file); 23.10 directory-level dirty set: model half reported 2026-10-09 (d49aa87, lane-1), closed 2026-10-09 as investigated (directory-only marks unsound; branch kept unmerged, 12.12 says what to salvage); 23.8b merged 2026-10-09 162fd19 (FSnapView was not a congruence; fixed; no-VIEW ground truth passes, 23.8 results stand); 23.11 born-dirty create merged 2026-10-10 421b2aa (model and code; the ghost-row fix proven by the ten mixed-fault sequences that failed on main now holding on ext4/xfs/btrfs; create fsyncs 1n+1 -> 0n+1; Fable-reviewed) |
| 24 | [Alpine series pins for the kernel and host tools](phases/24-alpine-series-pins.md) | done 2026-10-07 (d0b2cc2: kernel linux-virt 6.18 + QEMU, e2fsprogs, xfsprogs, btrfs-progs, busybox from Alpine v3.24; ~30 min of cold compiles -> 22 s fetch; two review rounds) |
| 25 | [Style convergence](phases/25-style-convergence.md) | in progress (25.1 done 2026-10-07 2f5515e; 25.1c done 752c573: every syscall through syscalls.h, enforced; 25.2 at a quiet point after 26.2/26.10/22; format items in 7.6/7.7); 25.3 logging levels + Status message rules merged 2026-10-08 (docs 7d8fab7, code 76ab735: 12 sites WARNING->ERROR, log-or-return pairs removed, ProducedByDcfs marks dcfs's own errors so the reply handler logs ERROR only for those, INFO lifecycle lines, --v=1/2/3; post-merge review: no safety issue; 25.3b follow-ups merged b3355fc: failed replies and process errnos are dcfs's own, idle line at the first backing call with a 60 s constant, fsync sync points at --v=1, NotFound exclusions, cancel.sh asserts through --v=2, coverage baseline 95.97/79.22); 25.4 merged d5eb185; 25.5 one-abstraction-over-variants and branching-discouraged rules merged 2771f62; existing tests convert in 7.5/7.5b (ASSERT_THAT 854 vs EXPECT_THAT 860 today); 25.7 + 25.8 merged 2026-10-09 704f11e (the rule, the inventory, repo_shape `no_test_only_comments`; the tips' precedence in style.md and the agent definitions; 7.1d's shims with it); 25.7b merged 2026-10-09 a10f58c (the peer gone: a read-only `events::Bookkeeping` view the check hooks receive, tests tamper a copy, repo_shape refuses testonly friends); 25.9 no intentional crashes done 2026-10-12 cee5005 (converted remaining crashes to Status/RET_CHECK, ForkSplit exit status, BecomeDaemon StatusOr, PoisonBacking on credential restore failure, removed all 23 check_in_production entries); 25.12 merged 2026-10-10 7c23d43 (absl::FixedArray for the 11 sized-once buffers, a repo_shape check with a one-entry allowlist); 25.10, 25.11, 25.13, 25.14 queued; 25.14 merged 2026-10-10 cf12b7d (no dcfs:: inside namespace dcfs at 40 sites, DcfsErrnoToStatus → ProducedErrnoToStatus, DcfsMountDevice → MountDeviceOfFuseDcfs, two repo_shape rules with no allowlist); 25.15 merged 2026-10-10 (the short status macros enabled; SwitchTo in russ's shape with setfsuid/setfsgid wrappers returning Status; three carried-status ladders rewritten; first dcfs-style-reviewer pass, six rulings into style 1.6a); 25.19 merged 2026-10-10 7c92256 (docs/abseil-utilities.md); 25.21 merged 2026-10-10 66e0848 (clang-tidy aspect with the full check set and nine clang-query matchers under //tools, 1354 allowlisted findings as the backlog; placement fast-vs-presubmit open for russ); 25.22 census merged 2026-10-10 e1d0b95 (pattern counts, no reading pass); 25.17 merged 2026-10-10 4b804dc (835 macros to the short names, 38 nodiscards gone, banned_std and iterator_pair allowlists emptied, deps declared); 25.23 part 1 merged 2026-10-10 4608f4f (the style_fix run target; include-cleaner 570→393, designated-initializers 60→0); 25.23 part 2 merged 2026-10-11 b49c64f (include-cleaner 390→88 by hand, seventeen auto-fix checks to zero or near; the judgement checks remain for 25.25); 25.16, 25.20 parked per the Queue |
| 26 | [Bumpers: narrow checks around generated code](phases/26-bumpers.md) | in progress (26.1, 26.8, 26.9, 26.13 done 2026-10-07; 26.3 and 26.4 done 2026-10-07; 26.2 done 4511503 (checking build in the small/medium tiers; +5-15% fast, +25-35% presubmit); 26.6 started (lane-3); 26.7 and 26.10 done a6b86df; 26.5 done 6788fc6 + 26.5b 4af0981 + 26.5c 1be6511 (mutation-changed and subjects as their own jobs, cold input, cold timeouts, trace test split); 26.6 done (fault sweep by call site: 49 sites, 111 iterations, +4 s to fast, no violation) and 26.4b done (one observer, request budgets, slope tests) pending rebase/merge; 26.6 approved as a try, must report its runtime; 26.11 after 25.2; 26.12 done 96c53e1); 26.4b + 26.6 merged 2026-10-08 b866c15 (ProtocolEvents observer, request budgets, slope tests, fault sweep; 26.6 cost: +4 s fast tier, 26 s in the guest); 26.5d merged 0f73230 (eight operators, arid rules, seeded sampling, equivalent.txt, per-operator reports, baseline run, flaky detection, time budget; the per-push job reports survivors in its summary instead of failing); 26.14 quiet kernel merged 2026-10-09 b0d57de (writeback sysctls off in guest/init, vfs_cache_pressure kept at 100 because a low value starves drop_caches, one vCPU by default with CONCURRENT_CPUS for stress/cancel/pjdfstest/bench, coverage_diff check and determinism note; follow-ups 26.14b/c/d in the phase file); 26.14c merged 2026-10-09 8f0d3c2 (lost profile-counter updates between the daemon's threads: atomic counters under coverage, the gate rejects 2^31+ branch counts, docs/coverage.md created); 26.15 kernel bug reports merged 2026-10-09 badacf3 (reproducers, manual guest proofs, the btrfs cause proven spurious, drafts; russ sends); 12.13 model mutation tool running, lane-5; 26.14d merged 2026-10-09 dae95e1 (a fixture bug: forged requests carried pid 0 and hit the rate-limited groups WARNING; two credentials tests added); 26.16 merged 2026-10-09 86a3775 (profiles per job in the summary and as artifacts; CI re-extracted @dcfs_llvm on every job: about an hour of runner time per push); 26.16b merged 2026-10-09 4e21a50 (the output base's extracted toolchain cached by pin hash, 0.58 GB; verify on the second push); 26.14e merged 2026-10-10 628af12 (the DCFS_NOISY knob with self-checks both ways, the weekly six-runner noisy job, the mixed-fault generator whose 22 pinned sequences show the 23.11 ghost in ten on today's main, a handle-based identity oracle); 26.17a RAM-backed guest disks merged 2026-10-10 3e0c778 (loop over tmpfs, 13 fault targets, mem from measurements, a fault_recover silent pass found); 26.17b deterministic budgets merged 2026-10-11 7b713f6 (CPU budgets with 100% tolerance, I/O counts to io_budgets.txt, wall time hang guard, load-starvation flag, non-SMT core pinning, xfstests cost lines and shard budgets); 26.18 end-to-end assertions and oracles in C++ with googletest's structured output merged into test.xml, approved 2026-10-09, after 26.17 and the quota reset (Friday 2026-10-16); 26.14f merged 2026-10-09 6d74ef5 (ForkDaemon returned in both processes under shared continuous-mode counters; ForkSplit primitive, per-test artifact check, fork fixture in coverage_pipeline_test); 26.20 CI triage merged 2026-10-10 9fd3ff2 (the three shard failures of run 38018425705 were test bugs: an octal uptime_ms, exportfs -f before the asan daemon served, xfs committing its log between the persistence point and the cut); 26.21 CI triage merged 2026-10-10 ca65a1c (the snapshot helper read files with O_NOATIME after a power cut's clock regress moved atimes; stress_short 320 MiB under coverage); 26.22 CI triage merged 2026-10-10 010ba9a (fsck.cc was the coverage drop: tests for every uncovered path; the xfstests guest now mounts fusectl so umount waits; collect-logs cp fixed; the btrfs hour-long hang not reproduced, open); 26.24 CI triage merged 2026-10-11 67652b7 (an xfs-only harness race in the ACE dropahead window, quiesced before the operation; the lasting fix parked as 26.25; coverage baseline 96.26/80.85) |
| 22 | [Request cancellation (Ctrl+C, EINTR)](phases/22-cancellation.md) | done 2026-10-08 (87b35a0: option (b) checkpoints + session loop; `timeout -s INT 1 ls` on 20k entries 0.8 s instead of 14-20 s; kernel-blocked syscalls uninterruptible until coroutines/io_uring, documented) |
| 13 | [Connected backing fds](phases/13-connected-backing-fds.md) | 13.1-13.3 built 2026-10-09 (lane-2, four commits, ident.tla extended); freeze conflict resolved by 13.4's experiment: connectable handles (AT_HANDLE_CONNECTABLE, 6.13+, EINVAL fallback to the plain handle on 6.9-6.12) give connected fds without freeze protection; built 2026-10-09 (four commits, fault_freeze_test unchanged and green on ext4/xfs/btrfs, a kernel-behaviour guard test); Opus review: another round (identity weakened for objects without a generation or birth time, TLC-verified; the guard test uses a timer; the diagnostics benefit holds only for fstype=none); SHELVED 2026-10-09 (russ): branches kept in lane-2, the identity TLC configuration that found the hole merged on its own via 12.12a; revisit as 13.5 (O_PATH-then-handle open) when a kernel with the `__d_obtain_alias` connected-alias preference ships, russ's design note in the phase file |
| 14 | [Identity from the backing filesystem](phases/14-identity-from-backing-fs.md) | planned |
| 15 | [The `mount.dcfs` wrapper](phases/15-mount-dcfs-wrapper.md) | 15.0 man page done 2026-10-07; 15.1 + 15.2 merged 2026-10-09 106e16e (mount.dcfs with argv dispatch, dcfs. option split, private-namespace capture via open_tree, own daemonisation with readiness after INIT, syslog sink behind absl, exit 1/32/native, --source gone; three review rounds; util-linux paths untested until 15.6's guest); 15.6 systemd guest merged 2026-10-09 4fd3136 (Debian 13 cloud image pinned, systemd PID 1, fstab/mount -a/units/remount/journald asserted; found the restart race 15.6b and that mount(8) resolves UUID= before the helper); 15.6b merged 2026-10-10 461fc6a (umount.fuse.dcfs default and umount.fuse opt-in, the daemon's per-mount lock with a blocking wait and re-check, fusectl decides whether to wait, busy_timeout gone, the sleep/timer family banned, the sleep allowlist, style.md's no-timers rule; two Opus rounds); 15.5 fsck.dcfs, 15.8 allow_other always on, 15.9 the directory form is `dcfs.fstype=bind` (none refused, the clone capture gone) merged 2026-10-10 31e795c (fsck(8) statuses, the systemd guest checks passno 2 at boot, both deployment shapes asserted); 15.6d kernel: synchronous FUSE_DESTROY for plain mounts (the helper becomes the fallback) added 2026-10-10 to notes/kernel-patches.md, the list of kernel patches we want; 15.6c merged 2026-10-10 ddadd31 (Bazel's downloader prefers IPv4; mirrors for the Debian image; the fetch that broke CI run 38009667624 proven through Bazel); 15.7 docs merged 2026-10-09 50fee50 (README fstab/remount/logging/operations/log-message table, design.md wrapper and daemonization, packaging/ removed, dcfs.8 extended; nine systemd/util-linux statements for 15.6 to assert, listed in the phase file); 15.5 refined 2026-10-09 (fsck.dcfs for fstab's sixth field: delegates the backing's fsck, checks and rebuilds the cache, fsck(8) statuses) and 15.8 allow_other always on and refused in fstab (russ, 2026-10-09), both after 15.6b; 15.12 done 2026-10-11 e73edcb (NFS bind-export restart order, held-descriptor reading past client readahead hitting disk, runbook order updated); 15.11 done 2026-10-11 95dafad (native-type namespace capture and dcfs.fstype deleted, backing_capture removed, pre-mounted directory/bind model retained, docs/tests updated); 15.11c repair merged 2026-10-16 c0d1442 (the systemd guest fixture fixed for the bind form, the options test and the capture's leftovers deleted, fsck on bind lines documented as verified with a positive control, an NFS refusal test); 15.3 identity/cache path next (Opus); 15.4 stubs after Phase 14 |
| 16 | [Reject case-insensitive and encrypted directories](phases/16-reject-casefold-and-encrypted-dirs.md) | planned |
| 17 | [xfstests subset](phases/17-xfstests-subset.md) | 17.1 merged 2026-10-10 5ea0719; 17.3 done 2026-10-11 313bff6 (the slowness investigated; side-by-side cost accounting via xfstests_costs.py showed daemon CPU accounts for 80-98% of extra latency, architectural design bottleneck from single-threaded loop and per-op SQLite transactions, notes/slowness-17.3.md, xfstests.slow updated); 17.2 done 2026-10-11 bc010b9 (setgid dropped on unprivileged writes via AsCaller, generic/683-685 passed); 17.2b merged 2026-10-17 1ed1280 (setgid through passthrough and a non-owner's write; 17.2c, 17.2d leads queued); 17.4 spike scheduled (in-daemon CPU breakdown under serial metadata storms) |
| 18 | [Disk quota (EDQUOT)](phases/18-disk-quota-edquot.md) | planned (just before 19) |
| 19 | [Soak test (manual)](phases/19-soak-test.md) | planned (last automated phase) |
| 20 | [Statistics](phases/20-statistics.md) | design to be discussed first |
| 21 | [Real-hardware checks](phases/21-real-hardware-checks.md) (russ, manual) | 21.1 after 11; 21.2 last; runbook written 2026-10-11 (notes/trial-runbook-2026-10-11.md), russ to run it after an HWE kernel boot; first run 2026-10-11 on the production server (russ's agent): standby held under metadata work, a read woke the disk; NFS client leg in progress (notes/trial-2026-10-11.md); client leg done 2026-10-11: 175k files listed over NFS with the disk asleep, one read woke it; the restart-under-NFS finding is 15.12; 21.1 verdict 2026-10-11: success (russ) |

Execution started 2026-10-05 (wave 1).

**Cut line.** The trial point after Phase 11 is the first time dcfs is
worth trying on real hardware (a spare disk, nothing precious): by then
it runs on a stock kernel with pinned tools, the cache permissions are
fixed, coverage is near 100% under ASan and UBSan, file names are
handled as bytes, the idle test proves no background disk I/O, and the
crash, power-loss and failure tests pass. Deployment on a server with
precious data waits for every phase, including the soak test, and the
final hardware check (Phase 21.2).

## Milestones (russ, 2026-10-17: "Agreed with your milestones")

| | Milestone | Steps | Done when |
|---|---|---|---|
| M1 | A trustworthy core | 25.28b, 22.5, 17.2e, 25.10, 22.7 (placement provisional), 25.28c, 12.20, 26.40, 12.19, 26.39 | every user-visible promise in the README has a test and, where it is the protocol, a model property |
| M2 | CI you can trust | 26.41, 26.36's follow-ups, 7.6b, 25.33b, the shell ports (boot.sh first), 25.27 | main stays green through a week of normal merges with no budget re-measure |
| M2.5 | In-memory cache (the database on tmpfs) | 27.1, 27.2 | the guest suites pass with the cache on tmpfs, restart and reboot behave as documented, and the kernel-cache interplay is measured and decided |
| M3 | Daily driver on russ's file server | 15.12, 14.10b, 26.38, a runbook | two weeks serving the media pool unattended, with the disks' sleep time shown |
| M4 | Release candidate | 16, 18, the xfstests target, the coverage target, man page and README, packaging | a fresh machine installs, mounts and survives the README's documented failures from the docs alone |
| M5 | Ship | 19 (soak), 21.2 (real hardware); red CI then gates merges | the soak and the hardware checks pass |

M1 and M2 run in parallel; M3 starts as M1's protocol steps land, and
what russ's real use teaches steers what follows.

## Queue (russ, 2026-10-11: "I agree with you. Change the schedule.")

New phases are frozen. The order from here, and why:

1. **Finish what is in flight**: done 2026-10-11 (25.23 part 2 merged; the
   CI triages 26.24 and 26.26 merged). From here only a red push is
   dispatched before item 2. 26.27 (the
   gate on regions) turned out to be a coverage-pipeline change, not a
   gate tweak, and is parked in item 6.
2. **21.1, the spare-disk trial: done 2026-10-11, a success** (russ; the
   write-up is notes/trial-2026-10-11.md; the leftovers are in phase 21).
   The original text follows for the record. (russ, on real hardware; the procedure in
   phases/21 and its notes for his setup). The orchestrator's part: a
   one-page runbook for russ's machine (the HWE kernel boot, the fstab
   line, the warm-up, the hour of metadata work with `hdparm -C` checks,
   what to capture: the daemon's CPU seconds, the sync-point cadence
   against the spin-down, the NFS clients' behaviour; and a short control
   hour without dcfs, the kernel's own caches warmed and pinned, to
   quantify what dcfs buys on a RAM-starved server) as the first
   dispatch once (1) lands. Everything below is ordered by what this
   teaches; a spin-up during metadata-only work reorders it again.
3. **26.17b, deterministic budgets: done 2026-10-11** (CPU seconds with
   100% tolerance, I/O counts by kind held to io_budgets.txt, wall time
   hang guard, load-starvation flag, non-SMT pinning, xfstests cost lines
   and shard budgets: 7b713f6), so every number quoted afterwards is hard.
4. **17.3, the slowness: done 2026-10-11** (313bff6, docs/plan/notes/slowness-17.3.md:
   daemon CPU is 80-98% of extra time, wait/IO 2-20%, conclusion: architectural
   design bottleneck from single-threaded loop and per-op SQLite transactions,
   not tuning).
5. **25.10 and 12.16 (with 12.18), the atomicity contract**: the three
   bounded retries out with a liveness property in the model; the ideal
   spec as SibylFS's semantics restricted to our operations; the checker
   spike. Before coroutines reopen the races the current design makes
   impossible by construction.
6. **Then, by what the trial teaches.** Parked until then, in no order:
   15.11 (delete the native-type capture; 12.17 goes with it) once the
   trial has run on the bind form; the style backlog (25.9 audit, 25.11,
   25.13, 25.16, 25.20, 25.24, 25.25), 26.18 (e2e oracles in C++), 26.23
   (blocked-task diagnostics), 15.10 (fusectl in the remounting guests),
   12.11b/c, 12.15,
   17.2 (setgid), 17.1b/c, 11.7's remainder, 11.3c, 7.3, 7.6b, 7.7,
   26.14b, 15.12 (done 2026-10-11, e73edcb: README, QEMU bind shape, held-descriptor test, runbook order),
   14.9 (a database epoch in the nodeid, the handle corpus test:
   the NFS handle as a compatibility surface; before the first release), 26.25 (the ACE
   dropahead window on xfs), 26.27 (merged profdata
   to the host so the gate can count regions). The kernel patches (notes/kernel-patches.md) go last of all,
   after everything else is done (russ, 2026-10-11: their timelines are so
   long that our order adds nothing); the two kernel bug mails are
   separate and russ's. The style checks
   keep every new line to the rules meanwhile; the backlog shrinks as
   code is touched for other reasons, not as a campaign.

Budget: the weekly meter is at 91% on 2026-10-11 with the reset on
Friday 2026-10-16 01:00; one Haiku lane until then, Sonnet for a red
push; the queue above starts in earnest after the reset.

## Open decisions

- **Statistics design** (Phase 20).

## Future work (not scheduled)

- **dcfs in front of a network filesystem (NFS, SMB, ...): a supported
  use case for an administrator who can guarantee exclusive access, not
  for russ's own clients** (russ, 2026-10-11). He considered it for his
  NFS clients and withdrew because he cannot guarantee that nothing else
  writes behind dcfs there, which the write-through protocol requires;
  the server-side instance already answers those clients' attribute
  traffic from its cache. For an administrator who can guarantee it, a
  network backing stays supported (the wrapper's `_netdev`/network-type
  handling, the README's text on network backings, and the identity work
  for filesystems without stable handles are the places it lives), and
  nothing in the plan may remove it.

- **More backing filesystems.** Phase 14 (Identity from the backing filesystem) supports ext4, xfs and btrfs
  handles. Extending it:
  - The generic encoder `generic_encode_ino32_fh` (fs/libfs.c; types
    `FILEID_INO32_GEN` and `_WITH_PARENT`) is used by ext2, ext4, f2fs, fat,
    jfs, ntfs3, ntfs, squashfs, ufs, affs, befs, jffs2, the SMB client and
    overlayfs; one decoder covers all of them. Each still needs a way to
    open by inode number when uncached (generation 0 accepted, or a
    filesystem-specific lookup like xfs bulkstat) and a device identity
    (FS_IOC_GETFSUUID or an equivalent).
  - Filesystems with their own encoders need one decoder each: gfs2,
    ocfs2, nilfs2, udf, isofs, erofs, ceph, orangefs, tmpfs (mm/shmem.c).
  - NFS does not use inode+generation: the client's handle is the fileid,
    the file type and the server's opaque handle (`nfs_encode_fh`,
    fs/nfs/export.c). Supporting it needs a different identity scheme, e.g.
    a generation derived from the server handle, or dcfs-assigned
    generations kept in the cache (handles then do not survive a wipe).
    FUSE backings are the same case.
  - A FUSE backing that itself uses passthrough needs dcfs's
    `max_stack_depth` raised to stay within the kernel's stacking limit
    (fs/fuse/backing.c).
- **Kernel: passthrough mmap should keep the FUSE file referenced** so
  RELEASE follows `munmap`. Until then dcfs holds an fd per written file
  and reconciles at the last FORGET (Phase 23.6, marked in the code);
  delete that workaround when the kernel change is the minimum.
- **Kernel: a FUSE remap_file_range operation** so FICLONE/FICLONERANGE/
  FIDEDUPERANGE can reach a FUSE server (today the VFS handles them and
  they fail EOPNOTSUPP on every FUSE filesystem; copy_file_range shares
  extents on btrfs/xfs meanwhile).
- **FUSEX, FUSE's next protocol version** (possible direction). The
  proposal on fuse-devel (Miklos Szeredi, 2026-04-29) identifies objects
  by a variable-size id instead of a nodeid, and Luis Henriques's
  LOOKUP_HANDLE work for file-handle-based NFS export is to be rebuilt on
  top of it (LKML thread on russ's kernel patch, 2026-09-29). If it lands,
  dcfs could pass backing file handles through as object ids, replacing
  the inode-number-plus-generation scheme of Phase 14 and its "open by
  inode number" step, and lifting the ext4/xfs/btrfs restriction to any
  filesystem with stable handles. Track it; no work until it is merged.
- **The rest of the guest shell, ported to C++** (russ, 2026-10-17: "we
  can queue the full port as a new phase much later"). 25.34 sets the rule
  (no new shell), the helpers and the port list; scripts port when a step
  would change them substantially. The phase that ports the remainder
  is queued after the trial and the current phases, at the orchestrator's
  choice of when.
- Coroutines over io_uring (the coroutine phase in history.md). io_uring
  is required, with no fallback loop (russ, 2026-10-17: "I'm ok requiring
  io_uring be available"); startup refuses clearly where it is disabled.
  It must keep 26.41's exact work counts possible (russ, 2026-10-17: "if
  we do this we'll need to find another solution when we switch to
  io_uring and coroutines"): concurrency and unbounded retries make
  counts depend on scheduling, so the design keeps an injectable,
  deterministic executor (a fake ring the test steps one completion at a
  time) under which the slope and count scenarios run one request at a
  time, and counts retries separately. It owes
  the cancellation gap Phase 22 left open: a backing syscall already
  blocked in the kernel (a disk spinning up, a hung network backing,
  `syncfs`) cannot be interrupted today, so a FUSE_INTERRUPT waits for it.
  The plan is in `phases/22-cancellation.md` ("Coroutines + io_uring"):
  a per-request token checked at every await, `IORING_OP_ASYNC_CANCEL`
  for in-flight backing I/O. To check then: not every case becomes
  cancellable (there is no io_uring `syncfs`, and work an io-wq worker
  runs in uninterruptible sleep is not cancelled), so the phase lists
  which kernel-blocked calls stay uninterruptible, with a test for each
  that does become cancellable (russ, 2026-10-17: "Make sure it's
  written down to work on later").
