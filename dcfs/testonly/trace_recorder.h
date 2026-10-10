#ifndef DCFS_TESTONLY_TRACE_RECORDER_H_
#define DCFS_TESTONLY_TRACE_RECORDER_H_

// The recording implementation of ProtocolEvents (dcfs/protocol_events.h),
// for trace validation (formal/Trace.tla, formal/README.md "Trace
// validation"). Test-only: production binaries link the no-op.
//
// What it writes. The model (formal/dcfs.tla) describes one directory D;
// the recorder projects the run onto every directory at once: each line
// belongs to one directory's trace, and each directory's trace is checked
// against the model on its own. A line is
//
//   DCFS-TRACE <trace> <dir> <json>
//
// where <trace> names the run (a test's name, or "e2e" for a guest boot,
// whose daemon processes all append to one trace), <dir> is the directory's
// inode id, and <json> one event of that directory's trace. Every line
// carries the directory's cached state after the event ("db": its dentries,
// completeness and epoch, whether its attributes are valid, its dirty row,
// the clean-shutdown flag, whether it is in Context::dirty.durable, and its
// FillGuards::inflight count), so that the model's state is compared with
// the code's after every step, not just its actions. A line whose state is
// the same as its trace's previous line leaves it out, and the host puts
// it back (formal/trace_validate.sh).
//
// How the projection works (formal/README.md explains why it is sound):
//  - A request touching D (a FUSE request, or a sync point, getattr,
//    lookup or refresh inside one) is one request slot of D's trace, "p1",
//    "p2", ...: the smallest slot no open request of D holds.
//  - Each event becomes a line of every directory it concerns, as the
//    model action it stands for in that directory's terms.
//  - A step the model does not have (an out-of-band change, a refused
//    boundary, a cross-directory rename, a link, a setattr or xattr change
//    of the directory itself, a syscall error the model does not know, a
//    request that failed half-way) ends that directory's trace with a
//    "cut" line saying why: what came before is still checked.
//  - After every event the recorder checks that no other directory's
//    cached state changed. If one did, its trace gets an "unexplained"
//    line, which no model action matches, so validation fails right there.
//  - Directory rows created by an event (a mkdir, a listing that finds a
//    subdirectory) begin a trace ("begin" line with the state they start
//    in); a directory row that disappears ends it ("gone").
//
// Files, for the revalidation model (formal/reval.tla, formal/README.md
// "The revalidation model"), if the recorder was made with `files`: each
// file whose trace began (at an open that found no shared backing fd, or a
// create) gets its own trace, on lines
//
//   DCFS-REVAL <trace> <file> <json>
//
// one per open (FileOpened), release (FileReleased), FS_IOC_SETFLAGS,
// FS_IOC_FSSETXATTR or FS_IOC_GETFLAGS ioctl, SETATTR of the mode or owner,
// and write dcfs makes itself (WRITE, FALLOCATE, COPY_FILE_RANGE into it),
// and an "oob" line where a test says it changed the backing file behind
// dcfs's back (NoteOutOfBand). Each open and release line carries the
// shared backing fd it left; the model's state is compared with it.
//
// Nodeids, for the lifetime model (formal/lifetime.tla, formal/README.md
// "The lifetime model"), if the recorder was made with `lifetimes`: each
// nodeid whose trace began (at the entry reply dcfs counts first, or at the
// CREATE or TMPFILE that made it) gets its own trace, on lines
//
//   DCFS-LIFE <trace> <nodeid> <json>
//
// one per step of LifetimeChanged (lookup, create, tmpfile, open, release,
// forget, removed, probe), and one at DESTROY, at a start after a crash or a
// clean shutdown, and when a start has run (its sweep). Each carries what
// dcfs keeps for the nodeid after the step ("st": its lookup count,
// removed record, written_ entry and open files as DirCacheFS reported
// them, and its row and the row's nlink as the database has them; at the
// run's lines only the row); the model's state is compared with it.
// Stubs' nodeids are not traced.
//
// Nodeids' identities, for the identity model (formal/ident.tla,
// formal/README.md "The identity model"), if the recorder was made with
// `identities`: each nodeid whose trace began (at the entry reply dcfs
// counts first) gets its own trace, on lines
//
//   DCFS-IDENT <trace> <nodeid> <json>
//
// one per entry reply (with the generation its row has, as a string), per
// FORGET (the lookups left), per reopen of its handle (IdentityResolved:
// the outcome, and whether the inode number, generation and birth time of
// what it reached are the row's, another, or unknown), when its row goes
// (InodeForgotten), and at DESTROY and a start. Each carries whether its
// row exists. Stubs' nodeids are not traced.
//
// Not thread-safe: dcfs serves one request at a time.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "dcfs/context.h"
#include "dcfs/protocol_events.h"

namespace dcfs::testonly {

class TraceRecorder final : public ProtocolEvents {
 public:
  // Writes lines to `fd` (not owned; each line is one write(2)) under the
  // trace name `trace` (no spaces); with `files`, the files' traces too;
  // with `lifetimes`, the nodeids' traces; with `identities`, the nodeids'
  // identity traces; without `directories`, no directory's trace (for a
  // scenario of the other models whose steps would cut them).
  TraceRecorder(int fd, std::string trace, bool files = false,
                bool lifetimes = false, bool identities = false,
                bool directories = true);

  // Begins the trace of every directory now in the cache. For a test that
  // sets Context::events itself, once its setup is done and nothing is in
  // flight; the daemon's recorder does it when StartRun completes.
  void BeginAll(Context &ctx);

  void RequestBegin(Context &ctx, const events::Request &request) override;
  void RequestEnd(Context &ctx, const absl::Status &status) override;
  void Replied(Context &ctx, int errnum) override;
  void GetattrBegin(Context &ctx, events::Ino id, bool valid) override;
  void GetattrEnd(Context &ctx, const absl::Status &status) override;
  void LookupBegin(Context &ctx, events::Ino parent,
                   std::string_view name) override;
  void LookupEnd(Context &ctx, const absl::Status &status) override;
  void LookupAnswered(Context &ctx, events::Ino parent, std::string_view name,
                      events::LookupOutcome answer, events::Ino child) override;
  void RefreshBegin(Context &ctx, events::Ino id) override;
  void RefreshEnd(Context &ctx, const absl::Status &status) override;
  void SyncBegin(Context &ctx) override;
  void SyncEnd(Context &ctx, const absl::Status &status) override;

  void LookupDecided(Context &ctx, events::Ino parent, std::string_view name,
                     events::LookupOutcome outcome, events::Ino child) override;
  void ResolveProbed(Context &ctx, events::Ino parent, std::string_view name,
                     const events::Probe &probe) override;
  void ResolveCommitted(Context &ctx, events::Ino parent, std::string_view name,
                        uint64_t snapshot, bool recorded) override;
  void ChildRowRecorded(Context &ctx, events::Ino dir, events::Ino child,
                        bool filled, bool created) override;
  void PopulateStarted(Context &ctx, events::Ino dir) override;
  void PopulateRead(Context &ctx, events::Ino dir,
                    events::ListingFn listing) override;
  void PopulateCommitted(Context &ctx, events::Ino dir, uint64_t snapshot,
                         bool recorded) override;
  void ListChecked(Context &ctx, events::Ino dir, bool complete) override;
  void AttrsStatted(Context &ctx, events::Ino id) override;
  void AttrsFilled(Context &ctx, events::Ino id, bool recorded) override;
  void ParentLookupStarted(Context &ctx, events::Ino dir) override;
  void ParentRecorded(Context &ctx, events::Ino dir, events::Ino parent,
                      uint64_t snapshot, bool filled) override;
  void RootRecorded(Context &ctx) override;

  void MutationBegun(Context &ctx, events::IdsFn ids, bool synced) override;
  void MutationAborted(Context &ctx, events::IdsFn ids) override;
  void MutationSyscallStarting(Context &ctx) override;
  void MutationSyscall(Context &ctx, const absl::Status &status) override;
  void NewChildProbed(Context &ctx, events::Ino parent, std::string_view name,
                      const events::Probe &probe) override;
  void MutationEnding(Context &ctx, events::IdsFn ids,
                      absl::FunctionRef<bool(events::Ino)> owns) override;
  void MutationEnded(Context &ctx, events::IdsFn ids) override;
  void NameResolved(Context &ctx, events::Ino parent, std::string_view name,
                    bool found) override;
  void Reresolve(Context &ctx, events::Ino parent,
                 std::string_view name) override;
  void WritesEnded(Context &ctx, events::Ino id) override;

  void SyncSnapshotTaken(Context &ctx) override;
  void SyncfsStarting(Context &ctx) override;
  void SyncfsDone(Context &ctx) override;
  void SyncCleared(Context &ctx) override;

  void RunStarting(Context &ctx) override;
  void Recovered(Context &ctx) override;
  void RunStarted(Context &ctx) override;
  void RecoveryDone(Context &ctx) override;
  void ShutdownBegin(Context &ctx) override;
  void Checkpointed(Context &ctx) override;
  void CleanShutdownRecorded(Context &ctx) override;

  void OutOfBandChange(Context &ctx, events::Ino id) override;
  void InodeForgetting(Context &ctx, events::Ino id) override;
  void InodeForgotten(Context &ctx, events::Ino id) override;

  void FileOpened(Context &ctx, events::Ino id, int flags, bool shared,
                  const absl::Status &status,
                  const events::SharedFd &after) override;
  void FileReleased(Context &ctx, events::Ino id, bool writable,
                    const events::SharedFd &after) override;

  void Interrupted(Context &ctx) override;

  void LifetimeChanged(Context &ctx, events::Ino id, events::LifetimeStep step,
                       uint64_t arg, events::LifetimeFn after) override;
  void Destroyed(Context &ctx) override;
  void IdentityResolved(Context &ctx, events::Ino id,
                        const events::IdentityCheck &check) override;

  // For a test: the backing file `id` changed behind dcfs's back (a flag or
  // mode change made directly on the backing filesystem). Its trace, if
  // one began, gets an "oob" line: the model's out-of-band change.
  void NoteOutOfBand(events::Ino id);

 private:
  using Ino = events::Ino;

  // One request slot of one directory's trace.
  struct Req {
    int slot = 0;
    std::string kind;              // a request kind of the model (AllKinds)
    std::string n, m;              // its names (escaped), if any
    bool arrived = false;          // its first line (the model's Arrive) is out
    bool terminal = false;         // the model's request has replied
    bool expects_refresh = false;  // a refresh of the directory is its own
    bool begun = false;            // a mutation's phase 1 committed
    bool syscall_seen = false;     // ... its phase-2 syscall returned
    bool syscall_ok = false;
    bool probe_absent = false;  // a create's probe found nothing
    bool owned = false;         // Mutation::Owns at its End
    bool interrupted = false;   // a checkpoint found it interrupted
    // A lookup's: what LookupOrPopulate answered (JSON: "neg", "refused" or
    // the object's key), for its reply line.
    std::string answer;
    // Why its trace ends at a point whose kind (a cut if the request fails,
    // "unexplained" if it replies OK) its end decides (Defer).
    std::string pending;
  };

  // A frame: a FUSE request, or a getattr, lookup, refresh or sync point
  // running inside one (or on its own).
  struct Frame {
    enum class Kind { kRequest, kGetattr, kLookup, kRefresh, kSync };
    Kind kind = Kind::kRequest;
    // kRequest: the request.
    events::Op op = events::Op::kOther;
    Ino ino = 0;
    Ino newparent = 0;
    std::string name, newname;  // raw bytes
    unsigned int flags = 0;
    int64_t offset = 0;
    int ioctl_arg = 0;
    // kRequest: the errno its reply carried (Replied), once sent.
    std::optional<int> sent_errno;
    // kGetattr, kRefresh: the inode; kLookup: the parent and the name.
    Ino id = 0;
    std::string lookup_name;
    // kRefresh: of valid attributes, outside any request that expects it:
    // nothing the model can see, so no lines; but its fill may record only
    // if no mutation line of the directory came since (`mark`: the
    // directory's mutation_lines at RefreshBegin).
    bool silent = false;
    int64_t mark = 0;
    // kRequest: the object (key) each (directory, raw name) resolved to,
    // as its lookups, probes and listings last reported.
    std::map<std::pair<Ino, std::string>, std::string> resolved;
    // kSync: the callback count at its snapshot (SyncSnapshotTaken).
    int64_t snapshot_at = -1;
    // The requests of directories' traces this frame owns.
    std::map<Ino, Req> reqs;
  };

  // What a FUSE request is to directory `dir`'s trace.
  struct Mapping {
    enum class Kind { kNone, kRequest, kUnmodelled };
    Kind kind = Kind::kNone;
    std::string req_kind, n, m;
    std::string why;  // kUnmodelled
  };

  // A directory's cached state, as a line's "db" spells it: its dentries
  // (raw name, then the value: "unknown", "absent", "refused" or a key),
  // and the rest of the object, already spelled.
  struct State {
    std::vector<std::pair<std::string, std::string>> dent;
    std::string rest;
    std::string Json() const;
  };

  struct Dir {
    std::string last;     // the state in its last line
    State last_state;     // the same, as a State
    bool dead = false;    // cut, unexplained or gone: no more lines
    std::set<int> slots;  // held by open requests
    // A population between PopulateStarted and PopulateRead: its line goes
    // where it started (the model's PopulateRead), so the lines written
    // meanwhile (by requests that ran during its reads) wait here.
    bool reading = false;
    int read_slot = 0;
    std::string read_db;  // the state when it started
    std::vector<std::string> held;
    // Its trace ends at a request's end (Defer): lines meanwhile are
    // dropped, and its state is not checked.
    bool pending = false;
    // Its phase1 (begun) and end lines so far: the mutations of it the
    // trace has seen begin or end.
    int64_t mutation_lines = 0;
    // mutation_seq_ at the last of them, and how many of its mutations have
    // begun and not ended.
    int64_t last_mutation = 0;
    int open_mutations = 0;
  };

  static Mapping Map(const Frame &request, Ino dir);
  // The traced directories a FUSE request mutates as a modelled request.
  std::vector<Ino> MutatedDirs(const Frame &request);
  // The key of `id`'s row ("" if none).
  std::string KeyOf(Context &ctx, Ino id);
  // Notes in the innermost request that (dir, name) resolved to `key`.
  void Resolved(Context &ctx, Ino dir, std::string_view name, std::string key);
  // A mutation names `dir` as an object in request `rf`: a dir-itself cut
  // if `rf` resolved one of its names to `dir`, else unexplained.
  void ItselfOrUnexplained(Context &ctx, const Frame &rf, Ino dir);
  // Starts handling a callback: names it for the lines, and counts it.
  void Enter(const char *cause);

  // The innermost FUSE request frame, or null.
  Frame *InnermostRequest();
  // The request of `dir`'s trace that the innermost frames (up to and
  // including the innermost FUSE request) own, or null; `owner` is set to
  // its frame.
  Req *Find(Ino dir, Frame **owner = nullptr);
  // The request of `dir`'s trace that the innermost FUSE request owns, or
  // null.
  Req *RequestReq(Ino dir);
  // The innermost sync point's frame if it took its snapshot (and has not
  // ended), outside a shutdown; null otherwise.
  Frame *SyncPastSnapshot();
  // Opens a request of `dir`'s trace owned by `frame`.
  Req &Open(Frame &frame, Ino dir, std::string kind, std::string n = "",
            std::string m = "");
  // Closes `frame`: frees its slots and ends each of its requests with a
  // "reply" line (with the errno the request sent, or else the one
  // `status` carries, 0 if OK, and a lookup's answer), or with a cut if
  // `unmodelled_end(req)` names a reason
  // (an end the model does not have), or an "unexplained" line if the
  // reason starts with "unexplained: ".
  void Close(Context &ctx, Frame &frame, const absl::Status &status,
             absl::FunctionRef<std::string(const Req &)> unmodelled_end);
  // Ends `dir`'s trace at this point, as Close of `req`'s frame decides.
  void Defer(Ino dir, Req &req, std::string why);

  bool Traced(Ino dir) const;
  std::string Snapshot(Context &ctx, Ino dir);
  State SnapshotState(Context &ctx, Ino dir);
  // Writes one line of `dir`'s trace: {"ev": ev, "p": ..., fields..., "db":
  // the state now}. `fields` is "" or starts with ",".
  void Emit(Context &ctx, Ino dir, Req *req, std::string_view ev,
            std::string_view fields = "");
  // Ends `dir`'s trace at a step the model does not have. `why` is
  // "<category>: <detail>"; formal/trace_validate.sh fails a test whose
  // traces end at a category it does not allow.
  void Cut(Context &ctx, Ino dir, std::string_view why);
  // A line no model action matches: validation fails here.
  void Unexplained(Context &ctx, Ino dir, std::string_view why);
  // Drops `dir`'s held lines (see Dir::reading) before its trace ends.
  void EndHeld(Ino dir);
  // How a frame that ends with `status` ends its requests (see Close).
  static std::string FrameEnd(std::string_view what,
                              const absl::Status &status);
  void Write(Ino dir, const std::string &json);
  // After every callback outside a transaction: begins the traces of new
  // directories, ends those of deleted ones, and gives every other
  // directory whose state changed an "unexplained" line, or a cut if the
  // change is exactly that of a forgotten inode (forgotten_).
  void After(Context &ctx);
  // How a directory row first seen at this callback came to be (a begin
  // line's "origin": existing, mkdir, listing, parent, or other).
  std::string Origin();
  std::vector<Ino> AllDirs(Context &ctx);
  // A run's line (restart, recover, start_run; written for every
  // directory, its trace's or not) carried `dir`'s state now: the state
  // its next line is compared with (After). A start in the same process
  // as a clean shutdown changes the clean flag there.
  void RunLineWritten(Context &ctx, Ino dir);

  int fd_;
  std::string trace_;
  std::vector<Frame> frames_;
  std::map<Ino, Dir> dirs_;
  bool shutdown_ = false;
  int64_t line_ = 0;
  const char *cause_ = "";  // the callback being handled, for the lines
  int64_t callbacks_ = 0;   // callbacks handled so far
  std::set<Ino> covered_;   // directories given a line by this callback
  // Directory -> raw names whose rows pointed at an inode forgotten since
  // the last check (InodeForgetting).
  std::map<Ino, std::set<std::string>> forgotten_;
  // Directory -> the child rows (child, filled) its listing or resolve
  // recorded in the transaction not yet reported committed.
  std::map<Ino, std::vector<std::pair<Ino, bool>>> child_rows_;
  // The keys of the inodes in the dirty set before recovery (RunStarting).
  std::vector<std::string> dirty_keys_;
  // Every phase1 (begun) and end line so far, of any directory.
  int64_t mutation_seq_ = 0;
  // mutation_seq_ at the snapshot event of the fill in progress: of a
  // listing or resolve of a directory (PopulateStarted, LookupDecided
  // kResolve), by that directory; of ParentOf(dir), by dir.
  std::map<Ino, int64_t> listing_marks_;
  std::map<Ino, int64_t> parent_marks_;
  // The directory whose listing or resolve just committed (0 otherwise),
  // while After() writes the begin lines of the rows it inserted: their
  // "parent_marked" (step 23.11).
  Ino filling_dir_ = 0;
  // Writes the child_fill lines of `dir`'s listing or resolve that took
  // `snapshot`.
  void ChildFills(Context &ctx, Ino dir);
  // A child_fill line for `dir` (the code's decision `filled`), or an
  // "unexplained" one if it filled although the trace saw a mutation of
  // `dir` begin or end since `mark` (mutation_seq_ at the fill's snapshot
  // event), or one in flight; mark < 0: no snapshot event was seen.
  void Fill(Context &ctx, Ino dir, int64_t mark, bool filled);
  // Notes a phase1 (begun) or end line of `dir`.
  void MutationLine(Ino dir, bool begun);

  // The files' traces (formal/reval.tla).
  struct FileTrace {
    bool dead = false;  // cut: no more lines
  };
  bool files_enabled_ = false;
  bool lifetimes_enabled_ = false;
  bool identities_enabled_ = false;
  bool directories_enabled_ = true;
  std::map<Ino, FileTrace> files_;
  int64_t file_line_ = 0;
  // Whether `id` has a trace that still takes lines.
  bool FileTraced(Ino id) const;
  // Writes one line of file `id`'s trace: {"i", "c", "ev": ev, fields...}.
  // `fields` is "" or starts with ",".
  void FileLine(Ino id, std::string_view ev, std::string_view fields = "");
  // Ends file `id`'s trace at a step the model does not have.
  void FileCut(Ino id, std::string_view why);
  // The file lines of a FUSE request that ended with errno `err`.
  void FileRequestEnd(const Frame &request, int err);
  // Writes `line` (one write(2) per call).
  void WriteLine(const std::string &line);

  // The nodeids' traces (formal/lifetime.tla): those that began.
  std::set<Ino> lives_;
  int64_t life_line_ = 0;
  // Writes one line of nodeid `id`'s trace: {"i", "c": cause, "ev": ev,
  // fields...}. `fields` is "" or starts with ",".
  void LifeLine(Ino id, std::string_view cause, std::string_view ev,
                std::string_view fields = "");
  // The lines of the run's steps (crash, restart, start, destroy) for every
  // nodeid trace, each with the row as the database has it.
  void LifeRunLines(Context &ctx, std::string_view cause, std::string_view ev);
  // `"row":..,"nl0":..`: whether `id` has a row, and whether its nlink
  // column is 0.
  static std::string RowJson(Context &ctx, Ino id);

  // The nodeids' identity traces (formal/ident.tla): those that began.
  std::set<Ino> idents_;
  int64_t ident_line_ = 0;
  // Writes one line of nodeid `id`'s identity trace: {"i", "c": cause,
  // "ev": ev, fields..., "st": {"row": whether it has a row now, more}}.
  // `fields` and `more` are "" or start with ",".
  void IdentLine(Context &ctx, Ino id, std::string_view cause,
                 std::string_view ev, std::string_view fields = "",
                 std::string_view more = "");
  // The identity lines of a step of LifetimeChanged: an entry reply or a
  // FORGET (the trace begins at the reply dcfs counts first).
  void IdentStep(Context &ctx, Ino id, events::LifetimeStep step,
                 const events::Lifetime &kept);
  // The lines of the run's steps (crash, restart, start, destroy) for
  // every identity trace.
  void IdentRunLines(Context &ctx, std::string_view cause, std::string_view ev);
  // Whether `id` has a row in inodes.
  static bool HasRow(Context &ctx, Ino id);
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_TRACE_RECORDER_H_
