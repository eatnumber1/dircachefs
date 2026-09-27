#include "dcfs/fuse_request.h"

#include <cerrno>
#include <sys/stat.h>
#include <vector>

#include "absl/status/status.h"
#include "dcfs/status.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

struct stat MakeStat(ino_t ino, mode_t mode) {
  struct stat st = {};
  st.st_ino = ino;
  st.st_mode = mode;
  return st;
}

fuse_entry_param MakeEntryParam(fuse_ino_t ino, mode_t mode) {
  fuse_entry_param entry = {};
  entry.ino = ino;
  entry.attr.st_ino = ino;
  entry.attr.st_mode = mode;
  return entry;
}

TEST(AppendDirEntriesTest, FitsEverythingWhenBufferIsLargeEnough) {
  std::vector<FuseDirEntry> entries = {
      {.name = ".", .stbuf = MakeStat(1, S_IFDIR), .off = 1},
      {.name = "..", .stbuf = MakeStat(1, S_IFDIR), .off = 2},
      {.name = "foo", .stbuf = MakeStat(2, S_IFREG), .off = 3},
  };
  // A size query (null req/buf, as libfuse's fuse_add_direntry allows) for
  // the total space all three entries need.
  size_t total = 0;
  for (const FuseDirEntry &e : entries) {
    total += fuse_add_direntry(
        /*req=*/nullptr, /*buf=*/nullptr, /*bufsize=*/0, e.name.c_str(),
        &e.stbuf, e.off);
  }

  std::vector<char> buf(total);
  size_t used = AppendDirEntries(nullptr, buf.data(), buf.size(), entries);
  EXPECT_EQ(used, total);
}

TEST(AppendDirEntriesTest, StopsBeforeAnEntryThatDoesNotFit) {
  std::vector<FuseDirEntry> entries = {
      {.name = ".", .stbuf = MakeStat(1, S_IFDIR), .off = 1},
      {.name = "..", .stbuf = MakeStat(1, S_IFDIR), .off = 2},
  };
  size_t first_size = fuse_add_direntry(
      nullptr, nullptr, 0, entries[0].name.c_str(), &entries[0].stbuf,
      entries[0].off);

  // Room for exactly the first entry, and nothing more.
  std::vector<char> buf(first_size);
  size_t used = AppendDirEntries(nullptr, buf.data(), buf.size(), entries);
  EXPECT_EQ(used, first_size);
}

TEST(AppendDirEntriesTest, ZeroSizeBufferYieldsNothing) {
  std::vector<FuseDirEntry> entries = {
      {.name = "x", .stbuf = MakeStat(1, S_IFREG), .off = 1},
  };
  EXPECT_EQ(AppendDirEntries(nullptr, nullptr, 0, entries), 0);
}

TEST(AppendDirEntriesPlusTest, FitsEverythingWhenBufferIsLargeEnough) {
  std::vector<FuseDirEntryPlus> entries = {
      {.name = ".", .entry = MakeEntryParam(1, S_IFDIR), .off = 1},
      {.name = "foo", .entry = MakeEntryParam(2, S_IFREG), .off = 2},
  };
  size_t total = 0;
  for (const FuseDirEntryPlus &e : entries) {
    total += fuse_add_direntry_plus(
        nullptr, nullptr, 0, e.name.c_str(), &e.entry, e.off);
  }

  std::vector<char> buf(total);
  size_t used =
      AppendDirEntriesPlus(nullptr, buf.data(), buf.size(), entries);
  EXPECT_EQ(used, total);
}

TEST(AppendDirEntriesPlusTest, StopsBeforeAnEntryThatDoesNotFit) {
  std::vector<FuseDirEntryPlus> entries = {
      {.name = ".", .entry = MakeEntryParam(1, S_IFDIR), .off = 1},
      {.name = "foo", .entry = MakeEntryParam(2, S_IFREG), .off = 2},
  };
  size_t first_size = fuse_add_direntry_plus(
      nullptr, nullptr, 0, entries[0].name.c_str(), &entries[0].entry,
      entries[0].off);

  std::vector<char> buf(first_size);
  size_t used =
      AppendDirEntriesPlus(nullptr, buf.data(), buf.size(), entries);
  EXPECT_EQ(used, first_size);
}

TEST(StatusToErrnoTest, ErrnoPayloadStatusRoundTrips) {
  absl::Status s = ErrnoToStatus(ENOENT, "open");
  EXPECT_EQ(StatusToErrno(s), ENOENT);
}

TEST(StatusToErrnoTest, PlainCodeStatusUsesFallbackMapping) {
  absl::Status s = absl::PermissionDeniedError("no");
  EXPECT_EQ(StatusToErrno(s), EPERM);
}

// A default-constructed FuseRequest holds no live request -- the same state
// a real one is left in after any Reply* call. Exercising a Reply* on it
// stands in for the "replied twice" bug the RET_CHECK in each Reply* method
// is meant to catch, without needing a live mount.
TEST(FuseRequestTest, ReplyOnASpentRequestFailsRatherThanCrashing) {
  FuseRequest fr;
  absl::Status s = fr.ReplyErrno(0);
  EXPECT_EQ(s.code(), absl::StatusCode::kInternal);
}

TEST(FuseRequestTest, ReplyFailureRejectsAnOkStatus) {
  FuseRequest fr;
  absl::Status s = fr.ReplyFailure(absl::OkStatus());
  EXPECT_EQ(s.code(), absl::StatusCode::kInternal);
}

}  // namespace
}  // namespace dcfs
