#ifndef DCFS_TESTONLY_INVARIANT_CHECKER_H_
#define DCFS_TESTONLY_INVARIANT_CHECKER_H_

// The checking implementation of InvariantChecks (dcfs/invariant_checks.h;
// docs/design.md, "Runtime invariant checks"; step 26.2). Test-only: the
// daemon of the testonly checking build (//dcfs:main_static_checked, which
// the fast and presubmit tiers' guests run) and the forged-request harness
// install it; production binaries link the no-op.
//
// What it checks, and where (each check's name is what a violation
// reports):
//
//  At every backing syscall (BackingCall):
//   no-transaction-at-backing-call  no transaction is open on the cache
//       database and no statement is part way through its rows (a read
//       cursor holds a read transaction): docs/design.md, "Rules that hold
//       now so that coroutines need no redesign".
//   dirty-set  every inode with a mutation in flight (FillGuards::inflight,
//       between its phase 1 and its end) has its dirty row: phase 1 commits
//       it before the syscall, and a sync point keeps it while the mutation
//       is in flight.
//
//  At the end of every request, after its reply (RequestEnd), for the rows
//  the request changed (SQLite's update hook tells which, so the work is
//  proportional to them, not to the database; except a DELETE with no
//  WHERE, which SQLite runs as a truncation without calling the hook), the
//  inodes it named, and every inode open for writing or durably dirty
//  (those whose dirty row such a truncating DELETE could drop: ClearDirty's
//  one-statement clear), while there are at most kRecountLimit of them:
//   no-transaction-at-request-end  as above, once the request is over.
//   tri-state  an inode's attributes recorded as current have every column
//       and a link count above 0 (backing::WriteAttrs keeps nlink 0
//       unknown); a dentry is 'refused' exactly when its stub row exists.
//   identity  only the root has FUSE generation 0.
//   dirty-set  as above; an inode in Context::dirty.durable has its dirty
//       row (phase 1's fast path skips the insert for those); if
//       Context::dirty.any is false the table is empty; a dirty row that
//       went (only a sync point or recovery deletes one) was not of an
//       inode with a mutation in flight, open for writing, or durable.
//   writable-open  an inode open for writing (Context::open_for_write) has
//       its attributes unknown and its dirty row (BeginWriting's phase 1;
//       a sync point keeps it), and is in DirCacheFS::written_ (its last
//       FORGET reconciles it) unless it is a removed object; a BackingFile
//       with writable opens is open for writing; refs >= writable_refs.
//   lookup-count  DirCacheFS::lookups_ holds only positive counts, and a
//       FORGET never drops more lookups than were counted (checked when it
//       arrives: Forgetting).
//   held-fds  DirCacheFS::held_fds_ is the number of written_ entries that
//       hold a descriptor, and at most max_held_fds_.
//   removed-record  a removed record (DirCacheFS::removed_) exists only
//       while the kernel holds a lookup of its nodeid, and never beside a
//       written_ entry (RetireRemoved takes it).
//  The DirCacheFS-wide counts are recounted while written_ and removed_
//  hold at most kRecountLimit entries; above that only the request's own
//  inodes are checked (DESTROY's full check still recounts).
//
//  At backing::StartRun's end (RunStarted) and after DESTROY (Destroyed):
//  all of the above over the whole database and every DirCacheFS entry.
//
// A violation is fatal: LOG(FATAL) "invariant violated: <invariant>: <what>
// (in request <OPCODE> nodeid <n>...)" (or "in StartRun", "in DESTROY",
// "outside any request"), after the same line, as
// "DCFS-INVARIANT-VIOLATION <invariant>: ...", is written to `console_fd`
// if there is one (the daemon's is /dev/console, the guest's serial log,
// where test/qemu/scripts/run-qemu.sh fails the run on it: a daemon
// writes its log to a file the guest script may never read).
//
// Not thread-safe: dcfs serves one request at a time.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/invariant_checks.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "sqlite3.h"

namespace dcfs::testonly {

class InvariantChecker final : public InvariantChecks {
 public:
  // See the top of this file for the DirCacheFS-wide recounts.
  static constexpr size_t kRecountLimit = 16384;

  // `console_fd` (not owned; -1 for none): where a violation is also
  // written (see the top of this file). The harness passes none: its death
  // tests' violations must not reach the serial log.
  explicit InvariantChecker(int console_fd = -1);
  InvariantChecker(const InvariantChecker &) = delete;
  InvariantChecker &operator=(const InvariantChecker &) = delete;
  // Removes the update hook from the database it was attached to, which
  // must still be open.
  ~InvariantChecker() override;

  // InvariantChecks: each aborts on a violation.
  void BackingCall(Context &ctx, std::string_view what) override;
  void RequestBegin(Context &ctx, const DirCacheFS &fs,
                    const events::Request &request) override;
  void RequestEnd(Context &ctx, const DirCacheFS &fs,
                  const events::Request &request) override;
  void Forgetting(Context &ctx, const DirCacheFS &fs, uint64_t ino,
                  uint64_t nlookup) override;
  void RunStarted(Context &ctx) override;
  void Destroyed(Context &ctx, const DirCacheFS &fs) override;

  // The checks themselves, for a test that wants to look rather than die
  // (step 26.6 checks the invariants after each injected fault): OK, or
  // the first violation, a FailedPrecondition whose message starts with
  // the invariant's name. Another error is a query of the checker's own
  // that failed. `fs` may be null: no DirCacheFS checks then.
  //
  // What BackingCall checks.
  absl::Status CheckBackingCall(Context &ctx);
  // What RequestEnd checks: the rows changed since the last check, and the
  // inodes `ids`; forgets those rows.
  absl::Status CheckChanged(Context &ctx, const DirCacheFS *fs,
                            std::span<const InodeId> ids);
  // What RunStarted and Destroyed check: everything; forgets the changed
  // rows.
  absl::Status CheckAll(Context &ctx, const DirCacheFS *fs);

 private:
  // One request being served (requests nest; see RequestBegin), or
  // StartRun's or DESTROY's check (`label`).
  struct Frame {
    std::string_view label;
    events::Op op = events::Op::kOther;
    uint64_t nodeid = 0;
    // The inodes it named (its nodeid, a rename's or link's new parent,
    // what a FORGET forgot), checked at its end with the rows it changed.
    std::vector<InodeId> ids;
    // What it forgot of each nodeid so far (Forgetting).
    absl::flat_hash_map<InodeId, uint64_t> forgotten;
  };

  // The per-inode checks of `id`: its row's columns if `row_changed`, and
  // what being open for writing or durably dirty requires.
  absl::Status CheckInode(Context &ctx, const DirCacheFS *fs, InodeId id,
                          bool row_changed);
  // The checks of an inodes row's own columns: `row` is (id, attrs_valid,
  // fuse_gen, nlink, whether an attribute column is NULL).
  absl::Status CheckInodeRow(const Context &ctx, const DirCacheFS *fs,
                             sqlite3::Statement &row);
  // The dentry with rowid `rowid`, if it still exists: refused exactly
  // when it has a stub. Adds its parent and child to `interest`.
  absl::Status CheckDentry(Context &ctx, int64_t rowid,
                           absl::flat_hash_set<InodeId> &interest);
  // The stub `id`, if it still exists: its dentry is refused.
  absl::Status CheckStub(Context &ctx, int64_t id);
  // CheckAll; `destroyed`: after DirCacheFS::Destroy, which empties
  // written_ by design (an inode still open for writing then is not in
  // it).
  absl::Status CheckEverything(Context &ctx, const DirCacheFS *fs,
                               bool destroyed);
  // DirCacheFS's bookkeeping: for the inodes in `interest` (and the
  // recounts below kRecountLimit), or everything if it is null.
  absl::Status CheckBookkeeping(const Context &ctx, const DirCacheFS &fs,
                                const absl::flat_hash_set<InodeId> *interest,
                                bool destroyed);
  // removed-record, for the removed record of `id`.
  absl::Status CheckRemoved(const DirCacheFS &fs, InodeId id);

  // Whether `id` is open for writing (Context::open_for_write), as the
  // checks see it: not if the open is older than the run (see
  // RunStarted).
  bool OpenForWrite(const Context &ctx, InodeId id) const;
  // Drops from stale_opens_ what is no longer open for writing.
  void ForgetReleasedStaleOpens(const Context &ctx);

  // Points the update hook at ctx.db if it is not there yet.
  void Attach(Context &ctx);
  static void OnUpdate(void *self, int op, const char *db, const char *table,
                       sqlite3_int64 rowid);
  // "in request LOOKUP nodeid 1, inside ...", "in DESTROY" or "outside
  // any request".
  std::string Where() const;
  // Aborts with `violation` (see the top of this file).
  void Fail(const absl::Status &violation);
  void FailIfNotOk(const absl::Status &status) {
    if (!status.ok()) Fail(status);
  }

  int console_fd_;
  ::sqlite3 *db_ = nullptr;
  std::vector<Frame> frames_;
  // The inodes open for writing when the run started (see RunStarted).
  absl::flat_hash_set<InodeId> stale_opens_;
  // The rows changed since the last check, by table (rowids; an inode's,
  // a stub's and a dirty row's rowid is its id).
  absl::flat_hash_set<int64_t> inodes_;
  absl::flat_hash_set<int64_t> dentries_;
  absl::flat_hash_set<int64_t> stubs_;
  absl::flat_hash_set<int64_t> dirty_;
  absl::flat_hash_set<int64_t> xattrs_;
  absl::flat_hash_set<int64_t> directories_;
  absl::flat_hash_set<int64_t> symlinks_;
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_INVARIANT_CHECKER_H_
