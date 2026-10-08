// Tests of the trace recorder's own decisions (trace_recorder.h): which
// changes of a directory's cached state it explains, cuts or calls
// unexplained, which files' events become lines of a file's trace
// (formal/reval.tla), and which nodeids' lifetime steps become lines of a
// nodeid's trace (formal/lifetime.tla). The events are produced by the
// cache functions themselves (they call Context::events), or called
// directly, on an in-memory database; the lines are read back from a file
// in TEST_TMPDIR.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include "dcfs/testonly/trace_recorder.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include "absl/strings/str_split.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "fuse_lowlevel.h"  // FUSE_SET_ATTR_*
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs::testonly {
namespace {

using ::absl_testing::IsOk;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;
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
    ASSERT_OK_AND_ASSIGN(
        fd_, syscalls::openat(AT_FDCWD, path_,
                              O_WRONLY | O_CREAT | O_TRUNC, 0600));
  }

  void TearDown() override {
    ctx_.events = &NoProtocolEvents();
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

  // Starts recording; every directory's trace begins now. With `files`,
  // files' traces too; with `lifetimes`, nodeids' traces; with
  // `identities`, nodeids' identity traces.
  void StartTrace(bool files = false, bool lifetimes = false,
                  bool identities = false, bool directories = true) {
    recorder_ = std::make_unique<TraceRecorder>(
        *fd_, "test", files, lifetimes, identities, directories);
    ctx_.events = recorder_.get();
    recorder_->BeginAll(ctx_);
  }

  // Some callback outside every transaction (an empty request), at which
  // the recorder checks what changed.
  void Tick() {
    events::RequestScope scope(*ctx_.events, ctx_,
                               {.op = events::Op::kStatfs, .ino = 999});
  }

  // The lines of the trace file that start with `prefix`.
  std::vector<std::string> LinesWithPrefix(const std::string &prefix) {
    std::vector<std::string> lines;
    absl::StatusOr<std::string> contents = ReadFileToString(path_);
    EXPECT_THAT(contents, IsOk());
    for (std::string_view line :
         absl::StrSplit(contents.value_or(""), '\n', absl::SkipEmpty())) {
      if (absl::StartsWith(line, prefix)) lines.emplace_back(line);
    }
    return lines;
  }

  // The lines of directory `dir`'s trace so far.
  std::vector<std::string> Lines(InodeId dir) {
    return LinesWithPrefix(absl::StrCat("DCFS-TRACE test ", dir, " "));
  }

  // The lines of file `id`'s trace so far.
  std::vector<std::string> FileLines(InodeId id) {
    return LinesWithPrefix(absl::StrCat("DCFS-REVAL test ", id, " "));
  }

  // The lines of nodeid `id`'s lifetime trace so far.
  std::vector<std::string> LifeLines(InodeId id) {
    return LinesWithPrefix(absl::StrCat("DCFS-LIFE test ", id, " "));
  }

  // The lines of nodeid `id`'s identity trace so far.
  std::vector<std::string> IdentLines(InodeId id) {
    return LinesWithPrefix(absl::StrCat("DCFS-IDENT test ", id, " "));
  }

  // A FUSE request of file `id` that ends with `status`.
  void FileRequest(events::Request request, absl::Status status) {
    events::RequestScope scope(*ctx_.events, ctx_, request);
    scope.Finish(std::move(status)).IgnoreError();
  }

  sqlite3::Connection db_;
  MountFds mounts_;
  absl::BitGen bitgen_;
  Context ctx_{db_, mounts_, bitgen_};
  std::unique_ptr<TraceRecorder> recorder_;
  std::string path_;
  FileDescriptor fd_;
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

// --- Fills over valid attributes (review of 2026-10-06, finding 4) -------

// A refresh of valid attributes has no line (the model would serve them),
// but its fill may record only if no mutation of the directory began or
// ended since its snapshot: one that records after a mutation is
// unexplained.
TEST_F(TraceRecorderTest, SilentRefreshThatRecordsOverAMutationIsUnexplained) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();

  recorder_->RefreshBegin(ctx_, d);  // Valid attributes: silent.
  {
    // A whole mkdir in d, inside the refresh (at its statx, say).
    events::RequestScope request(
        *ctx_.events, ctx_, {.op = events::Op::kMkdir, .ino = d, .name = "x"});
    ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                         cache::BeginCreate(ctx_, d, "x"));
    ctx_.events->MutationSyscallStarting(ctx_);
    ctx_.events->MutationSyscall(ctx_, absl::OkStatus());
    mutation.End();
  }
  recorder_->AttrsStatted(ctx_, d);
  recorder_->AttrsFilled(ctx_, d, /*recorded=*/true);
  recorder_->RefreshEnd(ctx_, absl::OkStatus());
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"unexplained\""),
                                       HasSubstr("\"c\":\"AttrsFilled\""))));
}

// The same refresh, with no mutation meanwhile: nothing to say.
TEST_F(TraceRecorderTest, SilentRefreshWithNothingMeanwhileIsSilent) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();

  recorder_->RefreshBegin(ctx_, d);
  recorder_->AttrsStatted(ctx_, d);
  recorder_->AttrsFilled(ctx_, d, /*recorded=*/true);
  recorder_->RefreshEnd(ctx_, absl::OkStatus());
  EXPECT_THAT(Lines(d), Not(Contains(HasSubstr("\"ev\":\"unexplained\""))));
  EXPECT_THAT(Lines(d), Not(Contains(HasSubstr("\"ev\":\"cut\""))));
}

// A child row's fill reports the code's own decision; recording it
// although a mutation of the child ran since the fill's snapshot is
// unexplained (not a stutter over valid attributes).
TEST_F(TraceRecorderTest, ChildRowFilledAgainstTheGuardIsUnexplained) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();

  const cache::FillSnapshot snapshot = cache::BeginFill(ctx_);
  recorder_->PopulateStarted(ctx_, cache::kRootInode);  // The snapshot's event.
  {
    events::RequestScope request(
        *ctx_.events, ctx_, {.op = events::Op::kMkdir, .ino = d, .name = "x"});
    ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                         cache::BeginCreate(ctx_, d, "x"));
    ctx_.events->MutationSyscallStarting(ctx_);
    ctx_.events->MutationSyscall(ctx_, absl::OkStatus());
    mutation.End();
  }
  // The root's listing records d's row as filled anyway.
  recorder_->ChildRowRecorded(ctx_, cache::kRootInode, d, /*filled=*/true);
  recorder_->PopulateCommitted(ctx_, cache::kRootInode, snapshot.seq,
                               /*recorded=*/false);
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"unexplained\""),
                                       HasSubstr("since the fill's snapshot"))));
}

// The guard check uses the recorder's own record of d's mutations since
// the fill's snapshot event, not cache::CanFill with the snapshot the code
// reports: a snapshot taken too late (after the mutation) would make
// CanFill agree with a fill that the model's whole getattr, snapshotted at
// the event, does not allow.
TEST_F(TraceRecorderTest, ChildRowFilledWithALateSnapshotIsUnexplained) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();

  recorder_->PopulateStarted(ctx_, cache::kRootInode);  // The snapshot's event.
  {
    events::RequestScope request(
        *ctx_.events, ctx_, {.op = events::Op::kMkdir, .ino = d, .name = "x"});
    ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                         cache::BeginCreate(ctx_, d, "x"));
    ctx_.events->MutationSyscallStarting(ctx_);
    ctx_.events->MutationSyscall(ctx_, absl::OkStatus());
    mutation.End();
  }
  // The code's snapshot, taken only now.
  const cache::FillSnapshot late = cache::BeginFill(ctx_);
  recorder_->ChildRowRecorded(ctx_, cache::kRootInode, d, /*filled=*/true);
  recorder_->PopulateCommitted(ctx_, cache::kRootInode, late.seq,
                               /*recorded=*/false);
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"unexplained\""),
                                       HasSubstr("since the fill's snapshot"))));
}

// A fill whose snapshot event the recorder never saw has nothing to be
// checked against: recording is unexplained.
TEST_F(TraceRecorderTest, ChildRowFilledWithoutItsSnapshotEventIsUnexplained) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();

  const cache::FillSnapshot snapshot = cache::BeginFill(ctx_);
  recorder_->ChildRowRecorded(ctx_, cache::kRootInode, d, /*filled=*/true);
  recorder_->PopulateCommitted(ctx_, cache::kRootInode, snapshot.seq,
                               /*recorded=*/false);
  EXPECT_THAT(Lines(d),
              Contains(AllOf(HasSubstr("\"ev\":\"unexplained\""),
                             HasSubstr("snapshot event was not seen"))));
}

// ParentOf's fill of the parent row is checked from its own snapshot
// event (ParentLookupStarted): with no mutation since, a child_fill line;
// with one since, unexplained.
TEST_F(TraceRecorderTest, ParentRowFilledIsCheckedFromParentLookupStarted) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  ASSERT_OK_AND_ASSIGN(InodeId e, MakeDir(d, "e", 11));
  StartTrace();

  uint64_t snapshot = cache::BeginFill(ctx_).seq;
  recorder_->ParentLookupStarted(ctx_, e);
  recorder_->ParentRecorded(ctx_, e, d, snapshot, /*filled=*/true);
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"child_fill\""),
                                       HasSubstr("\"filled\":true"))));
  EXPECT_THAT(Lines(d), Not(Contains(HasSubstr("\"ev\":\"unexplained\""))));

  recorder_->ParentLookupStarted(ctx_, e);
  {
    events::RequestScope request(
        *ctx_.events, ctx_, {.op = events::Op::kMkdir, .ino = d, .name = "x"});
    ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                         cache::BeginCreate(ctx_, d, "x"));
    ctx_.events->MutationSyscallStarting(ctx_);
    ctx_.events->MutationSyscall(ctx_, absl::OkStatus());
    mutation.End();
  }
  snapshot = cache::BeginFill(ctx_).seq;  // Late, as in the test above.
  recorder_->ParentRecorded(ctx_, e, d, snapshot, /*filled=*/true);
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"unexplained\""),
                                       HasSubstr("since the fill's snapshot"))));
}

// A mutation of the directory in flight at the fill (its phase 1 before the
// snapshot event, its end after the fill) makes recording unexplained too.
TEST_F(TraceRecorderTest, ParentRowFilledOverAMutationInFlightIsUnexplained) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  ASSERT_OK_AND_ASSIGN(InodeId e, MakeDir(d, "e", 11));
  StartTrace();

  events::RequestScope request(
      *ctx_.events, ctx_, {.op = events::Op::kMkdir, .ino = d, .name = "x"});
  ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                       cache::BeginCreate(ctx_, d, "x"));
  const uint64_t snapshot = cache::BeginFill(ctx_).seq;
  recorder_->ParentLookupStarted(ctx_, e);
  recorder_->ParentRecorded(ctx_, e, d, snapshot, /*filled=*/true);
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"unexplained\""),
                                       HasSubstr("since the fill's snapshot"))));
  ctx_.events->MutationSyscallStarting(ctx_);
  ctx_.events->MutationSyscall(ctx_, absl::OkStatus());
  mutation.End();
}

// --- A directory named as an object (re-review of 2026-10-07, finding 3) --

// A mutation that names directory d as an object (an rmdir's child, a
// rename's source) ends d's trace (dir-itself) only if the request resolved
// one of its names to d; otherwise it is unexplained (a wrong id passed to
// a Begin* function would otherwise just cut d).
TEST_F(TraceRecorderTest, DirectoryNamedByAnUnresolvedRequestIsUnexplained) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();
  {
    events::RequestScope request(
        *ctx_.events, ctx_,
        {.op = events::Op::kRmdir, .ino = cache::kRootInode, .name = "x"});
    ASSERT_OK_AND_ASSIGN(
        cache::Mutation mutation,
        cache::BeginRemove(ctx_, cache::kRootInode, "x", d,
                           cache::BeginFill(ctx_)));
    request.Finish(absl::InternalError("stop here")).IgnoreError();
  }
  EXPECT_THAT(Lines(d), Contains(HasSubstr("\"ev\":\"unexplained\"")));
  EXPECT_THAT(Lines(d), Not(Contains(HasSubstr("dir-itself"))));
}

TEST_F(TraceRecorderTest, DirectoryNamedByTheRequestsResolveIsCut) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();
  {
    events::RequestScope request(
        *ctx_.events, ctx_,
        {.op = events::Op::kRmdir, .ino = cache::kRootInode, .name = "d"});
    {
      events::Scope lookup(*ctx_.events, ctx_, &ProtocolEvents::LookupBegin,
                           &ProtocolEvents::LookupEnd, cache::kRootInode,
                           std::string_view("d"));
      ctx_.events->LookupDecided(ctx_, cache::kRootInode, "d",
                                 events::LookupOutcome::kFound, d);
    }
    ASSERT_OK_AND_ASSIGN(
        cache::Mutation mutation,
        cache::BeginRemove(ctx_, cache::kRootInode, "d", d,
                           cache::BeginFill(ctx_)));
    request.Finish(absl::InternalError("stop here")).IgnoreError();
  }
  EXPECT_THAT(Lines(d), Contains(HasSubstr("dir-itself: ")));
}

// --- IOCTL of a directory (review of Phase 23, tests) --------------------

// A read-only ioctl of D (lsattr's FS_IOC_GETFLAGS) changes nothing: its
// getattr is the model's getattr, not a cut. Only a set (chattr) is a
// mutation of D's attributes the model does not have.
TEST_F(TraceRecorderTest, ReadOnlyIoctlOfADirectoryIsNoCut) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();
  {
    events::RequestScope request(
        *ctx_.events, ctx_,
        {.op = events::Op::kIoctl, .ino = d, .flags = FS_IOC_GETFLAGS});
    recorder_->GetattrBegin(ctx_, d, /*valid=*/true);
    recorder_->GetattrEnd(ctx_, absl::OkStatus());
  }
  EXPECT_THAT(Lines(d), Not(Contains(HasSubstr("\"ev\":\"cut\""))));
  {
    events::RequestScope request(
        *ctx_.events, ctx_,
        {.op = events::Op::kIoctl, .ino = d, .flags = FS_IOC_SETFLAGS});
    recorder_->GetattrBegin(ctx_, d, /*valid=*/true);
    recorder_->GetattrEnd(ctx_, absl::OkStatus());
  }
  EXPECT_THAT(Lines(d), Contains(AllOf(HasSubstr("\"ev\":\"cut\""),
                                       HasSubstr("dir-attrs"))));
}

// --- Files' traces (formal/reval.tla) ------------------------------------

constexpr InodeId kFile = 50;

events::SharedFd ReadWrite(int refs, int writable_refs) {
  return {.held = true,
          .writable = true,
          .refs = refs,
          .writable_refs = writable_refs};
}

// A file's trace begins at an open that finds no shared backing fd (the
// model's initial state has none); a file first seen shared is not traced,
// and nothing is unless the recorder was made with `files`.
TEST_F(TraceRecorderTest, FileTraceBeginsAtAnOpenWithoutASharedFd) {
  StartTrace(/*files=*/false);
  recorder_->FileOpened(ctx_, kFile, O_RDWR, /*shared=*/false,
                        absl::OkStatus(), ReadWrite(1, 1));
  EXPECT_THAT(FileLines(kFile), IsEmpty());

  StartTrace(/*files=*/true);
  recorder_->FileOpened(ctx_, kFile, O_RDONLY, /*shared=*/true,
                        absl::OkStatus(), ReadWrite(2, 1));
  recorder_->NoteOutOfBand(kFile);
  EXPECT_THAT(FileLines(kFile), IsEmpty());

  constexpr InodeId kOther = 51;
  recorder_->FileOpened(ctx_, kOther, O_WRONLY | O_APPEND, /*shared=*/false,
                        absl::OkStatus(), ReadWrite(1, 1));
  recorder_->NoteOutOfBand(kOther);
  recorder_->FileReleased(ctx_, kOther, /*writable=*/true, {});
  const std::vector<std::string> lines = FileLines(kOther);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_THAT(lines[0], HasSubstr("\"ev\":\"begin\""));
  EXPECT_THAT(lines[1],
              AllOf(HasSubstr("\"ev\":\"open\",\"mode\":\"wa\""),
                    HasSubstr("\"shared\":false,\"errno\":0"),
                    HasSubstr("{\"sfd\":\"rw\",\"wfd\":\"none\","
                              "\"refs\":1,\"wrefs\":1}")));
  EXPECT_THAT(lines[2], HasSubstr("\"ev\":\"oob\""));
  EXPECT_THAT(lines[3],
              AllOf(HasSubstr("\"ev\":\"release\",\"writable\":true"),
                    HasSubstr("\"sfd\":\"none\"")));
}

// An open refused for the backing file's flags (EPERM) is the model's; any
// other failure is not, and ends the file's trace.
TEST_F(TraceRecorderTest, FileOpenFailingOtherwiseThanEpermIsCut) {
  StartTrace(/*files=*/true);
  recorder_->FileOpened(ctx_, kFile, O_RDWR, /*shared=*/false,
                        absl::OkStatus(), ReadWrite(1, 1));
  recorder_->FileOpened(ctx_, kFile, O_WRONLY, /*shared=*/true,
                        ErrnoToStatus(EPERM, "immutable"), ReadWrite(1, 1));
  EXPECT_THAT(FileLines(kFile),
              Contains(HasSubstr("\"mode\":\"w\",\"shared\":true,"
                                 "\"errno\":1,")));
  recorder_->FileOpened(ctx_, kFile, O_WRONLY, /*shared=*/true,
                        ErrnoToStatus(ESTALE, "gone"), ReadWrite(1, 1));
  recorder_->FileReleased(ctx_, kFile, /*writable=*/true, {});
  const std::vector<std::string> lines = FileLines(kFile);
  ASSERT_FALSE(lines.empty());
  EXPECT_THAT(lines.back(), AllOf(HasSubstr("\"ev\":\"cut\""),
                                  HasSubstr("failed: the open failed")));
}

// The requests of a traced file that the model has: flag changes (the
// flags SETFLAGS set; FSSETXATTR's left free), flag reads, a SETATTR of the
// mode or owner (not of the size), and dcfs's own writes (EBADF is the
// model's; another error ends the trace).
TEST_F(TraceRecorderTest, FileRequestsBecomeLinesOfItsTrace) {
  StartTrace(/*files=*/true);
  recorder_->FileOpened(ctx_, kFile, O_RDWR, /*shared=*/false,
                        absl::OkStatus(), ReadWrite(1, 1));
  FileRequest({.op = events::Op::kIoctl,
               .ino = kFile,
               .flags = FS_IOC_SETFLAGS,
               .ioctl_arg = FS_IMMUTABLE_FL},
              absl::OkStatus());
  FileRequest({.op = events::Op::kIoctl,
               .ino = kFile,
               .flags = FS_IOC_FSSETXATTR},
              ErrnoToStatus(EPERM, "x"));
  FileRequest({.op = events::Op::kIoctl,
               .ino = kFile,
               .flags = FS_IOC_GETFLAGS},
              absl::OkStatus());
  FileRequest({.op = events::Op::kIoctl,
               .ino = kFile,
               .flags = FS_IOC_GETVERSION},
              absl::OkStatus());
  FileRequest({.op = events::Op::kSetattr,
               .ino = kFile,
               .flags = FUSE_SET_ATTR_SIZE},
              absl::OkStatus());
  FileRequest({.op = events::Op::kSetattr,
               .ino = kFile,
               .flags = FUSE_SET_ATTR_UID},
              ErrnoToStatus(EPERM, "x"));
  FileRequest({.op = events::Op::kFallocate, .ino = kFile},
              ErrnoToStatus(EBADF, "x"));
  FileRequest({.op = events::Op::kWrite, .ino = kFile}, absl::OkStatus());
  FileRequest({.op = events::Op::kCopyFileRange, .ino = kFile},
              ErrnoToStatus(EIO, "x"));
  FileRequest({.op = events::Op::kWrite, .ino = kFile}, absl::OkStatus());
  const std::vector<std::string> lines = FileLines(kFile);
  ASSERT_EQ(lines.size(), 9u);
  EXPECT_THAT(lines[2], HasSubstr("\"ev\":\"setflags\",\"errno\":0,"
                                  "\"imm\":true,\"app\":false}"));
  EXPECT_THAT(lines[3], HasSubstr("\"ev\":\"setflags\",\"errno\":1}"));
  EXPECT_THAT(lines[4], HasSubstr("\"ev\":\"getflags\",\"errno\":0}"));
  EXPECT_THAT(lines[5], HasSubstr("\"ev\":\"chmod\",\"errno\":1}"));
  EXPECT_THAT(lines[6], HasSubstr("\"ev\":\"write\",\"errno\":9}"));
  EXPECT_THAT(lines[7], HasSubstr("\"ev\":\"write\",\"errno\":0}"));
  EXPECT_THAT(lines[8], AllOf(HasSubstr("\"ev\":\"cut\""),
                              HasSubstr("failed: a write failed: errno 5")));
}

// --- Interrupts (formal/dcfs.tla's Interrupt) -----------------------------

// A checkpoint's interrupt (ProtocolEvents::Interrupted) is an "interrupt"
// line of each directory the request is the model's request of, and its
// EINTR reply a "reply" line, not a cut: a lookup interrupted before its
// population, and a mkdir interrupted between its phase 1 and its syscall,
// whose line comes with its End (the model's Interrupt Ends it).
TEST_F(TraceRecorderTest, InterruptedRequestsReplyEintr) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 10));
  StartTrace();
  {
    events::RequestScope request(
        *ctx_.events, ctx_, {.op = events::Op::kLookup, .ino = d, .name = "x"});
    events::Scope lookup(*ctx_.events, ctx_, &ProtocolEvents::LookupBegin,
                         &ProtocolEvents::LookupEnd, d, std::string_view("x"));
    ctx_.events->LookupDecided(ctx_, d, "x", events::LookupOutcome::kPopulate,
                               0);
    ctx_.events->Interrupted(ctx_);
    lookup.Finish(ErrnoToStatus(EINTR, "interrupted")).IgnoreError();
    request.Finish(ErrnoToStatus(EINTR, "interrupted")).IgnoreError();
  }
  {
    events::RequestScope request(
        *ctx_.events, ctx_, {.op = events::Op::kMkdir, .ino = d, .name = "y"});
    ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                         cache::BeginCreate(ctx_, d, "y"));
    ctx_.events->Interrupted(ctx_);
    mutation.End();
    request.Finish(ErrnoToStatus(EINTR, "interrupted")).IgnoreError();
  }
  const std::vector<std::string> lines = Lines(d);
  EXPECT_THAT(lines, Not(Contains(HasSubstr("\"ev\":\"cut\""))));
  EXPECT_THAT(lines, Not(Contains(HasSubstr("\"ev\":\"unexplained\""))));
  std::vector<std::string> evs;
  for (const std::string &line : lines) {
    const size_t at = line.find("\"ev\":\"");
    evs.push_back(line.substr(at + 6, line.find('"', at + 6) - at - 6));
  }
  EXPECT_THAT(evs, ::testing::ElementsAre("begin", "lookup", "interrupt",
                                          "reply", "phase1", "interrupt",
                                          "reply"));
  // The mkdir's interrupt line is written after its End: no mutation in
  // flight.
  EXPECT_THAT(lines[5], HasSubstr("\"inflight\":0"));
}

// --- Startup lines ---------------------------------------------------------

// A start in the same process after a clean shutdown (FinishRun, then
// StartRun): the start's lines carry the directory's state, so the clean
// flag StartRun clears is the start_run line's, not an unexplained change.
TEST_F(TraceRecorderTest, StartAfterACleanShutdownExplainsTheClearedFlag) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 70));
  ASSERT_THAT(SetCleanShutdown(db_, true), IsOk());
  StartTrace();
  recorder_->RunStarting(ctx_);
  recorder_->Recovered(ctx_);
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  recorder_->RunStarted(ctx_);
  Tick();
  EXPECT_THAT(Lines(d), Contains(HasSubstr("\"ev\":\"start_run\"")));
  EXPECT_THAT(Lines(d), Not(Contains(HasSubstr("\"ev\":\"unexplained\""))));
}

// The end of the start's probe (step 12.6b): a line of every directory
// (dcfs.tla's ProbesDone); the recovered rows stay dirty.
TEST_F(TraceRecorderTest, RecoveryDoneIsALineOfEveryDirectory) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 74));
  const InodeId dirty[] = {d};
  ASSERT_THAT(cache::MarkDirty(ctx_, dirty), IsOk());
  StartTrace();
  recorder_->RunStarting(ctx_);
  recorder_->Recovered(ctx_);
  recorder_->RunStarted(ctx_);
  recorder_->RecoveryDone(ctx_);
  Tick();
  const std::vector<std::string> lines = Lines(d);
  ASSERT_THAT(lines, Not(::testing::IsEmpty()));
  EXPECT_THAT(lines.back(), AllOf(HasSubstr("\"ev\":\"recovery_done\""),
                                  HasSubstr("\"dirty\":true")));
  EXPECT_THAT(lines, Not(Contains(HasSubstr("\"ev\":\"unexplained\""))));
}

// --- Nodeids' lifetime traces (formal/lifetime.tla) ----------------------

// What DirCacheFS reports it keeps for a nodeid after a step.
events::Lifetime Kept(uint64_t lookups, int refs = 0,
                      events::Lifetime::Written written =
                          events::Lifetime::Written::kNo,
                      bool removed = false) {
  return {.lookups = lookups,
          .removed = removed,
          .written = written,
          .refs = refs};
}

void Step(TraceRecorder &recorder, Context &ctx, InodeId id,
          events::LifetimeStep step, uint64_t arg,
          const events::Lifetime &after) {
  recorder.LifetimeChanged(ctx, id, step, arg, [&] { return after; });
}

// A nodeid's trace begins at the lookup dcfs counts first (lookups_ from 0
// to 1), in the state it had before it (the model's initial state: no
// lookup, nothing kept, its row as the database has it); one first seen
// with lookups counted already is not traced, and nothing is unless the
// recorder was made with `lifetimes`.
TEST_F(TraceRecorderTest, LifetimeTraceBeginsAtTheFirstCountedLookup) {
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(60, S_IFREG | 0644));
  ASSERT_OK_AND_ASSIGN(InodeId g, Make(61, S_IFREG | 0644));
  StartTrace(/*files=*/true, /*lifetimes=*/false);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  EXPECT_THAT(LifeLines(f), IsEmpty());

  StartTrace(/*files=*/false, /*lifetimes=*/true);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(2));
  Step(*recorder_, ctx_, f, events::LifetimeStep::kForgot, 2, Kept(0));
  EXPECT_THAT(LifeLines(f), IsEmpty());

  Step(*recorder_, ctx_, g, events::LifetimeStep::kLookup, 0, Kept(1));
  Step(*recorder_, ctx_, g, events::LifetimeStep::kOpened, 1,
       Kept(1, 1, events::Lifetime::Written::kNoFd));
  Step(*recorder_, ctx_, g, events::LifetimeStep::kReleased, 1,
       Kept(1, 0, events::Lifetime::Written::kHeld));
  Step(*recorder_, ctx_, g, events::LifetimeStep::kForgot, 1, Kept(0));
  const std::vector<std::string> lines = LifeLines(g);
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_THAT(lines[0],
              AllOf(HasSubstr("\"ev\":\"begin\""),
                    HasSubstr("\"st\":{\"lk\":0,\"rec\":false,"
                              "\"wr\":\"no\",\"refs\":0,\"row\":true,"
                              "\"nl0\":false}")));
  EXPECT_THAT(lines[1],
              AllOf(HasSubstr("\"ev\":\"lookup\",\"via\":\"lookup\""),
                    HasSubstr("\"st\":{\"lk\":1,\"rec\":false,"
                              "\"wr\":\"no\",\"refs\":0,\"row\":true,"
                              "\"nl0\":false}")));
  EXPECT_THAT(lines[2], AllOf(HasSubstr("\"ev\":\"open\",\"w\":true"),
                              HasSubstr("\"wr\":\"nofd\",\"refs\":1")));
  EXPECT_THAT(lines[3],
              AllOf(HasSubstr("\"ev\":\"release\",\"w\":true"),
                    HasSubstr("\"wr\":\"held\",\"refs\":0")));
  EXPECT_THAT(lines[4],
              AllOf(HasSubstr("\"ev\":\"forget\",\"n\":1,"
                              "\"batch\":false"),
                    HasSubstr("\"lk\":0,")));
}

// A CREATE or TMPFILE begins its nodeid's trace before the row it made
// (the model's Create and Tmpfile make it); a stub's nodeid is not traced.
TEST_F(TraceRecorderTest, LifetimeTraceOfACreatedNodeidBeginsWithoutItsRow) {
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(62, S_IFREG | 0644));
  StartTrace(/*files=*/false, /*lifetimes=*/true);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kCreated, 0, Kept(1, 1));
  const std::vector<std::string> lines = LifeLines(f);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_THAT(lines[0], AllOf(HasSubstr("\"ev\":\"begin\""),
                              HasSubstr("\"row\":false,\"nl0\":false}")));
  EXPECT_THAT(lines[1], AllOf(HasSubstr("\"ev\":\"create\",\"w\":false"),
                              HasSubstr("\"refs\":1,\"row\":true,")));

  constexpr InodeId kStub = -5;
  Step(*recorder_, ctx_, kStub, events::LifetimeStep::kLookup, 0, Kept(1));
  EXPECT_THAT(LifeLines(kStub), IsEmpty());
}

// How an entry reply handed the nodeid out, from the request it answered:
// a LINK (the model's Link), a LOOKUP of "." or ".." (no name of the
// object), or by a name.
TEST_F(TraceRecorderTest, LifetimeLookupSaysWhichRequestHandedItOut) {
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(63, S_IFREG | 0644));
  StartTrace(/*files=*/false, /*lifetimes=*/true);
  {
    events::RequestScope scope(*ctx_.events, ctx_,
                               {.op = events::Op::kLookup, .ino = f,
                                .name = "."});
    Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  }
  {
    events::RequestScope scope(*ctx_.events, ctx_,
                               {.op = events::Op::kLinkTmpfile, .ino = f,
                                .newparent = cache::kRootInode,
                                .newname = "n"});
    Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(2));
  }
  {
    events::RequestScope scope(
        *ctx_.events, ctx_,
        {.op = events::Op::kReaddirplus, .ino = cache::kRootInode});
    Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(3));
  }
  const std::vector<std::string> lines = LifeLines(f);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_THAT(lines[1], HasSubstr("\"via\":\"dot\""));
  EXPECT_THAT(lines[2], HasSubstr("\"via\":\"link\""));
  EXPECT_THAT(lines[3], HasSubstr("\"via\":\"lookup\""));
}

// A removal's phase 3, a FORGET_MULTI's entry, DESTROY, and a start after a
// crash: each a line of every nodeid trace it concerns (the last two of
// all of them), the run's lines with the row as the database has it.
TEST_F(TraceRecorderTest, LifetimeRemovalBatchDestroyAndRestartLines) {
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(64, S_IFREG | 0644));
  StartTrace(/*files=*/false, /*lifetimes=*/true);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(2));
  ASSERT_THAT(cache::DeleteInode(ctx_, f), IsOk());
  Step(*recorder_, ctx_, f, events::LifetimeStep::kRemoved, 1,
       Kept(2, 0, events::Lifetime::Written::kNo, /*removed=*/true));
  Step(*recorder_, ctx_, f, events::LifetimeStep::kForgotInBatch, 1,
       Kept(1, 0, events::Lifetime::Written::kNo, /*removed=*/true));
  recorder_->Destroyed(ctx_);
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  recorder_->RunStarting(ctx_);
  recorder_->RunStarted(ctx_);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kProbed, 1, Kept(0));
  const std::vector<std::string> lines = LifeLines(f);
  ASSERT_EQ(lines.size(), 9u);
  EXPECT_THAT(lines[8], AllOf(HasSubstr("\"ev\":\"probe\",\"gone\":true"),
                              HasSubstr("\"lk\":0,")));
  EXPECT_THAT(lines[3],
              AllOf(HasSubstr("\"ev\":\"removed\",\"held\":true"),
                    HasSubstr("\"lk\":2,\"rec\":true,"),
                    HasSubstr("\"row\":false,")));
  EXPECT_THAT(lines[4], HasSubstr("\"ev\":\"forget\",\"n\":1,"
                                  "\"batch\":true"));
  EXPECT_THAT(lines[5], AllOf(HasSubstr("\"ev\":\"destroy\""),
                              HasSubstr("\"st\":{\"row\":false,"
                                        "\"nl0\":false}")));
  EXPECT_THAT(lines[6], HasSubstr("\"ev\":\"crash\""));
  EXPECT_THAT(lines[7], AllOf(HasSubstr("\"ev\":\"start\""),
                              HasSubstr("\"st\":{\"row\":false,"
                                        "\"nl0\":false}")));
}

// --- Nodeids' identity traces (formal/ident.tla) ------------------------

// An identity trace begins at the entry reply dcfs counts first, and only
// with `identities`; each reply carries the generation the row has (as a
// string: a uint32), each FORGET the lookups left, and a stub is not
// traced.
TEST_F(TraceRecorderTest, IdentityTraceRecordsRepliesAndForgets) {
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(70, S_IFREG | 0644));
  ASSERT_OK_AND_ASSIGN(uint32_t gen, cache::GetGeneration(ctx_, f));
  StartTrace(/*files=*/false, /*lifetimes=*/true, /*identities=*/false);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  EXPECT_THAT(IdentLines(f), IsEmpty());

  StartTrace(/*files=*/false, /*lifetimes=*/false, /*identities=*/true);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(2));
  Step(*recorder_, ctx_, f, events::LifetimeStep::kOpened, 0, Kept(2, 1));
  Step(*recorder_, ctx_, f, events::LifetimeStep::kForgot, 2, Kept(0));
  constexpr InodeId kStub = -5;
  Step(*recorder_, ctx_, kStub, events::LifetimeStep::kLookup, 0, Kept(1));
  EXPECT_THAT(IdentLines(kStub), IsEmpty());
  const std::vector<std::string> lines = IdentLines(f);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_THAT(lines[0], AllOf(HasSubstr("\"ev\":\"begin\""),
                              HasSubstr("\"st\":{\"row\":true}")));
  const std::string reply = absl::StrCat(
      "\"ev\":\"reply\",\"via\":\"lookup\",\"fgen\":\"", gen, "\"");
  EXPECT_THAT(lines[1], AllOf(HasSubstr(reply),
                              HasSubstr("\"st\":{\"row\":true,\"lk\":1}")));
  EXPECT_THAT(lines[2], HasSubstr("\"st\":{\"row\":true,\"lk\":2}"));
  EXPECT_THAT(lines[3], AllOf(HasSubstr("\"ev\":\"forget\""),
                              HasSubstr("\"st\":{\"row\":true,\"lk\":0}")));
}

// A reopen by handle is a resolve line: its outcome, and how what it
// reached compares with the row (the inode number, and the generation and
// birth time where both sides know them); a row that goes is a gone line,
// and DESTROY and a start are the run's lines.
TEST_F(TraceRecorderTest, IdentityTraceRecordsResolutionsAndRowsGoing) {
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(71, S_IFREG | 0644));
  StartTrace(/*files=*/false, /*lifetimes=*/false, /*identities=*/true);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  recorder_->IdentityResolved(
      ctx_, f,
      {.outcome = events::IdentityCheck::Outcome::kServed,
       .row_ino = 71, .row_gen = 0, .row_btime_sec = 1071,
       .row_btime_nsec = 7, .found_ino = 71, .found_gen = 0,
       .found_btime_known = true, .found_btime_sec = 1071,
       .found_btime_nsec = 7});
  recorder_->IdentityResolved(
      ctx_, f,
      {.outcome = events::IdentityCheck::Outcome::kMismatch,
       .row_ino = 71, .row_gen = 5, .row_btime_sec = 1071,
       .row_btime_nsec = 7, .found_ino = 71, .found_gen = 6,
       .found_btime_known = true, .found_btime_sec = 1071,
       .found_btime_nsec = 8});
  recorder_->IdentityResolved(
      ctx_, f,
      {.outcome = events::IdentityCheck::Outcome::kMismatch,
       .row_ino = 71, .row_gen = 5, .row_btime_sec = 1071,
       .row_btime_nsec = 7, .found_ino = 72, .found_gen = 0,
       .found_btime_known = false});
  recorder_->IdentityResolved(
      ctx_, f, {.outcome = events::IdentityCheck::Outcome::kStaleHandle,
                .row_ino = 71});
  ASSERT_THAT(cache::DeleteInode(ctx_, f), IsOk());
  recorder_->Destroyed(ctx_);
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  recorder_->RunStarting(ctx_);
  recorder_->RunStarted(ctx_);
  const std::vector<std::string> lines = IdentLines(f);
  ASSERT_EQ(lines.size(), 10u);
  EXPECT_THAT(lines[2],
              HasSubstr("\"ev\":\"resolve\",\"outcome\":\"served\","
                        "\"ino\":\"same\",\"gen\":\"unknown\","
                        "\"bt\":\"same\",\"st\":{\"row\":true}"));
  EXPECT_THAT(lines[3],
              HasSubstr("\"outcome\":\"mismatch\",\"ino\":\"same\","
                        "\"gen\":\"other\",\"bt\":\"other\""));
  EXPECT_THAT(lines[4],
              HasSubstr("\"outcome\":\"mismatch\",\"ino\":\"other\","
                        "\"gen\":\"unknown\",\"bt\":\"unknown\""));
  EXPECT_THAT(lines[5], HasSubstr("\"outcome\":\"stale_handle\","));
  EXPECT_THAT(lines[6], AllOf(HasSubstr("\"ev\":\"gone\""),
                              HasSubstr("\"st\":{\"row\":false}")));
  EXPECT_THAT(lines[7], HasSubstr("\"ev\":\"destroy\""));
  EXPECT_THAT(lines[8], HasSubstr("\"ev\":\"crash\""));
  EXPECT_THAT(lines[9], AllOf(HasSubstr("\"ev\":\"start\""),
                              HasSubstr("\"st\":{\"row\":false}")));
}

// Without `directories`, no directory's trace is written (an identity
// scenario's invalidation would cut them), and the other traces are.
TEST_F(TraceRecorderTest, WithoutDirectoriesOnlyTheOtherTracesAreWritten) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(cache::kRootInode, "d", 72));
  ASSERT_OK_AND_ASSIGN(InodeId f, Make(73, S_IFREG | 0644));
  ASSERT_THAT(cache::LinkDentry(ctx_, d, "f", f), IsOk());
  StartTrace(/*files=*/false, /*lifetimes=*/true, /*identities=*/true,
             /*directories=*/false);
  Step(*recorder_, ctx_, f, events::LifetimeStep::kLookup, 0, Kept(1));
  ASSERT_THAT(cache::DeleteInode(ctx_, f), IsOk());
  Tick();
  EXPECT_THAT(LinesWithPrefix("DCFS-TRACE "), IsEmpty());
  EXPECT_THAT(IdentLines(f), SizeIs(3));
  EXPECT_THAT(LifeLines(f), SizeIs(2));
}

}  // namespace
}  // namespace dcfs::testonly
