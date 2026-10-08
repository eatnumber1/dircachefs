#ifndef DCFS_TESTONLY_OBSERVERS_H_
#define DCFS_TESTONLY_OBSERVERS_H_

// Several testonly observers (dcfs/protocol_events.h) installed as one: each
// call goes to every one of them, in the order they were added (step
// 26.4b: the recorder, the invariant checker and the cost counter share the
// one hook object Context::events). Generated from protocol_events.h's
// method list: a method added there is added here too (the compiler says
// so only if it is pure, so keep them in step).

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/source_location.h"
#include "dcfs/protocol_events.h"

namespace dcfs::testonly {

class Observers final : public ProtocolEvents {
 public:
  Observers() = default;
  explicit Observers(std::vector<ProtocolEvents *> observers)
      : observers_(std::move(observers)) {}

  void Add(ProtocolEvents *observer) { observers_.push_back(observer); }
  void Remove(ProtocolEvents *observer) { std::erase(observers_, observer); }

  void RequestBegin(Context &ctx, const events::Request &request) override {
    for (ProtocolEvents *o : observers_) o->RequestBegin(ctx, request);
  }
  void RequestEnd(Context &ctx, const absl::Status &status) override {
    for (ProtocolEvents *o : observers_) o->RequestEnd(ctx, status);
  }
  void Replied(Context &ctx, int errnum) override {
    for (ProtocolEvents *o : observers_) o->Replied(ctx, errnum);
  }
  void GetattrBegin(Context &ctx, events::Ino id, bool valid) override {
    for (ProtocolEvents *o : observers_) o->GetattrBegin(ctx, id, valid);
  }
  void GetattrEnd(Context &ctx, const absl::Status &status) override {
    for (ProtocolEvents *o : observers_) o->GetattrEnd(ctx, status);
  }
  void LookupBegin(Context &ctx, events::Ino parent,
                   std::string_view name) override {
    for (ProtocolEvents *o : observers_) o->LookupBegin(ctx, parent, name);
  }
  void LookupEnd(Context &ctx, const absl::Status &status) override {
    for (ProtocolEvents *o : observers_) o->LookupEnd(ctx, status);
  }
  void LookupAnswered(Context &ctx, events::Ino parent, std::string_view name,
                      events::LookupOutcome answer,
                      events::Ino child) override {
    for (ProtocolEvents *o : observers_)
      o->LookupAnswered(ctx, parent, name, answer, child);
  }
  void RefreshBegin(Context &ctx, events::Ino id) override {
    for (ProtocolEvents *o : observers_) o->RefreshBegin(ctx, id);
  }
  void RefreshEnd(Context &ctx, const absl::Status &status) override {
    for (ProtocolEvents *o : observers_) o->RefreshEnd(ctx, status);
  }
  void SyncBegin(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->SyncBegin(ctx);
  }
  void SyncEnd(Context &ctx, const absl::Status &status) override {
    for (ProtocolEvents *o : observers_) o->SyncEnd(ctx, status);
  }
  void LookupDecided(Context &ctx, events::Ino parent, std::string_view name,
                     events::LookupOutcome outcome,
                     events::Ino child) override {
    for (ProtocolEvents *o : observers_)
      o->LookupDecided(ctx, parent, name, outcome, child);
  }
  void ResolveProbed(Context &ctx, events::Ino parent, std::string_view name,
                     const events::Probe &probe) override {
    for (ProtocolEvents *o : observers_)
      o->ResolveProbed(ctx, parent, name, probe);
  }
  void ResolveCommitted(Context &ctx, events::Ino parent, std::string_view name,
                        uint64_t snapshot, bool recorded) override {
    for (ProtocolEvents *o : observers_)
      o->ResolveCommitted(ctx, parent, name, snapshot, recorded);
  }
  void ChildRowRecorded(Context &ctx, events::Ino dir, events::Ino child,
                        bool filled) override {
    for (ProtocolEvents *o : observers_)
      o->ChildRowRecorded(ctx, dir, child, filled);
  }
  void PopulateStarted(Context &ctx, events::Ino dir) override {
    for (ProtocolEvents *o : observers_) o->PopulateStarted(ctx, dir);
  }
  void PopulateRead(Context &ctx, events::Ino dir,
                    events::ListingFn listing) override {
    for (ProtocolEvents *o : observers_) o->PopulateRead(ctx, dir, listing);
  }
  void PopulateCommitted(Context &ctx, events::Ino dir, uint64_t snapshot,
                         bool recorded) override {
    for (ProtocolEvents *o : observers_)
      o->PopulateCommitted(ctx, dir, snapshot, recorded);
  }
  void ListChecked(Context &ctx, events::Ino dir, bool complete) override {
    for (ProtocolEvents *o : observers_) o->ListChecked(ctx, dir, complete);
  }
  void AttrsStatted(Context &ctx, events::Ino id) override {
    for (ProtocolEvents *o : observers_) o->AttrsStatted(ctx, id);
  }
  void AttrsFilled(Context &ctx, events::Ino id, bool recorded) override {
    for (ProtocolEvents *o : observers_) o->AttrsFilled(ctx, id, recorded);
  }
  void ParentLookupStarted(Context &ctx, events::Ino dir) override {
    for (ProtocolEvents *o : observers_) o->ParentLookupStarted(ctx, dir);
  }
  void ParentRecorded(Context &ctx, events::Ino dir, events::Ino parent,
                      uint64_t snapshot, bool filled) override {
    for (ProtocolEvents *o : observers_)
      o->ParentRecorded(ctx, dir, parent, snapshot, filled);
  }
  void RootRecorded(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->RootRecorded(ctx);
  }
  void MutationBegun(Context &ctx, events::IdsFn ids, bool synced) override {
    for (ProtocolEvents *o : observers_) o->MutationBegun(ctx, ids, synced);
  }
  void MutationAborted(Context &ctx, events::IdsFn ids) override {
    for (ProtocolEvents *o : observers_) o->MutationAborted(ctx, ids);
  }
  void MutationSyscallStarting(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->MutationSyscallStarting(ctx);
  }
  void MutationSyscall(Context &ctx, const absl::Status &status) override {
    for (ProtocolEvents *o : observers_) o->MutationSyscall(ctx, status);
  }
  void NewChildProbed(Context &ctx, events::Ino parent, std::string_view name,
                      const events::Probe &probe) override {
    for (ProtocolEvents *o : observers_)
      o->NewChildProbed(ctx, parent, name, probe);
  }
  void MutationEnding(Context &ctx, events::IdsFn ids,
                      absl::FunctionRef<bool(events::Ino)> owns) override {
    for (ProtocolEvents *o : observers_) o->MutationEnding(ctx, ids, owns);
  }
  void MutationEnded(Context &ctx, events::IdsFn ids) override {
    for (ProtocolEvents *o : observers_) o->MutationEnded(ctx, ids);
  }
  void NameResolved(Context &ctx, events::Ino parent, std::string_view name,
                    bool found) override {
    for (ProtocolEvents *o : observers_)
      o->NameResolved(ctx, parent, name, found);
  }
  void Reresolve(Context &ctx, events::Ino parent,
                 std::string_view name) override {
    for (ProtocolEvents *o : observers_) o->Reresolve(ctx, parent, name);
  }
  void WritesEnded(Context &ctx, events::Ino id) override {
    for (ProtocolEvents *o : observers_) o->WritesEnded(ctx, id);
  }
  void FileOpened(Context &ctx, events::Ino id, int flags, bool shared,
                  const absl::Status &status,
                  const events::SharedFd &after) override {
    for (ProtocolEvents *o : observers_)
      o->FileOpened(ctx, id, flags, shared, status, after);
  }
  void FileReleased(Context &ctx, events::Ino id, bool writable,
                    const events::SharedFd &after) override {
    for (ProtocolEvents *o : observers_)
      o->FileReleased(ctx, id, writable, after);
  }
  void LifetimeChanged(Context &ctx, events::Ino id, events::LifetimeStep step,
                       uint64_t arg, events::LifetimeFn after) override {
    for (ProtocolEvents *o : observers_)
      o->LifetimeChanged(ctx, id, step, arg, after);
  }
  void Destroyed(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->Destroyed(ctx);
  }
  void SyncSnapshotTaken(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->SyncSnapshotTaken(ctx);
  }
  void SyncfsStarting(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->SyncfsStarting(ctx);
  }
  void SyncfsDone(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->SyncfsDone(ctx);
  }
  void SyncCleared(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->SyncCleared(ctx);
  }
  void RunStarting(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->RunStarting(ctx);
  }
  void Recovered(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->Recovered(ctx);
  }
  void RunStarted(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->RunStarted(ctx);
  }
  void RecoveryDone(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->RecoveryDone(ctx);
  }
  void ShutdownBegin(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->ShutdownBegin(ctx);
  }
  void Checkpointed(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->Checkpointed(ctx);
  }
  void CleanShutdownRecorded(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->CleanShutdownRecorded(ctx);
  }
  void OutOfBandChange(Context &ctx, events::Ino id) override {
    for (ProtocolEvents *o : observers_) o->OutOfBandChange(ctx, id);
  }
  void InodeForgetting(Context &ctx, events::Ino id) override {
    for (ProtocolEvents *o : observers_) o->InodeForgetting(ctx, id);
  }
  void InodeForgotten(Context &ctx, events::Ino id) override {
    for (ProtocolEvents *o : observers_) o->InodeForgotten(ctx, id);
  }
  void BackingCall(Context &ctx, std::string_view what,
                   absl::SourceLocation site) override {
    for (ProtocolEvents *o : observers_) o->BackingCall(ctx, what, site);
  }
  void CheckRequestBegin(Context &ctx, const DirCacheFS &fs,
                         const events::Request &request) override {
    for (ProtocolEvents *o : observers_) o->CheckRequestBegin(ctx, fs, request);
  }
  void CheckRequestEnd(Context &ctx, const DirCacheFS &fs,
                       const events::Request &request) override {
    for (ProtocolEvents *o : observers_) o->CheckRequestEnd(ctx, fs, request);
  }
  void CheckForgetting(Context &ctx, const DirCacheFS &fs, uint64_t ino,
                       uint64_t nlookup) override {
    for (ProtocolEvents *o : observers_)
      o->CheckForgetting(ctx, fs, ino, nlookup);
  }
  void CheckRunStarting(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->CheckRunStarting(ctx);
  }
  void CheckRunStarted(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->CheckRunStarted(ctx);
  }
  void CheckDestroyed(Context &ctx, const DirCacheFS &fs) override {
    for (ProtocolEvents *o : observers_) o->CheckDestroyed(ctx, fs);
  }
  void SqliteStep(std::string_view sql) override {
    for (ProtocolEvents *o : observers_) o->SqliteStep(sql);
  }
  void SqliteTransaction(bool durable) override {
    for (ProtocolEvents *o : observers_) o->SqliteTransaction(durable);
  }

  void IdentityResolved(Context &ctx, events::Ino id,
                        const events::IdentityCheck &check) override {
    for (ProtocolEvents *o : observers_) o->IdentityResolved(ctx, id, check);
  }

  void Interrupted(Context &ctx) override {
    for (ProtocolEvents *o : observers_) o->Interrupted(ctx);
  }

 private:
  std::vector<ProtocolEvents *> observers_;
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_OBSERVERS_H_
