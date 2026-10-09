#ifndef DCFS_TESTONLY_FAKE_BOOKKEEPING_H_
#define DCFS_TESTONLY_FAKE_BOOKKEEPING_H_

// A plain-data events::Bookkeeping (dcfs/protocol_events.h): a copy of what
// a real view reported, which a test then edits to show the runtime
// invariant checks (dcfs/testonly/invariant_checker.h) a state DirCacheFS
// never reaches, such as a lookup count of 0 or a removed record beside a
// written entry. The checker's tests give it to the checks directly
// (InvariantChecker::CheckAll and the others) or install it for the hooks
// (InvariantChecker::TamperBookkeeping).

#include <cstddef>
#include <cstdint>
#include <optional>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "dcfs/protocol_events.h"

namespace dcfs::testonly {

struct FakeBookkeeping final : events::Bookkeeping {
  absl::flat_hash_map<events::Ino, uint64_t> lookups;
  // Each entry of written_: whether it holds a descriptor.
  absl::flat_hash_map<events::Ino, bool> written;
  size_t held_fds = 0;
  size_t max_held_fds = 0;
  absl::flat_hash_set<events::Ino> open_for_write;
  // What OpenForWriteSet() returns: the real set's identity, so that the
  // check of Context::open_for_write still compares the right pointer.
  const void *open_for_write_set = nullptr;
  absl::flat_hash_set<events::Ino> removed;
  absl::flat_hash_map<events::Ino, events::Bookkeeping::SharedFile> shared;

  // A copy of `real`, entry by entry.
  static FakeBookkeeping CopyOf(const events::Bookkeeping &real) {
    FakeBookkeeping copy;
    real.ForEachLookup(
        [&](events::Ino id, uint64_t count) { copy.lookups[id] = count; });
    real.ForEachWritten(
        [&](events::Ino id, bool holds) { copy.written[id] = holds; });
    copy.held_fds = real.HeldFdCount();
    copy.max_held_fds = real.HeldFdLimit();
    real.ForEachOpenForWrite(
        [&](events::Ino id) { copy.open_for_write.insert(id); });
    copy.open_for_write_set = real.OpenForWriteSet();
    real.ForEachRemoved([&](events::Ino id) { copy.removed.insert(id); });
    real.ForEachSharedFile(
        [&](events::Ino id, const events::Bookkeeping::SharedFile &file) {
          copy.shared[id] = file;
        });
    return copy;
  }

  std::optional<uint64_t> Lookups(events::Ino id) const override {
    auto it = lookups.find(id);
    if (it == lookups.end()) return std::nullopt;
    return it->second;
  }
  size_t LookupEntries() const override { return lookups.size(); }
  void ForEachLookup(
      absl::FunctionRef<void(events::Ino, uint64_t)> each) const override {
    for (const auto &[id, count] : lookups) each(id, count);
  }
  bool IsWritten(events::Ino id) const override {
    return written.contains(id);
  }
  size_t WrittenEntries() const override { return written.size(); }
  void ForEachWritten(
      absl::FunctionRef<void(events::Ino, bool)> each) const override {
    for (const auto &[id, holds] : written) each(id, holds);
  }
  size_t HeldFdCount() const override { return held_fds; }
  size_t HeldFdLimit() const override { return max_held_fds; }
  bool IsOpenForWrite(events::Ino id) const override {
    return open_for_write.contains(id);
  }
  size_t OpenForWriteEntries() const override { return open_for_write.size(); }
  void ForEachOpenForWrite(
      absl::FunctionRef<void(events::Ino)> each) const override {
    for (events::Ino id : open_for_write) each(id);
  }
  const void *OpenForWriteSet() const override { return open_for_write_set; }
  bool IsRemoved(events::Ino id) const override { return removed.contains(id); }
  size_t RemovedEntries() const override { return removed.size(); }
  void ForEachRemoved(
      absl::FunctionRef<void(events::Ino)> each) const override {
    for (events::Ino id : removed) each(id);
  }
  std::optional<events::Bookkeeping::SharedFile> SharedFileOf(
      events::Ino id) const override {
    auto it = shared.find(id);
    if (it == shared.end()) return std::nullopt;
    return it->second;
  }
  void ForEachSharedFile(
      absl::FunctionRef<void(events::Ino,
                             const events::Bookkeeping::SharedFile &)>
          each) const override {
    for (const auto &[id, file] : shared) each(id, file);
  }
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_FAKE_BOOKKEEPING_H_
