// SPDX-License-Identifier: MIT
#include "dcfs/fuse_request.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/status/status.h"
#include "dcfs/status.h"
#include "fuse_lowlevel.h"
#include "gmock/gmock.h"
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
  size_t first_size =
      fuse_add_direntry(nullptr, nullptr, 0, entries[0].name.c_str(),
                        &entries[0].stbuf, entries[0].off);

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
    total += fuse_add_direntry_plus(nullptr, nullptr, 0, e.name.c_str(),
                                    &e.entry, e.off);
  }

  std::vector<char> buf(total);
  size_t used = AppendDirEntriesPlus(nullptr, buf.data(), buf.size(), entries);
  EXPECT_EQ(used, total);
}

TEST(AppendDirEntriesPlusTest, StopsBeforeAnEntryThatDoesNotFit) {
  std::vector<FuseDirEntryPlus> entries = {
      {.name = ".", .entry = MakeEntryParam(1, S_IFDIR), .off = 1},
      {.name = "foo", .entry = MakeEntryParam(2, S_IFREG), .off = 2},
  };
  size_t first_size =
      fuse_add_direntry_plus(nullptr, nullptr, 0, entries[0].name.c_str(),
                             &entries[0].entry, entries[0].off);

  std::vector<char> buf(first_size);
  size_t used = AppendDirEntriesPlus(nullptr, buf.data(), buf.size(), entries);
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

// How many times fuse_reply_none was called (the --wrap fake below).
int &RepliesNone() {
  static int replies = 0;
  return replies;
}

// The text of every ERROR logged while it lives.
class ErrorCapture : public absl::LogSink {
 public:
  ErrorCapture() { absl::AddLogSink(this); }
  ~ErrorCapture() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    if (entry.log_severity() >= absl::LogSeverity::kError) {
      lines.emplace_back(entry.text_message());
    }
  }
  std::vector<std::string> lines;
};

// FORGET has no reply to carry an error in: a second ReplyNone (a bug) is
// refused, never reaching libfuse with the request it freed, and logged once.
TEST(FuseRequestTest, ReplyNoneTwiceRepliesOnceAndLogsOnce) {
  ErrorCapture errors;
  const int before = RepliesNone();
  {
    FuseRequest req(reinterpret_cast<fuse_req_t>(0x1));
    req.ReplyNone();
    req.ReplyNone();
  }
  EXPECT_EQ(RepliesNone() - before, 1);
  EXPECT_THAT(errors.lines,
              ::testing::ElementsAre(::testing::HasSubstr("already replied")));
}

}  // namespace
}  // namespace dcfs

extern "C" void __wrap_fuse_reply_none(fuse_req_t) { ++dcfs::RepliesNone(); }
