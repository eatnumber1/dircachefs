# Formal prior art for dcfs: survey and recommendations (2026-10-06)

Method: web search plus primary pages and repos (GitHub API for dates and licences). I read the SibylFS paper text, docs and opam file, and the dcfs plan. I built and ran nothing. (unverified) marks claims from search snippets only.

## Bottom line

- No TLA+ specification of a filesystem, VFS, FUSE, NFS handles, or a metadata cache exists in what I could find. The tlaplus/Examples index lists none. The only TLA+ forum thread asking for POSIX specs ([discuss.tlapl.us/msg05484](https://discuss.tlapl.us/msg05484.html)) got one answer: none, try SibylFS. Absence is from searching, so private or unindexed work may exist. The 12.4 and 12.5 models are original work.
- The directly reusable artefacts are test oracles and workloads, not spec text: SibylFS (ISC), ACE workloads (Apache-2.0) and Metis (Apache-2.0).
- The best returns for the planned models are techniques: refinement mapping, crash refinement, crash-during-recovery.

## 1. TLA+ and other specs of filesystem-like things

- **Specifying Systems: CachingMemory and LiveWriteThroughCache** ([directory](https://github.com/tlaplus/Examples/tree/master/specifications/SpecifyingSystems/CachingMemory)). Lamport's write-through cache over a memory, with a refinement mapping to the plain memory. It is not a metadata cache, so there is no text to reuse. The technique applies: state dcfs's transparency as "operations seen through dcfs refine operations on the backing filesystem". `CacheNeverWrong` is an invariant on served answers only. A refinement mapping would also cover mutation results (what create/unlink/rename return).
- **Formal VFS and POSIX work** ([Ernst et al.](https://arxiv.org/abs/1211.6187), [SPIN model of Linux VFS](https://www.academia.edu/288837/Model_Checking_the_Linux_Virtual_File_System), Alloy flash-FS model by Kang): none is TLA+ or FUSE-level; informative only.
- **POSIX concurrent specification** ([Ntzik et al., ECOOP 2018](https://www.doc.ic.ac.uk/~pg/publications/Ntzik2018Concurrent.html)). Separation-logic specs of path resolution, rename and link. It shows rename is the hard operation and that atomicity is per component. Informative for multi-directory `dcfs.tla`.
- **FUSE lookup counts, FORGET, generations.** No formal model found. The nearest thing to a spec is the prose in `fuse_lowlevel.h` ("lookup count", `forget`, "generation" for NFS), which is already in `~/Sources/libfuse`. This is the real source for 12.4.
- **NFS handles and ESTALE.** No formal model found. Sources are prose:
  - Linux [exporting.rst](https://docs.kernel.org/filesystems/nfs/exporting.html): handles stable across rename, truncate and server reboot; `fh_to_dentry` returns NULL when not found; disconnected dentries; one dentry per directory.
  - RFC 8881 section 4 (persistent and volatile filehandles, NFS4ERR_STALE, NFS4ERR_FHEXPIRED).
  - RFC 1813 (NFS3ERR_STALE).
  
  Informative; they give the rule list for 12.5.

## 2. Oracles and test generators

- **SibylFS** ([paper](https://6826.csail.mit.edu/2019/papers/sibylfs.pdf), [src](https://github.com/sibylfs/sibylfs_src), [docs](https://sibylfs.github.io/), [suite](https://github.com/sibylfs/sibylfs_test_suite)). A Lem and OCaml specification of POSIX with Linux, OS X and FreeBSD variants. It is an oracle: `fs_test exec` runs about 21,000 generated scripts in a chroot as root and records the libc calls and returns as traces; `fs_test check linux_spec <dir>` checks them against the model, with state sets for nondeterminism. The paper's own targets include **fusexmp/tmpfs** (a FUSE passthrough, like dcfs), SSHFS, overlayfs and NFS.
  - Scope gaps from the paper: no host crashes; timestamps modelled but largely untested; no unusual file types; no `*at` calls; the harness does not race calls. I saw nothing on xattrs or `O_TMPFILE`.
  - Licence is ISC, so scripts can be embedded.
  - Maintenance is poor: last release 0.5.0 (2015), last push 2020-12, opam pins `sexplib < 113.01`, `cow < 2`, `camlp4`; the docs call nix "more stable than opam". A hermetic Bazel build is the hard part.
  - Assessment: dcfs claims transparency, so the useful oracle is the backing filesystem itself, not POSIX. A differential run is cheaper: run the same scripts on the backing mount directly and through dcfs, normalise, and diff the traces. That needs only the scripts and an executor. The Lem checker adds POSIX-conformance of the backing, which is not dcfs's problem.
- **Ferrite** ([paper](https://sandcat.cs.washington.edu/ferrite), [repo](https://github.com/uwplse/ferrite)). Crash-consistency models (litmus tests, axiomatic and operational) with an ext4 model, in Rosette, Dafny and a C++ `blkenum`. It is a 2016 research prototype with 13 commits. It targets the backing filesystem's crash semantics, not dcfs's. Reusable as a checklist for `bOpts`/`dbOpts`: which post-crash states ext4 permits for create, rename and fsync of a file versus a directory. Do not run it; dm-log-writes replay already enumerates block-level states.
- **CrashMonkey and ACE** ([repo](https://github.com/utsaslab/crashmonkey), [OSDI'18](https://usenix.org/conference/osdi18/presentation/mohan)). Bounded black-box workloads: ACE exhaustively generates sequences of file operations with persistence points (fsync, fdatasync, sync). The J-lang output converts to C++ or bash and to xfstests format. CrashMonkey needs a custom kernel module and old kernels (tested up to ext4, xfs, f2fs, btrfs); the last push was 2022-10. Reusable: the ACE generator and its workloads, driven by our own dm-log-writes crash points. The oracle is simple for dcfs: after recovery, dcfs's view equals the backing filesystem's.
- **xfstests** (GPL-2.0, do not copy code, as `docs/plan/phases/11` already says). [README.fuse](https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git/plain/README.fuse) documents running `generic/` tests on a FUSE passthrough via a `mount.fuse.*` helper. Phase 11 already plans fsstress/fsx; its log-writes crash tests (using [log-writes](https://github.com/josefbacik/log-writes)) are the pattern for 11.1.
- **Metis** ([FAST'24](https://www.usenix.org/conference/fast24/presentation/liu-yifei), [repo](https://github.com/sbu-fsl/Metis), Apache-2.0, pushed 2025-03). A SPIN-driven differential model checker over mounted filesystems that compares a filesystem under test with a reference (RefFS or ext4) and hashes abstract states. dcfs with a plain backing mount as the reference is its natural use. Needs a patched SPIN and Swarm; optional.

## 3. Verified filesystems: what they teach

- **FSCQ and DFSCQ** ([SOSP'15](https://pdos.csail.mit.edu/papers/fscq:sosp15.pdf), [repo](https://github.com/mit-pdos/fscq), Coq, Crash Hoare Logic). Specs carry an explicit crash condition and require recovery to be idempotent under crashes during recovery. Check that `dcfs.tla` takes `Crash` at every step of `RecoverDirty`/`StartRun` and that coverage shows it fires (`-coverage 1`). The tree specification of fsync in DFSCQ (SOSP'17, unverified) is the best statement of what a directory fsync guarantees.
- **Yggdrasil** ([OSDI'16](https://www.usenix.org/conference/osdi16/technical-sessions/presentation/sigurbjarnarson)). Crash refinement: the set of states an implementation can leave after a crash must be a subset of the spec's. `CrashSafe` already has this shape, including "check without taking the crash". Informative; confirms the design.
- **DaisyNFS** ([OSDI'22](https://usenix.org/conference/osdi22/presentation/chajed)). A verified NFS server (Dafny on top of the GoTxn transaction system). I did not read how it models filehandles. Informative for 12.5; read its handle and stale-handle spec before writing ours. Repository location unverified.
- **ShardStore** ([SOSP'21](https://www.cs.utexas.edu/~bornholt/papers/shardstore-sosp21.pdf)). Executable reference models plus property-based tests, including crash and "dirty reboot" events, found 16 issues. The same shape as a differential test of dcfs against its backing filesystem, with crashes. Informative; the technique is reusable (biased argument selection, shrinking).
- **VeriBetrKV** is a key-value store, not a filesystem. I found nothing on "IronFS", and no relevant seL4 or CertiKOS filesystem work. Skip.

## 4. Trace validation and conformance in practice

- **Cirstea, Kuppe, Loillier, Merz** ([arXiv 2404.16075](https://arxiv.org/abs/2404.16075v2)) is our method. Points from the paper that matter here:
  - Log only some variables and let TLC infer the rest.
  - State-space cost grows steeply as trace detail shrinks: event names alone explode, while variables plus events plus arguments stay small.
  - Instrument at shared-state updates and commits (stable-storage commits are named).
  - The authors say the approach adapts to any imperative language, and that they expect Apalache's constraint overhead to be prohibitive.
- **MongoDB** ([2020 paper](https://arxiv.org/pdf/2006.00915), [2025 retrospective](https://www.mongodb.com/company/blog/engineering/conformance-checking-at-mongodb-testing-our-code-matches-our-tla-specs)).
  - Trace checking of the C++ server was abandoned after about 10 weeks: snapshotting multithreaded state was infeasible, and the spec was too abstract and written long after the code.
  - Test-case generation from the TLC state graph succeeded on a narrow algorithm: 4,913 tests, 100% branch coverage against 21% handwritten.
  - Our traces are per-request events at syscall boundaries, which avoids the snapshot problem; the unused technique is test generation.
- **CCF** ([NSDI'25, arXiv 2406.17455](https://arxiv.org/abs/2406.17455v2)). C++ with TLA+, trace validation in CI, six bugs found. It also used a simulation driver that forces interleavings. Closest published analogue to our setup.
- **Beyond what we do:** state-graph test generation (TLC `-dump`) and a driver that forces the model's interleavings. dcfs's link-time `--wrap` fakes could hold a request at a chosen syscall, which would make replay of a model behavior deterministic. This is my proposal; I have not checked feasibility against the code.

## 5. Recommendations (effort is my estimate)

| Artefact | Verdict | Effort |
|---|---|---|
| Specifying Systems CachingMemory | Read; decide whether `CacheNeverWrong` should become a refinement of the backing's behavior, mutation results included | 1 day reading, 2-4 days to add |
| libfuse `fuse_lowlevel.h`, kernel `exporting.rst`, RFC 8881 s.4, RFC 1813 | Extract a numbered rule list into the 12.4/12.5 step text before modelling | 1 day |
| DaisyNFS handle spec | Read before 12.5 | 0.5 day |
| FSCQ crash-during-recovery | Check coverage of `Crash` during recovery in `dcfs.tla`; add a known-bug variant if absent | 1 day |
| Ferrite ext4 model and litmus tests | Cross-check `bOpts`/`dbOpts` against it; port a few litmus tests to our replay | 2-4 days |
| ACE workloads | Adopt the generator's seq-1/seq-2 workloads as 11.1 inputs, with the oracle "dcfs view equals backing view after recovery" | about 1 week |
| SibylFS scripts plus trace diff of dcfs against direct backing | Adopt only if pjdfstest, fsx and fsstress leave gaps; requires an executor (the script grammar is unread) | 3-5 days |
| SibylFS Lem checker under Bazel | Skip for now: old OCaml pins, camlp4, little gain over differential | 2-3 weeks, high risk |
| TLC state-graph test generation with syscall-hold driver | Worth a spike after 12.4 | 1-2 weeks |
| Metis | Optional, later | about 1 week |
| Hydra, VeriBetrKV, Apalache | Skip | none |

Licences: SibylFS ISC; ACE/CrashMonkey and Metis Apache-2.0; xfstests GPL-2.0 (tool only, no copying); Ferrite unspecified. Hydra (fuzzer, LKL executor, not FUSE) is skipped.


# Addendum: ideas worth reimplementing in TLA+ for dcfs

Read from primary text this time: the SibylFS paper, the Ferrite paper, the FSCQ (SOSP'15) and DFSCQ (SOSP'17) papers, plus RFC 8881 and the kernel exporting doc by fetch (partial; RFC section numbers other than 5.8.1.5, 10.3, 10.4, 10.9 come from memory). Efforts are my estimates for writing and checking one TLA+ module with a known-bug variant.

## SibylFS

**Structure.** The model is a labelled transition system. Labels are CALL, RETURN, CREATE, DESTROY (processes) and TAU. A call is not atomic: call, then an internal TAU where the effect happens, then return. The model is "receptive": any process may call at any time. State is layered: a path-resolution module, a state module (directories map names to refs; files are refs), a file-system module that works on resolved paths, and a POSIX layer with processes and an open-file-description table (`fid_table`). Permissions and timestamps are "traits" mixed into a core model, so "core without permissions" exists. Errors are modelled as the set of all allowed errnos, built with a parallel combinator (`rename checks root ||| subdir ||| parentdirs ||| perms`), and the observed return picks the branch. A `parentdirs` check covers *disconnected* objects. Directory streams get a hand-written "must/may" semantics: from `opendir`, track every change to the directory; an entry untouched since `opendir` must be returned exactly once, an entry added or removed meanwhile may be. Concurrency is state sets: the checker keeps all model states consistent with the trace so far, and nondeterminism resolves when the next label is observed. Not covered: crashes, and (in what I read) `O_PATH` and xattrs.

**As an oracle.** Scripts run on a real mount, traces record libc calls and returns, the checker folds `os_trans` over the label sequence, and an empty state set is a failure. Refining the model against thousands of failing traces took 4-6 weeks of manual triage. The authors suggest an executable abstraction function from the implementation's state to the model's, checked at each step. That is what our trace validation does.

**dcfs modules.**
1. *Request lifecycle with an effect point* (`Obs.tla`, about 3-5 days). Give each FUSE request a call, an internal effect step and a reply, and keep a history of (call, reply). Property: every reply equals what the backing would answer at *some* instant between call and reply. `CacheNeverWrong` checks only the reply instant; this is the concurrency-correct form, and it also covers mutation results.
2. *Directory stream must/may* (about 1 week). Today readdir is one step. Model `opendir`, offset-cookie `readdir` chunks, and concurrent mutation plus cache refill between chunks. Invariants: untouched entries are returned exactly once, cookies stay stable when a refill changes the cache between chunks. I think this is where dcfs is least covered, but I have not checked the code.
3. *Three-level lifetime for 12.4*: dentry to inode record to kernel lookup count to open description. SibylFS's names/refs/`fid_table` split gives the shape. Its "disconnected" notion is a removed object still referenced, which FORGET must eventually release.
4. *Permissions as a separable trait* for `reval.tla`: compute `CanAccess(user, obj, op)` from backing state, and compare against what dcfs holds. The SibylFS paper never mentions `O_PATH` (I grepped its text), so an `O_PATH` descriptor (identity without access check) would be original modelling.

## Ferrite

**Structure.** A program is a sequence of events: `write`, `extend`, `setattr`, `link`, `unlink`, `rename`, plus sync events `fsync`, `sync`, `mark(label)`, `begin`/`commit`. The canonical trace has no crashes. A valid trace is a reordering that respects barriers and dependency rules (same file's metadata, same block, same directory by overlapping arguments, write before extend). A crash trace is any prefix of a valid trace. Operational form: state is ⟨inCore, onDisk⟩ with rules STEP, CRASH, and NONDET (PARTIALFLUSH). Sequential crash-consistency (no reordering) and ext4 (weak) are two instances of one framework.

**Litmus tests.** Format: `initial` (ends with an implicit sync), `main`, `exists?` predicates. `mark()` labels an externally visible event, so durability reads "marked ∧ ¬content". A predicate holds if some execution reaches it, so a test asks whether a surprising outcome is reachable. Patterns: prefix-append, ordered same-file and two-file overwrites, directory-op reordering, implied-directory-fsync, atomic-replace-via-rename (ARVR) and atomic-create-via-rename (ACVR). Found: ext4's ordered mode violates prefix-append, and most filesystems violate "safe rename".

**dcfs modules.**
1. *`BackingCrash.tla`* (1-2 weeks). Replace `bOpts` (a set of whole states) with an explicit ⟨inCore, onDisk⟩ backing and a constant selecting SCC, metadata-prefix or ext4-weak reordering by Ferrite's dependency rules. The risk it addresses: the single-directory model cannot show reordering across directories, which the plan's multi-directory models will need.
2. *Litmus tests as TLC configs* (1-2 days each). ARVR through dcfs, ACVR, implied-directory-fsync. Each asks whether a bad outcome is reachable through dcfs, and expects it only when it is reachable on the backing directly.
3. *`CrashRefines`*, the Yggdrasil-style top property (see DFSCQ): post-recovery views through dcfs ⊆ post-crash backing states. That makes "dcfs adds no new crash outcomes" a statement the litmus tests exercise.

## FSCQ and DFSCQ

**Structure.** FSCQ's disk model maps each block to ⟨last value, set of previous values⟩. A write adds to the set, `sync` clears it, a crash picks any member. Every procedure has a crash condition, and a recovery procedure runs after crashes *including crashes during recovery*. Logical address spaces give one abstraction per layer (a file is offset to data, a directory is name to inode, the log gives a logical disk). `log_intact(last, committed)` stands for all physical states that recover to either transaction state. Recovery is specified as idempotent: its crash condition implies its own precondition, so it can crash and restart repeatedly. DFSCQ adds the *metadata-prefix* spec as *tree sequences*: the abstract state is a sequence of trees, one per metadata update since the last sync. A crash yields any tree in the sequence. Data writes apply to every tree, so they may reorder relative to metadata. `fsync` truncates the sequence to its last tree, and `fsync` of a directory flushes all pending metadata (not just that directory). `fdatasync` flushes only file data.

**Oracle use.** None at run time; these are proofs. The ideas transfer.

**dcfs modules.**
1. *Explicit recovery idempotence* (about 1 day). Add an invariant: from every state where recovery has partly run, the recovery precondition still holds, so recovery may crash and restart. This is cheap, and it states what `RecoveryTerminates` only implies.
2. *Tree-sequence representation of the backing* (about 1 week). Keep a sequence of backing states since the last sync, with the crash picking any element, instead of a set of whole states. It preserves order (if b is on disk then a is), and the data-applies-to-all-trees trick would let the model add child objects' attributes and the writable-open gap in `formal/README.md` without multiplying the state sets.
3. *Directory-fsync semantics as a switch*: ext4/DFSCQ flush all metadata on any fsync; a filesystem that flushes only the named directory breaks a dirty set cleared on that assumption. Effort 2 days after (2).

## NFS semantics documents

**Structure.** Handles are opaque (fsid, fileid, generation). Persistence is a *class* (persistent, volatile, expire-with-open, expire on rename or migration) rather than a boolean. Servers must not reuse a handle for a different object without a generation change. The kernel's exporting doc adds: stable across rename, truncate and reboot; `fh_to_dentry` returns "not found" rather than another object; handle-created dentries are *disconnected* and must be spliced to a single dentry per directory. Caching: the *change attribute* is updated on every data or metadata change (RFC 8881 5.8.1.5); close-to-open requires revalidation at open (10.3); a delegation assures no inconsistent changes while held (10.4, 10.9). RFC 1813's weak cache consistency (pre-operation attributes returned with a mutation, from memory) lets a client tell whether its cache was current just before the mutation.

**dcfs modules.**
1. *`ident.tla` for 12.5* (1-2 weeks). Constants: handle class; backing inode recycling; durable vs volatile node ids and generations. Safety: a handle never resolves to a different object, and it resolves while its object exists and the class promises persistence. Stale: it yields ESTALE exactly when the object is gone or its generation differs. Add a cache wipe and restart to see whether handles survive. This is original work built from these rules; I found no published TLA+ for it.
2. *Disconnected objects* (inside item 1): a handle reaching an object with no parent path in the cache, which dcfs must rebuild from the database.
3. *Coherence parameter for `reval.tla`* (3-5 days): `Exclusive` (a delegation analogue, invariant `CacheNeverWrong`) versus `CloseToOpen` (out-of-band changes allowed; the weaker invariant is that after an open the held state is fresh as of that open). This turns the "documented limitation" in 12.3 into a checked statement. The model's existing directory stamp plays the change attribute's role, and phase 3 probes and the epoch compare-and-set play wcc's.

## Priority

1. Recovery idempotence invariant and the `Obs` effect-point property: cheap and likely to surface something.
2. `ident.tla` and the coherence parameter, because 12.3 and 12.5 need them anyway.
3. `BackingCrash.tla` with tree sequences, before any multi-directory model.
4. Directory streams, after (1).
