// Tests of the trace recorder's own decisions (trace_recorder.h): which
// changes of a directory's cached state it explains, cuts or calls
// unexplained. The events are produced by the cache functions themselves
// (they call Context::events), on an in-memory database; the lines are read
// back from a file in TEST_TMPDIR.

#include "dcfs/testonly/trace_recorder.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#define DCFS_TEST_CONCAT_INNER(x, y) x##y
#define DCFS_TEST_CONCAT(x, y) DCFS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                        \
  ASSERT_OK_AND_ASSIGN_IMPL(                                    \
      DCFS_TEST_CONCAT(_status_or_value_, __LINE__), lhs, rexpr)
#define ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());        \
  lhs = std::move(statusor).value()

namespace dcfs::testonly {
namespace {

using ::absl_testing::IsOk;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using cache::InodeId;

DeviceId Source() {
  DeviceId id;
  id.uuid.fill(0xAB);
  return id;
}

struct statx Stx(uint64_t ino, mode_t mode) {
  struct statx stx {};
  stx.stx_mask = STATX_BASIC_STATS | STATX_BTIME;
  stx.stx_ino = ino;
  stx.stx_mode = mode;
  stx.stx_nlink = 1;
  stx.stx_btime = {.tv_sec = 1000 + static_cast<int64_t>(ino), .tv_nsec = 7};
  return stx;
}

class TraceRecorderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
    ASSERT_THAT(Migrate(db_, RootIdentity{.device_id = Source(),
                                          .fstype = 0xEF53,
                                          .backing_ino = 2}),
                IsOk());
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    path_ = absl::StrCat(tmpdir, "/trace_", ::testing::UnitTest::GetInstance()
                                                ->current_test_info()
                                                ->name());
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ASSERT_GE(fd_, 0) << std::strerror(errno);
  }

  void TearDown() override {
    ctx_.events = &NoProtocolEvents();
    if (fd_ >= 0) ::close(fd_);
  }

  // A row for a backing object, with a handle derived from `ino`.
  absl::StatusOr<InodeId> Make(uint64_t ino, mode_t mode) {
    std::string bytes = absl::StrCat("h", ino);
    ABSL_ASSIGN_OR_RETURN(
        cache::UpsertResult row,
        cache::UpsertInode(
            ctx_,
            FileHandle{.device = Source(),
                       .handle_type = 1,
                       .bytes = std::vector<uint8_t>(bytes.begin(),
                                                     bytes.end())},
            Stx(ino, mode), 0));
    return row.id;
  }

  // A directory row linked as (parent, name), listing incomplete.
  absl::StatusOr<InodeId> MakeDir(InodeId parent, std::string_view name,
                                  uint64_t ino) {
    ABSL_ASSIGN_OR_RETURN(InodeId id, Make(ino, S_IFDIR | 0755));
    ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx_, parent, name, id));
    ABSL_RETURN_IF_ERROR(cache::MarkDirComplete(ctx_, id, false));
    return id;
  }

  // Starts recording; every directory's trace begins now.
  void StartTrace() {
    recorder_ = std::make_unique<TraceRecorder>(fd_, "test");
    ctx_.events = recorder_.get();
    recorder_->BeginAll(ctx_);
  }

  // Some callback outside every transaction (an empty request), at which
  // the recorder checks what changed.
  void Tick() {
    events::RequestScope scope(*ctx_.events, ctx_,
                               {.op = events::Op::kStatfs, .ino = 999});
  }

  // The lines of directory `dir`'s trace so far.
  std::vector<std::string> Lines(InodeId dir) {
    std::ifstream in(path_);
    std::vector<std::string> lines;
    const std::string prefix = absl::StrCat("DCFS-TRACE test ", dir, " ");
    for (std::string line; std::getline(in, line);) {
      if (absl::StartsWith(line, prefix)) lines.push_back(line);
    }
    return lines;
  }

  sqlite3::Connection db_;
  MountFds mounts_;
  absl::BitGen bitgen_;
  Context ctx_{db_, mounts_, bitgen_};
  std::unique_ptr<TraceRecorder> recorder_;
  std::string path_;
  int fd_ = -1;
};

// --- InodeForgotten (review of 2026-10-06, finding 3) ----------------------

// An invalidated inode makes the names pointing at it unknown, which the
// model has no step for: the directories holding them are cut, and nothing
// else is.
TEST_F(TraceRecorderTest, ForgottenInodeCutsTheDirectoriesThatNamedIt) {
  ASSERT_OK_AND_ASSIGN(InodeId d1, MakeDir(cache::kRootInode, "d1", 10));
  ASSERT_OK_AND_ASSIGN(InodeId d2, MakeDir(cache::kRootInode, "d2", 11));
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(20, S_IFREG | 0644));
  ASSERT_THAT(cache::LinkDentry(ctx_, d1, "f", f), IsOk());
  StartTrace();

  ASSERT_THAT(cache::InvalidateInode(ctx_, f), IsOk());
  Tick();
  EXPECT_THAT(Lines(d1), Contains(AllOf(HasSubstr("\"ev\":\"cut\""),
                                        HasSubstr("invalidated: "))));
  EXPECT_THAT(Lines(d2), Not(Contains(HasSubstr("\"ev\":\"cut\""))));
  EXPECT_THAT(Lines(d2), Not(Contains(HasSubstr("unexplained"))));
}

// Inside a transaction (an upsert invalidating a recycled inode number does
// that): the recorder looks at nothing until the transaction committed, and
// then a change beyond the forgotten names is unexplained, in the
// directory that named it and in any other.
TEST_F(TraceRecorderTest, ForgottenInodeInATransactionHidesNothing) {
  ASSERT_OK_AND_ASSIGN(InodeId d1, MakeDir(cache::kRootInode, "d1", 10));
  ASSERT_OK_AND_ASSIGN(InodeId d2, MakeDir(cache::kRootInode, "d2", 11));
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(20, S_IFREG | 0644));
  ASSERT_OK_AND_ASSIGN(InodeId g, Make(21, S_IFREG | 0644));
  ASSERT_THAT(cache::LinkDentry(ctx_, d1, "f", f), IsOk());
  StartTrace();

  ASSERT_THAT(ctx_.db.Transaction([&]() -> absl::Status {
    // Writes no event explains, one in each directory.
    ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx_, d2, "x", g));
    ABSL_RETURN_IF_ERROR(cache::InvalidateInode(ctx_, f));
    return cache::LinkDentry(ctx_, d1, "y", g);
  }),
              IsOk());
  Tick();
  EXPECT_THAT(Lines(d2), Contains(HasSubstr("\"ev\":\"unexplained\"")));
  EXPECT_THAT(Lines(d2), Not(Contains(HasSubstr("\"ev\":\"cut\""))));
  EXPECT_THAT(Lines(d1), Contains(HasSubstr("\"ev\":\"unexplained\"")));
  EXPECT_THAT(Lines(d1), Not(Contains(HasSubstr("\"ev\":\"cut\""))));
}

}  // namespace
}  // namespace dcfs::testonly
