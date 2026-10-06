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
// Not thread-safe: dcfs serves one request at a time.

#include <cstdint>
#include <map>
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
  // trace name `trace` (no spaces).
  TraceRecorder(int fd, std::string trace);

  // Begins the trace of every directory now in the cache. For a test that
  // sets Context::events itself, once its setup is done and nothing is in
  // flight; the daemon's recorder does it when StartRun completes.
  void BeginAll(Context &ctx);

  void RequestBegin(Context &ctx, const events::Request &request) override;
  void RequestEnd(Context &ctx, const absl::Status &status) override;
  void GetattrBegin(Context &ctx, events::Ino id, bool valid) override;
  void GetattrEnd(Context &ctx, const absl::Status &status) override;
  void LookupBegin(Context &ctx, events::Ino parent,
                   std::string_view name) override;
  void LookupEnd(Context &ctx, const absl::Status &status) override;
  void RefreshBegin(Context &ctx, events::Ino id) override;
  void RefreshEnd(Context &ctx, const absl::Status &status) override;
  void SyncBegin(Context &ctx) override;
  void SyncEnd(Context &ctx, const absl::Status &status) override;

  void LookupDecided(Context &ctx, events::Ino parent, std::string_view name,
                     events::LookupOutcome outcome,
                     events::Ino child) override;
  void ResolveProbed(Context &ctx, events::Ino parent, std::string_view name,
                     const events::Probe &probe) override;
  void ResolveCommitted(Context &ctx, events::Ino parent,
                        std::string_view name, uint64_t snapshot,
                        bool recorded, events::Ino child) override;
  void PopulateStarted(Context &ctx, events::Ino dir) override;
  void PopulateRead(Context &ctx, events::Ino dir,
                    events::ListingFn listing) override;
  void PopulateCommitted(Context &ctx, events::Ino dir, uint64_t snapshot,
                         bool recorded, events::IdsFn children) override;
  void ListChecked(Context &ctx, events::Ino dir, bool complete) override;
  void AttrsStatted(Context &ctx, events::Ino id) override;
  void AttrsFilled(Context &ctx, events::Ino id, bool recorded) override;
  void ParentRecorded(Context &ctx, events::Ino parent,
                      uint64_t snapshot) override;
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
  void ShutdownBegin(Context &ctx) override;
  void Checkpointed(Context &ctx) override;
  void CleanShutdownRecorded(Context &ctx) override;

  void OutOfBandChange(Context &ctx, events::Ino id) override;
  void InodeForgotten(Context &ctx, events::Ino id) override;

 private:
  using Ino = events::Ino;

  // One request slot of one directory's trace.
  struct Req {
    int slot = 0;
    std::string kind;  // a request kind of the model (AllKinds)
    std::string n, m;  // its names (escaped), if any
    bool arrived = false;   // its first line (the model's Arrive) is out
    bool terminal = false;  // the model's request has replied
    bool expects_refresh = false;  // a refresh of the directory is its own
    bool begun = false;            // a mutation's phase 1 committed
    bool syscall_seen = false;     // ... its phase-2 syscall returned
    bool syscall_ok = false;
    bool probe_absent = false;     // a create's probe found nothing
    bool owned = false;            // Mutation::Owns at its End
    bool ended_early = false;      // its End came before its syscall
  };

  // A frame: a FUSE request, or a getattr, lookup, refresh or sync point
  // running inside one (or on its own).
  struct Frame {
    enum Kind { kRequest, kGetattr, kLookup, kRefresh, kSync };
    Kind kind = kRequest;
    // kRequest: the request.
    events::Op op = events::Op::kOther;
    Ino ino = 0;
    Ino newparent = 0;
    std::string name, newname;  // raw bytes
    unsigned int flags = 0;
    int64_t offset = 0;
    // kGetattr, kRefresh: the inode; kLookup: the parent and the name.
    Ino id = 0;
    std::string lookup_name;
    // kRefresh: of valid attributes, outside any request that expects it:
    // nothing the model can see, so no lines.
    bool silent = false;
    // kSync: the callback count at its snapshot (SyncSnapshotTaken).
    int64_t snapshot_at = -1;
    // The requests of directories' traces this frame owns.
    std::map<Ino, Req> reqs;
  };

  // What a FUSE request is to directory `dir`'s trace.
  struct Mapping {
    enum Kind { kNone, kRequest, kUnmodelled };
    Kind kind = kNone;
    std::string req_kind, n, m;
    std::string why;  // kUnmodelled
  };

  struct Dir {
    std::string last;  // the state in its last line
    bool dead = false;  // cut, unexplained or gone: no more lines
    std::set<int> slots;  // held by open requests
    // A population between PopulateStarted and PopulateRead: its line goes
    // where it started (the model's PopulateRead), so the lines written
    // meanwhile (by requests that ran during its reads) wait here.
    bool reading = false;
    int read_slot = 0;
    std::string read_db;  // the state when it started
    std::vector<std::string> held;
  };

  static Mapping Map(const Frame &request, Ino dir);
  // The traced directories a FUSE request mutates as a modelled request.
  std::vector<Ino> MutatedDirs(const Frame &request);
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
  // Opens a request of `dir`'s trace owned by `frame`.
  Req &Open(Frame &frame, Ino dir, std::string kind, std::string n = "",
            std::string m = "");
  // Closes `frame`: frees its slots and ends each of its requests with a
  // "reply" line, or with a cut if `unmodelled_end(req)` names a reason
  // (an end the model does not have), or an "unexplained" line if the
  // reason starts with "unexplained: ".
  void Close(Context &ctx, Frame &frame,
             absl::FunctionRef<std::string(const Req &)> unmodelled_end);

  bool Traced(Ino dir) const;
  std::string Snapshot(Context &ctx, Ino dir);
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
  // How a frame that ends with `status` ends its requests (see Close).
  static std::string FrameEnd(std::string_view what,
                              const absl::Status &status);
  void Write(Ino dir, const std::string &json);
  // After every callback: begins the traces of new directories, ends those
  // of deleted ones, and gives every other directory whose state changed
  // an "unexplained" line (a cut instead if `cut_changes`).
  void After(Context &ctx, bool cut_changes = false);
  std::vector<Ino> AllDirs(Context &ctx);

  int fd_;
  std::string trace_;
  std::vector<Frame> frames_;
  std::map<Ino, Dir> dirs_;
  bool shutdown_ = false;
  int64_t line_ = 0;
  const char *cause_ = "";  // the callback being handled, for the lines
  int64_t callbacks_ = 0;   // callbacks handled so far
  std::set<Ino> covered_;   // directories given a line by this callback
  int64_t changes_ = -1;    // sqlite3_total_changes at the last check
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_TRACE_RECORDER_H_
