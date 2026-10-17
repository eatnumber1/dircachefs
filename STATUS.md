<!-- SPDX-License-Identifier: MIT -->
# dcfs status

[![CI](https://github.com/eatnumber1/dircachefs/actions/workflows/ci.yml/badge.svg)](https://github.com/eatnumber1/dircachefs/actions/workflows/ci.yml)
[![Mutation testing](https://github.com/eatnumber1/dircachefs/actions/workflows/mutation.yml/badge.svg)](https://github.com/eatnumber1/dircachefs/actions/workflows/mutation.yml)
![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/eatnumber1/dircachefs/badges/coverage.json)
![TLC](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/eatnumber1/dircachefs/badges/tlc.json)

Where the project stands, for anyone following along. Updated by the
orchestrator at every merge; the full record is
[docs/plan/log.md](docs/plan/log.md), the order of work is the Queue in
[docs/plan/README.md](docs/plan/README.md). Not released: no backward
compatibility is kept yet.

_Last updated: 2026-10-16._

## Where it is

- **The core claim holds on real hardware.** On the maintainer's production
  file server (2026-10-11), metadata work over 175,000 files, locally and
  from an NFS client, kept the disk in standby; one data read woke it
  ([trial write-up](docs/plan/notes/trial-2026-10-11.md)).
- **Kernel floor: Linux 6.9** (FUSE passthrough, `FS_IOC_GETFSUUID`).
- **Backings:** local ext4, xfs, btrfs. Network filesystems (NFS, SMB) are
  not supported yet: they have no filesystem UUID (step 14.10).
- **Known slow:** metadata-heavy workloads run 15-76x slower than the bare
  filesystem in the test guests, almost all of it daemon CPU (step 17.4
  profiles it, optimized build first).

## Now

| Step | What | State |
|---|---|---|
| 25.9b | Finish "no intentional crashes": the poisoned-backing shutdown test, banned symbols, remove unreachable checks | in flight |
| 25.28b | The sync interval bounds the cache's own durability lag after a power cut | queued |
| 17.2b | Setgid on writes through passthrough: reproduce, then fix | queued |
| 12.18b | SibylFS's checker as a second oracle on a dcfs mount | queued |

## Next

1. **The atomicity contract** (25.10, 12.16): bounded retries out, a
   syscall-level ideal spec in TLA+ checked against SibylFS, then SibylFS
   retired.
2. **Identity without a UUID** (14.10): network backings.
3. **The slowness** (17.4): profile on an optimized build, then decide
   between tuning and design.
4. Everything else in the Queue's parked list; kernel patches last.

## Waiting on the maintainer

- Two kernel bug reports to send ([drafts](docs/plan/notes/kernel-bugs-2026-10-09.md)).

## Recently landed

- 2026-10-16: 15.11c, the repairs after deleting the old mount form (the
  systemd test, stale docs, dead code; fsck on bind lines verified).
- 2026-10-16: the SibylFS verdict re-checked and reversed (usable as a
  differential oracle); link-time fault injection kept, the virtual-syscalls
  rewrite reverted; reviews of the temporary orchestrator's week; 26.29
  cleanups; the Gemini watchdog and the Claude usage reader under
  `tools/agents/`.
- 2026-10-11/12: the real-hardware trial; deterministic test budgets (26.17b);
  crash audit and no-crash conversion (25.9, being finished in 25.9b); setgid
  on unprivileged writes (17.2); the old mount form deleted (15.11).
