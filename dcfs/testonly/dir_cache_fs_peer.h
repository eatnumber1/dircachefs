#ifndef DCFS_TESTONLY_DIR_CACHE_FS_PEER_H_
#define DCFS_TESTONLY_DIR_CACHE_FS_PEER_H_

// DirCacheFS's in-memory bookkeeping, for the runtime invariant checks
// (dcfs/testonly/invariant_checker.h), which read it, and for their tests,
// which break it on purpose. DirCacheFS names this struct as a friend;
// nothing in production uses it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fd.h"
#include "dcfs/metadata_cache.h"

namespace dcfs::testonly {

struct DirCacheFSPeer {
  // A BackingFile's counts (DirCacheFS::BackingFile).
  struct Shared {
    int refs = 0;
    int writable_refs = 0;
  };

  static const absl::flat_hash_map<InodeId, uint64_t> &Lookups(
      const DirCacheFS &fs) {
    return fs.lookups_;
  }
  static const absl::flat_hash_map<InodeId, std::optional<FileDescriptor>> &
  Written(const DirCacheFS &fs) {
    return fs.written_;
  }
  static size_t HeldFds(const DirCacheFS &fs) { return fs.held_fds_; }
  static size_t MaxHeldFds(const DirCacheFS &fs) { return fs.max_held_fds_; }
  static const absl::flat_hash_set<int64_t> &OpenForWrite(
      const DirCacheFS &fs) {
    return fs.open_for_write_;
  }
  static size_t RemovedCount(const DirCacheFS &fs) {
    return fs.removed_.size();
  }
  static bool IsRemoved(const DirCacheFS &fs, InodeId id) {
    return fs.removed_.contains(id);
  }
  static void ForEachRemoved(const DirCacheFS &fs,
                             absl::FunctionRef<void(InodeId)> each) {
    for (const auto &[id, removed] : fs.removed_) each(id);
  }
  static std::optional<Shared> SharedFile(const DirCacheFS &fs, InodeId id) {
    auto it = fs.backing_files_.find(id);
    if (it == fs.backing_files_.end()) return std::nullopt;
    return Shared{.refs = it->second.refs,
                  .writable_refs = it->second.writable_refs};
  }
  static void ForEachSharedFile(
      const DirCacheFS &fs,
      absl::FunctionRef<void(InodeId, const Shared &)> each) {
    for (const auto &[id, file] : fs.backing_files_) {
      each(id, Shared{.refs = file.refs, .writable_refs = file.writable_refs});
    }
  }

  // For the tests that break an invariant on purpose.
  static absl::flat_hash_map<InodeId, uint64_t> &MutableLookups(
      DirCacheFS &fs) {
    return fs.lookups_;
  }
  static absl::flat_hash_map<InodeId, std::optional<FileDescriptor>> &
  MutableWritten(DirCacheFS &fs) {
    return fs.written_;
  }
  static void AddRemoved(DirCacheFS &fs, InodeId id, cache::CachedAttr row,
                         FileDescriptor fd) {
    fs.removed_.insert_or_assign(
        id, DirCacheFS::Removed{.row = row, .fd = std::move(fd)});
  }
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_DIR_CACHE_FS_PEER_H_
