#ifndef DCFS_PROTOCOL_EVENTS_H_
#define DCFS_PROTOCOL_EVENTS_H_

// The protocol events: one call for each step of the write-through protocol
// that formal/dcfs.tla models (a mutation's phase 1, its backing syscall and
// its phase 3, a fill's read and commit, an answer served from the cache, a
// sync point, startup recovery, clean shutdown), plus the request frames
// that tell which steps belong to one request. Trace validation
// (formal/Trace.tla) records them and checks that the sequence is a behavior
// of the model; formal/README.md ("Trace validation") maps each event to the
// model action it stands for and the docs/design.md section that describes
// it.
//
// Production code calls them through Context::events, which is
// NoProtocolEvents() (every method does nothing) in every production
// binary: main.cc gets its implementation from MainProtocolEvents(), which
// protocol_events_main.cc defines as the no-op, and which the testonly
// recording build (//dcfs:main_static_traced) links from
// dcfs/testonly/main_recorder.cc instead. Unit tests set Context::events
// themselves.
//
// Where a call goes matters more than what it carries: an event emitted at
// the wrong point would make the trace a behavior of the model while the
// code does something else. Each call site says which model step it marks;
// the rule is that the call comes right after the code that the model's
// step stands for, with no backing syscall (no suspension point, under
// coroutines) in between. Arguments that cost something to build (listings,
// child ids) are passed as callbacks the no-op never calls.
//
// Every method has an empty default body, so an implementation overrides
// only what it records. Methods take the Context so that a recorder can read
// the cache's state at that moment; they must not write to it.

#include <cstdint>
#include <string_view>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {

struct Context;

namespace events {

// A cache::InodeId (not included here: metadata_cache.h includes
// context.h, which includes this file).
using Ino = int64_t;

// The FUSE operation a request frame serves.
enum class Op {
  kOther,
  kLookup,
  kGetattr,
  kSetattr,
  kReadlink,
  kMknod,
  kMkdir,
  kUnlink,
  kRmdir,
  kSymlink,
  kRename,
  kLink,
  kOpen,
  kRead,
  kWrite,
  kFlush,
  kRelease,
  kFsync,
  kOpendir,
  kReaddir,
  kReaddirplus,
  kReleasedir,
  kFsyncdir,
  kStatfs,
  kSetxattr,
  kGetxattr,
  kListxattr,
  kRemovexattr,
  kAccess,
  kCreate,
  kFallocate,
  kCopyFileRange,
  kIoctl,
  kTmpfile,
  // A LINK of an unnamed O_TMPFILE file (DirCacheFS::IsUnnamedTmpfile):
  // to the new parent, a create (the model's "linkcreate", step 23.4).
  kLinkTmpfile,
  // FORGET and BATCH_FORGET: no protocol-event frame is opened for them
  // (they change nothing the model has); only the invariant checks'
  // request frames (dcfs/invariant_checks.h) name them.
  kForget,
  kBatchForget,
};

// A request as it arrived: `ino` is the inode it names (the parent for a
// namespace operation), `name` its name argument; `newparent`/`newname`
// for a rename or link (for a link, `ino` is the source). The views are
// valid until RequestBegin returns.
struct Request {
  Op op = Op::kOther;
  Ino ino = 0;
  std::string_view name;
  Ino newparent = 0;
  std::string_view newname;
  unsigned int flags = 0;  // rename flags; an ioctl's command; setattr's to_set
  int64_t offset = 0;      // readdir offset
  int ioctl_arg = 0;       // an FS_IOC_SETFLAGS's flags (FUSE sends an int)
};

// What a probe of a name read: the object it holds (by its backing inode
// number and birth time, which is what a recorder compares with the cached
// rows), nothing, or a refused mount/subvolume boundary.
struct Probe {
  enum class Kind { kPresent, kAbsent, kRefused };
  Kind kind = Kind::kAbsent;
  uint64_t ino = 0;
  int64_t btime_sec = 0;
  uint32_t btime_nsec = 0;
};

// A file's shared backing descriptor (DirCacheFS::BackingFile), as an open
// or a release left it. For the revalidation model (formal/reval.tla).
struct SharedFd {
  enum class WriteFd { kNone, kPlain, kAppend };
  bool held = false;      // some open of the file is outstanding
  bool writable = false;  // the shared descriptor was opened O_RDWR
  WriteFd write_fd = WriteFd::kNone;  // the write fd beside a read-only one
  int refs = 0;                       // the outstanding opens
  int writable_refs = 0;              // ... that may write
};

// What dcfs keeps for a nodeid, as a step of its lifetime left it. For the
// lifetime model (formal/lifetime.tla).
struct Lifetime {
  enum class Written { kNo, kNoFd, kHeld };
  uint64_t lookups = 0;   // the kernel's lookups dcfs counted (lookups_)
  bool removed = false;   // a removed record answers for it (removed_)
  Written written = Written::kNo;  // written_: no entry, one without or
                                   // with a held descriptor
  int refs = 0;           // its open files (BackingFile::refs)
};
using LifetimeFn = absl::FunctionRef<Lifetime()>;

// A step of a nodeid's lifetime (ProtocolEvents::LifetimeChanged).
enum class LifetimeStep {
  kLookup,    // an entry reply handed it out (arg: unused)
  kCreated,   // a CREATE made it, open (arg: 1 if the open may write)
  kTmpfile,   // a TMPFILE made it, open for writing
  kOpened,    // an OPEN of it succeeded (arg: 1 if it may write)
  kReleased,  // a RELEASE of an open of it ended (arg: 1 if it could write)
  kForgot,    // a FORGET of it (arg: its nlookup)
  kForgotInBatch,  // an entry of a FORGET_MULTI (arg: its nlookup)
  kRemoved,   // phase 3 of an unlink, rmdir or rename that removed one of
              // its names ended (arg: 1 if HoldForRemoval held it)
  kProbed,    // a start probed its recovered row by handle
              // (backing::Startup; arg: 1 if the row went)
};

// What OpenNode found when it reopened a row's handle, for the identity
// model (formal/ident.tla): the row's recorded identity and what the
// reopened object reported, which a recorder compares. 0 is "unknown" for
// a generation and a birth time, as in the cache.
struct IdentityCheck {
  enum class Outcome {
    kServed,       // the object matches the row: served
    kStaleHandle,  // open_by_handle_at failed with ESTALE
    kMismatch,     // it reached another object (VerifyBackingIdentity)
  };
  Outcome outcome = Outcome::kServed;
  uint64_t row_ino = 0;
  uint64_t row_gen = 0;
  int64_t row_btime_sec = 0;
  int64_t row_btime_nsec = 0;
  // The reopened object's (kStaleHandle: none). found_gen is 0 if it was
  // not read: another inode number, or a row without a generation.
  uint64_t found_ino = 0;
  uint64_t found_gen = 0;
  bool found_btime_known = false;
  int64_t found_btime_sec = 0;
  int64_t found_btime_nsec = 0;
};

// The decision LookupOrPopulate takes after reading the cache.
enum class LookupOutcome {
  kFound,     // served from the cache: present
  kNegative,  // served from the cache: absent
  kRefused,   // served from the cache: a refused boundary (EXDEV)
  kResolve,   // unknown in a complete listing: ResolveName
  kPopulate,  // unknown, listing incomplete: PopulateDirectory
};

// Calls `each(name, probe)` for every name a listing read.
using ListingFn = absl::FunctionRef<void(
    absl::FunctionRef<void(std::string_view name, const Probe &probe)> each)>;
// Calls `each(id)` for a set of inode ids.
using IdsFn =
    absl::FunctionRef<void(absl::FunctionRef<void(Ino id)> each)>;

}  // namespace events

class ProtocolEvents {
 public:
  ProtocolEvents() = default;
  ProtocolEvents(const ProtocolEvents &) = delete;
  ProtocolEvents &operator=(const ProtocolEvents &) = delete;
  virtual ~ProtocolEvents() = default;

  // --- Frames ---------------------------------------------------------
  //
  // Which events belong to one request (model: one request slot). Begin
  // and End nest; use the RAII scopes below. Each End gets the status the
  // frame returned (OK unless the code told its scope otherwise, with
  // Finish): a recorder ends a request that failed differently from one
  // that succeeded without the steps it should have taken.

  // A FUSE request (fuse_ops.cc), from dispatch to reply. The status is
  // what was replied (OK also for a reply sent by the handler itself).
  virtual void RequestBegin(Context &ctx, const events::Request &request) {}
  virtual void RequestEnd(Context &ctx, const absl::Status &status) {}

  // DirCacheFS::FreshAttr: the attributes of `id` were read from the cache
  // and are `valid` (served) or not (refreshed: RefreshBegin follows).
  // Model: Arrive of a getattr (GAFrom), or Readdirplus's check of "."'s
  // attributes (part of RDFrom).
  virtual void GetattrBegin(Context &ctx, events::Ino id, bool valid) {}
  virtual void GetattrEnd(Context &ctx, const absl::Status &status) {}

  // backing::LookupOrPopulate(parent, name). Model: a LookupOrPopulate
  // (LK and what follows): a lookup request's, an unlink's or a rename's
  // resolve, or a failed mutation's re-resolve.
  virtual void LookupBegin(Context &ctx, events::Ino parent,
                           std::string_view name) {}
  virtual void LookupEnd(Context &ctx, const absl::Status &status) {}

  // backing::RefreshAttrs/RefreshAttrsFromFd of `id`, from its fill
  // snapshot (taken right after this call) to its end. Model: the statx and
  // the fill that end a getattr, a readdirplus or a mutation (GA_stat ...
  // R_fill).
  virtual void RefreshBegin(Context &ctx, events::Ino id) {}
  virtual void RefreshEnd(Context &ctx, const absl::Status &status) {}

  // backing::SyncBacking (a sync point), from before cache::BeginSync to
  // its return. Model: a sync request (S1, S2), or StopSync/StopClear
  // inside FinishRun.
  virtual void SyncBegin(Context &ctx) {}
  virtual void SyncEnd(Context &ctx, const absl::Status &status) {}

  // --- Reading the cache and the backing filesystem ------------------

  // LookupOrPopulate read the cache and decided (`listed`: whether the
  // listing was complete, for an unknown name). Model: LookupStep (or the
  // Arrive of a lookup, unlink or rename): serve, ResolveProbe next, or
  // PopulateRead next. `child` is the cached row for kFound.
  virtual void LookupDecided(Context &ctx, events::Ino parent,
                             std::string_view name,
                             events::LookupOutcome outcome,
                             events::Ino child) {}

  // ResolveName's probe of `name` read what it holds (ProbeChild, right
  // after its openat and statx: the read; the identity cannot change after
  // it). Model: ResolveProbe (the fill snapshot and the probe).
  virtual void ResolveProbed(Context &ctx, events::Ino parent,
                             std::string_view name,
                             const events::Probe &probe) {}

  // ResolveName's transaction committed: the name was recorded iff
  // `recorded` (cache::CanFill of `parent`). `snapshot` is the fill
  // snapshot (FillSnapshot::seq). Model: ResolveCommit.
  virtual void ResolveCommitted(Context &ctx, events::Ino parent,
                                std::string_view name, uint64_t snapshot,
                                bool recorded) {}

  // Inside a listing's or a resolve's transaction (RecordChild): the row of
  // `child`, an entry of `dir`, was upserted, its attributes recorded as
  // current iff `filled` (the code's own decision: CanFill, and not open
  // for writing). PopulateCommitted or ResolveCommitted follows once the
  // transaction committed. Model: a whole getattr fill of `child`.
  virtual void ChildRowRecorded(Context &ctx, events::Ino dir,
                                events::Ino child, bool filled) {}

  // PopulateDirectory took its fill snapshot and the directory's epoch;
  // its reads (getdents64, then a probe of every name) follow. Model: where
  // PopulateRead takes place (see PopulateRead below).
  virtual void PopulateStarted(Context &ctx, events::Ino dir) {}

  // PopulateDirectory's phase A read the whole listing (getdents64 and the
  // probe of every name). Model: PopulateRead, at PopulateStarted: the
  // model reads the listing in one step, the code over several syscalls, at
  // which other requests may run (formal/README.md, "Trace validation").
  virtual void PopulateRead(Context &ctx, events::Ino dir,
                            events::ListingFn listing) {}

  // PopulateDirectory's phase B committed; the listing was recorded iff
  // `recorded`. Model: PopulateCommit.
  virtual void PopulateCommitted(Context &ctx, events::Ino dir,
                                 uint64_t snapshot, bool recorded) {}

  // DirCacheFS::ListCached checked whether `dir`'s listing can be served
  // (cache::IsDirComplete); if `complete`, the listing it serves was taken
  // right after. Model: ReaddirStep (or the Arrive of a readdir).
  virtual void ListChecked(Context &ctx, events::Ino dir, bool complete) {}

  // A refresh's statx of `id` returned. Model: GetattrStat,
  // ReaddirplusStat, CreateStat, UnlinkStat, RenameStat.
  virtual void AttrsStatted(Context &ctx, events::Ino id) {}

  // backing::FillAttrs committed; the attributes were recorded iff
  // `recorded` (CanFill). Model: GetattrFill, ReaddirplusFill, CreateFill,
  // UnlinkFill, RenameFill.
  virtual void AttrsFilled(Context &ctx, events::Ino id, bool recorded) {}

  // backing::ParentOf(dir) took the fill snapshot for the parent row it is
  // about to record (no model step: where the whole getattr's snapshot is).
  virtual void ParentLookupStarted(Context &ctx, events::Ino dir) {}
  // backing::ParentOf(dir) recorded the parent row `parent` from the backing
  // filesystem, its attributes as current iff `filled` (the code's own
  // decision). `snapshot` is its fill snapshot. Model: a whole getattr
  // fill of that directory.
  virtual void ParentRecorded(Context &ctx, events::Ino dir,
                              events::Ino parent, uint64_t snapshot,
                              bool filled) {}

  // backing::InitRoot recorded the root's identity and attributes (fresh,
  // at startup). Model: a whole getattr fill of the root.
  virtual void RootRecorded(Context &ctx) {}

  // --- Mutations ------------------------------------------------------

  // cache::BeginMutation committed phase 1 for `ids` (with a WAL fsync iff
  // `synced`) and registered the mutation. Model: the create's Arrive
  // (C1From), UnlinkPhase1, RenamePhase1 (each: the BeginMutation branch).
  virtual void MutationBegun(Context &ctx, events::IdsFn ids, bool synced) {}

  // BeginRemove's or BeginRename's verification failed: nothing was
  // written, no mutation began (the caller resolves again, or gives up
  // with EAGAIN). Model: the retry or EAGAIN branch of UnlinkPhase1 or
  // RenamePhase1.
  virtual void MutationAborted(Context &ctx, events::IdsFn ids) {}

  // The phase-2 syscall of the request's mutation is about to be issued
  // (no model step: a recorder checks that phase 1 has begun).
  virtual void MutationSyscallStarting(Context &ctx) {}

  // The phase-2 syscall of the request's mutation returned. Model:
  // CreateSyscall, UnlinkSyscall, RenameSyscall.
  virtual void MutationSyscall(Context &ctx, const absl::Status &status) {}

  // RecordNewChild probed the created name (right after its openat and
  // statx, or the openat's ENOENT). Model: CreateProbe.
  virtual void NewChildProbed(Context &ctx, events::Ino parent,
                              std::string_view name,
                              const events::Probe &probe) {}

  // cache::Mutation::End, about to change the guards: `owns(id)` is what
  // Mutation::Owns says now (what the phase 3 just committed went by). No
  // model step of its own; MutationEnded follows.
  virtual void MutationEnding(Context &ctx, events::IdsFn ids,
                              absl::FunctionRef<bool(events::Ino)> owns) {}
  // cache::Mutation::End changed the guards: the mutation of `ids` is no
  // longer in flight. Model: CreatePhase3, UnlinkPhase3, RenamePhase3
  // (commit, End and the refresh's snapshot), or CreateFailed,
  // UnlinkFailed, RenameFailed.
  virtual void MutationEnded(Context &ctx, events::IdsFn ids) {}

  // RemoveChild or Rename resolved the (source) name: `found` or not
  // (ENOENT). Model: UnlinkPhase1's ENOENT branch, RenameResolveDst.
  virtual void NameResolved(Context &ctx, events::Ino parent,
                            std::string_view name, bool found) {}

  // DirCacheFS::ReresolveAfterFailure is about to resolve `name` again.
  // Model: RenameFailed2 for a rename's destination; nothing otherwise.
  virtual void Reresolve(Context &ctx, events::Ino parent,
                         std::string_view name) {}

  // cache::EndWrites: the writes through `id`'s writable opens ended (a
  // guard event, like Mutation::End). Not modelled (writable opens are a
  // file's, see formal/README.md); recorded so a trace shows it.
  virtual void WritesEnded(Context &ctx, events::Ino id) {}

  // --- Files: the revalidation model (formal/reval.tla) --------------
  //
  // Not the main model's: a file's shared backing descriptor, its access
  // mode and its write descriptor, which reval.tla models together with
  // the flag and mode changes that should make dcfs ask the backing
  // filesystem again. A recorder projects these, with the SETATTR and
  // IOCTL request frames of the same file, onto one trace per file.

  // DirCacheFS::Open of file `id` with open flags `flags`, or the open a
  // Create or Tmpfile makes (only when it succeeds), ended with `status`
  // (OK: the open was granted; Open refuses with EPERM where the backing
  // file's flags do). `shared`: a shared backing fd existed when it began;
  // `after`: the shared backing fd now. Model: OpenF.
  virtual void FileOpened(Context &ctx, events::Ino id, int flags,
                          bool shared, const absl::Status &status,
                          const events::SharedFd &after) {}
  // DirCacheFS::Release of an open of `id` (one that may write iff
  // `writable`) ended; `after`: the shared backing fd now. Model: ReleaseF.
  virtual void FileReleased(Context &ctx, events::Ino id, bool writable,
                            const events::SharedFd &after) {}

  // --- Nodeids: the lifetime model (formal/lifetime.tla) ------------
  //
  // Not the main model's: the kernel's lookup counts as dcfs counts them,
  // and what dcfs keeps for a nodeid (its row, a removed record, the
  // written_ entry and its held descriptor, open files) at each step that
  // changes them. A recorder projects these onto one trace per nodeid.

  // DirCacheFS (or, for kProbed, backing::Startup) finished `step` of
  // nodeid `id` (see events::LifetimeStep; `arg` is the step's), right
  // after the code the model's step stands for; `after` reads what dcfs
  // keeps for it now. Model: Lookup, Create, Tmpfile, Link (a kLookup of a
  // LINK), Open, Release, Remove and Settle (kRemoved), Forget,
  // ForgetMulti, and Restart's probe (kProbed).
  virtual void LifetimeChanged(Context &ctx, events::Ino id,
                               events::LifetimeStep step, uint64_t arg,
                               events::LifetimeFn after) {}
  // DirCacheFS::Destroy let go of every nodeid (the kernel sends no
  // FORGETs at unmount). Model: Destroy.
  virtual void Destroyed(Context &ctx) {}

  // --- Identity: the identity model (formal/ident.tla) ----------------
  //
  // Not the main model's: what a reopen of nodeid `id`'s handle reached
  // (backing::OpenNode), right after the decision and before the row of a
  // stale or mismatched one is forgotten. Model: the resolution an Access
  // or a request takes (Resolve), and IdentTrace.tla's resolve step.
  virtual void IdentityResolved(Context &ctx, events::Ino id,
                                const events::IdentityCheck &check) {}

  // --- Sync points ----------------------------------------------------

  // cache::BeginSync took its snapshot; the syncfs calls follow. Model: a
  // sync request's Arrive (S1From), or StopSync.
  virtual void SyncSnapshotTaken(Context &ctx) {}
  // The syncfs calls are about to be issued (no model step: a recorder
  // checks that the snapshot came right before, with nothing between).
  virtual void SyncfsStarting(Context &ctx) {}
  // Every syncfs returned. Model: nothing (the model's syncfs takes effect
  // at S1, which only allows more crash outcomes).
  virtual void SyncfsDone(Context &ctx) {}
  // cache::ClearDirty committed. Model: SyncClearDirty, or StopClear.
  virtual void SyncCleared(Context &ctx) {}

  // --- Startup and shutdown -------------------------------------------

  // backing::StartRun is about to recover (the database is as the last run,
  // or the crash, left it). Model: Crash (if the last run did not shut down
  // cleanly) and Restart.
  virtual void RunStarting(Context &ctx) {}
  // cache::RecoverDirty committed. Model: Recover.
  virtual void Recovered(Context &ctx) {}
  // StartRun's last transaction (clean_shutdown = 0) committed. Model:
  // StartRun.
  virtual void RunStarted(Context &ctx) {}
  // backing::FinishRun began (no request is in flight). Model:
  // BeginShutdown.
  virtual void ShutdownBegin(Context &ctx) {}
  // FinishRun's checkpoint completed. Model: StopCkpt.
  virtual void Checkpointed(Context &ctx) {}
  // FinishRun's clean_shutdown = 1 committed. Model: StopFlag.
  virtual void CleanShutdownRecorded(Context &ctx) {}

  // --- Steps the model does not have ---------------------------------

  // backing::ReconcileAttrs adopted an out-of-band change of `id`.
  virtual void OutOfBandChange(Context &ctx, events::Ino id) {}
  // cache::InvalidateInode/DeleteInode is about to delete `id`'s row,
  // inside its transaction (which may be nested in a caller's): every
  // dentry that points at it will become unknown.
  virtual void InodeForgetting(Context &ctx, events::Ino id) {}
  // ... and deleted it (the caller's transaction, if any, has not
  // committed yet).
  virtual void InodeForgotten(Context &ctx, events::Ino id) {}
};

// The implementation every production Context uses: records nothing. A
// function-local static, but a stateless one (no members, every method
// empty): nothing is shared through it, which is why it may be one object
// for every Context.
inline ProtocolEvents &NoProtocolEvents() {
  static ProtocolEvents none;
  return none;
}

// The implementation main.cc installs: NoProtocolEvents() in production
// (protocol_events_main.cc), a recorder in the testonly recording build
// (dcfs/testonly/main_recorder.cc). Link-time selection: a binary links
// exactly one of the two.
ProtocolEvents &MainProtocolEvents();

namespace events {

// RAII frames (see ProtocolEvents' frame methods).

class RequestScope {
 public:
  RequestScope(ProtocolEvents &ev, Context &ctx, const Request &request)
      : ev_(ev), ctx_(ctx) {
    ev_.RequestBegin(ctx_, request);
  }
  RequestScope(const RequestScope &) = delete;
  RequestScope &operator=(const RequestScope &) = delete;
  ~RequestScope() { ev_.RequestEnd(ctx_, status_); }

  // Records the status the request replied, and returns it.
  absl::Status Finish(absl::Status status) {
    status_ = status;
    return status;
  }

 private:
  ProtocolEvents &ev_;
  Context &ctx_;
  absl::Status status_;
};

// Calls (ev.*begin)(ctx, args...) now and (ev.*end)(ctx, status) at scope
// exit, where status is what Finish was given (OK if it never was).
class Scope {
 public:
  template <typename... BeginArgs, typename... Args>
  Scope(ProtocolEvents &ev, Context &ctx,
        void (ProtocolEvents::*begin)(Context &, BeginArgs...),
        void (ProtocolEvents::*end)(Context &, const absl::Status &),
        Args &&...args)
      : ev_(ev), ctx_(ctx), end_(end) {
    (ev_.*begin)(ctx_, static_cast<Args &&>(args)...);
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  ~Scope() { (ev_.*end_)(ctx_, status_); }

  // Records the frame's result, and returns it.
  absl::Status Finish(absl::Status status) {
    status_ = status;
    return status;
  }
  template <typename T>
  absl::StatusOr<T> Finish(absl::StatusOr<T> result) {
    status_ = result.status();
    return result;
  }

 private:
  ProtocolEvents &ev_;
  Context &ctx_;
  void (ProtocolEvents::*end_)(Context &, const absl::Status &);
  absl::Status status_;
};

}  // namespace events
}  // namespace dcfs

#endif  // DCFS_PROTOCOL_EVENTS_H_
