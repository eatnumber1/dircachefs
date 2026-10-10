#include "dcfs/backing.h"

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <random>
#include <cstdio>
#include <thread>
#include <span>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "dcfs/context.h"
#include "dcfs/credentials.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs::backing {
namespace {


using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::ElementsAre;
using ::testing::Not;
using ::testing::Optional;
using cache::InodeId;
using cache::kRootInode;
using cache::LookupResult;

MATCHER_P(IsLookup, kind, "") { return arg.kind == kind; }

DeviceId OtherDevice() {
  DeviceId id;
  id.uuid.fill(0x5A);
  return id;
}

int ErrnoOf(const absl::Status &status) {
  return GetErrnoFromStatus(status).value_or(0);
}

struct statx StatPath(const std::string &path) {
  absl::StatusOr<struct statx> stx = syscalls::statx(
      AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME);
  EXPECT_THAT(stx, IsOk()) << path;
  return stx.ok() ? *stx : (struct statx){};
}

// setxattr(2) of a string value on `path`.
absl::Status SetUserXattr(const std::string &path, std::string_view name,
                          std::string_view value) {
  return syscalls::setxattr(
      path, name,
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t *>(value.data()), value.size()),
      0);
}

void WriteFile(const std::string &path, std::string_view contents) {
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor fd,
      syscalls::openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  EXPECT_THAT(syscalls::write(*fd, contents.data(), contents.size()),
              IsOkAndHolds(contents.size()));
}

class BackingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    std::string templ = absl::StrCat(tmpdir, "/backing_XXXXXX");
    ASSERT_OK_AND_ASSIGN(source_, syscalls::mkdtemp(templ));

    // The source tree every test starts from.
    WriteFile(Path("file"), "hello");
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("dir"), 0755), IsOk());
    WriteFile(Path("dir/inner"), "inner");
    ASSERT_THAT(syscalls::symlinkat("file", AT_FDCWD, Path("link")), IsOk());
    ASSERT_THAT(
        syscalls::mknodat(AT_FDCWD, Path("fifo"), S_IFIFO | 0644, 0), IsOk());
    WriteFile(Path("hl1"), "linked");
    ASSERT_THAT(
        syscalls::linkat(AT_FDCWD, Path("hl1"), AT_FDCWD, Path("hl2"), 0),
        IsOk());
    xattrs_supported_ = SetUserXattr(Path("file"), "user.test", "value").ok();

    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = DbPath()}.Open());
    // A real (non-O_PATH) fd: InitRoot registers it as the source
    // filesystem's mount fd, and open_by_handle_at's mount fd argument
    // rejects O_PATH (fs/fhandle.c get_path_from_fd()).
    ASSERT_OK_AND_ASSIGN(
        FileDescriptor owned,
        syscalls::openat(AT_FDCWD, source_, O_RDONLY | O_DIRECTORY));
    const int source_fd = *owned;
    ASSERT_OK_AND_ASSIGN(RootIdentity root, ProbeRoot(ctx_, source_fd));
    ASSERT_THAT(Migrate(db_, root), IsOk());
    ASSERT_THAT(InitRoot(ctx_, std::move(owned)), IsOk());
    source_fd_ = source_fd;  // Now owned by mounts_.
  }

  void TearDown() override {
    Unlock();
    if (!source_.empty()) testonly::RemoveAll(source_);
  }

  std::string Path(std::string_view rel) const {
    return absl::StrCat(source_, "/", rel);
  }

  // The cache database: in memory, unless a test needs a file (a second
  // connection, a WAL checkpoint).
  virtual std::string DbPath() const { return ":memory:"; }

  // Removes all permissions from everything in the source tree (children
  // before their directories, the source itself last), so that any backing
  // access from here on fails with EACCES for this unprivileged test.
  void Lock() {
    ASSERT_OK_AND_ASSIGN(std::vector<std::string> paths,
                         testonly::ListTree(source_));
    std::reverse(paths.begin(), paths.end());
    paths.push_back(source_);
    for (const std::string &path : paths) {
      ASSERT_OK_AND_ASSIGN(
          struct stat st,
          syscalls::fstatat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW));
      if (S_ISLNK(st.st_mode)) continue;
      locked_.emplace_back(path, st.st_mode & 07777);
      ASSERT_THAT(syscalls::fchmodat(AT_FDCWD, path, 0, 0), IsOk());
    }
  }

  void Unlock() {
    for (auto it = locked_.rbegin(); it != locked_.rend(); ++it) {
      syscalls::fchmodat(AT_FDCWD, it->first, it->second, 0).IgnoreError();
    }
    locked_.clear();
  }

  absl::StatusOr<InodeId> Id(std::string_view name, InodeId dir = kRootInode) {
    absl::StatusOr<LookupResult> result = LookupOrPopulate(ctx_, dir, name);
    if (!result.ok()) return result.status();
    if (result->kind != LookupResult::Kind::kFound) {
      return absl::NotFoundError(absl::StrCat(name, " not found"));
    }
    return result->id;
  }

  // Deletes `name` and creates a new file there. Returns whether the new
  // file got the old inode number back (ext4 usually reuses it at once).
  bool Recreate(std::string_view name) {
    struct statx before = StatPath(Path(name));
    EXPECT_THAT(syscalls::unlinkat(AT_FDCWD, Path(name), 0), IsOk());
    WriteFile(Path(name), "recreated");
    struct statx after = StatPath(Path(name));
    return before.stx_ino == after.stx_ino;
  }

  std::string source_;
  int source_fd_ = -1;
  bool xattrs_supported_ = false;
  std::vector<std::pair<std::string, mode_t>> locked_;
  sqlite3::Connection db_;
  MountFds mounts_;
  // Fixed seed: generations are random, but tests should be reproducible.
  absl::BitGen bitgen_{std::seed_seq{4, 10}};
  Context ctx_{db_, mounts_, bitgen_};
};

std::vector<std::string> ListNames(Context &ctx, InodeId dir) {
  std::vector<std::string> names;
  EXPECT_THAT(cache::ListDir(ctx, dir, 0,
                             [&](std::string_view name, InodeId, int64_t) {
                               names.emplace_back(name);
                               return true;
                             }),
              IsOk());
  std::sort(names.begin(), names.end());
  return names;
}

TEST_F(BackingTest, PopulatedDirectoryIsServedFromTheCache) {
  ASSERT_THAT(LookupOrPopulate(ctx_, kRootInode, "file"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  const std::vector<std::string> names = {"dir", "fifo", "file",
                                          "hl1", "hl2",  "link"};
  std::vector<struct statx> expected;
  for (const std::string &name : names) expected.push_back(StatPath(Path(name)));

  // Lock() (chmod 0) no longer proves the backing filesystem is
  // unreachable: dcfs always runs as root now, and root's CAP_DAC_OVERRIDE
  // bypasses file permission checks entirely. It is still exercised below
  // as a smoke check that cache reads need no permission on the backing
  // tree, but it can no longer serve as the "really would have failed"
  // control that it was when tests ran unprivileged.
  Lock();

  for (size_t i = 0; i < names.size(); ++i) {
    SCOPED_TRACE(names[i]);
    ASSERT_OK_AND_ASSIGN(InodeId id, Id(names[i]));
    ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, id));
    const struct statx &stx = expected[i];
    EXPECT_TRUE(attr.valid);
    EXPECT_EQ(attr.backing_ino, stx.stx_ino);
    EXPECT_EQ(attr.st.st_ino, stx.stx_ino);
    EXPECT_EQ(attr.st.st_mode, stx.stx_mode);
    EXPECT_EQ(attr.st.st_nlink, stx.stx_nlink);
    EXPECT_EQ(attr.st.st_uid, stx.stx_uid);
    EXPECT_EQ(attr.st.st_gid, stx.stx_gid);
    EXPECT_EQ(attr.st.st_size, static_cast<off_t>(stx.stx_size));
    EXPECT_EQ(attr.st.st_mtim.tv_sec, stx.stx_mtime.tv_sec);
    EXPECT_EQ(attr.st.st_mtim.tv_nsec, stx.stx_mtime.tv_nsec);
    EXPECT_EQ(attr.st.st_ctim.tv_sec, stx.stx_ctime.tv_sec);
    if ((stx.stx_mask & STATX_BTIME) != 0) {
      EXPECT_EQ(attr.btime.tv_sec, stx.stx_btime.tv_sec);
      EXPECT_EQ(attr.btime.tv_nsec, stx.stx_btime.tv_nsec);
    }
    EXPECT_EQ(attr.device, *GetDeviceId(source_fd_));
    EXPECT_THAT(cache::ListXattrs(ctx_, id), IsOkAndHolds(Optional(testing::_)));
  }

  ASSERT_OK_AND_ASSIGN(InodeId hl1, Id("hl1"));
  ASSERT_OK_AND_ASSIGN(InodeId hl2, Id("hl2"));
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(InodeId link, Id("link"));
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  EXPECT_EQ(hl1, hl2);
  EXPECT_NE(hl1, file);

  EXPECT_THAT(cache::Readlink(ctx_, link), IsOkAndHolds("file"));
  EXPECT_THAT(cache::Readlink(ctx_, file),
              StatusIs(absl::StatusCode::kNotFound));
  if (xattrs_supported_) {
    ASSERT_OK_AND_ASSIGN(std::optional<std::vector<std::string>> xattrs,
                         cache::ListXattrs(ctx_, file));
    ASSERT_TRUE(xattrs.has_value());
    EXPECT_THAT(*xattrs, Contains("user.test"));
    EXPECT_THAT(cache::GetXattr(ctx_, file, "user.test"),
                IsOkAndHolds(Optional(std::string("value"))));
    EXPECT_THAT(cache::GetXattr(ctx_, file, "user.absent"),
                StatusIs(absl::StatusCode::kNotFound));
  }

  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));
  EXPECT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(false));
  EXPECT_THAT(ListNames(ctx_, kRootInode),
              ElementsAre("dir", "fifo", "file", "hl1", "hl2", "link"));
}

TEST_F(BackingTest, MissingNameIsCachedAsNegative) {
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "missing"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "missing"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));

  Lock();
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "missing"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  // The listing is complete, so another absent name needs no I/O either.
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "also_missing"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
}

TEST_F(BackingTest, RepopulationTracksChangesOnDisk) {
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_THAT(cache::SetNegative(ctx_, kRootInode, "ghost"), IsOk());

  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("fifo"), 0), IsOk());
  WriteFile(Path("new"), "new");
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());

  // Gone from the listing, which is complete: absent.
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "fifo"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "ghost"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "new"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  EXPECT_THAT(Id("file"), IsOkAndHolds(file));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));
  EXPECT_THAT(ListNames(ctx_, kRootInode),
              ElementsAre("dir", "file", "hl1", "hl2", "link", "new"));
}

TEST_F(BackingTest, RecycledInodeNumberGetsANewRow) {
  ASSERT_OK_AND_ASSIGN(InodeId old_id, Id("file"));
  if (!Recreate("file")) GTEST_SKIP() << "the inode number was not reused";

  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId new_id, Id("file"));
  EXPECT_NE(new_id, old_id);
  EXPECT_THAT(cache::GetAttr(ctx_, old_id),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));
}

TEST_F(BackingTest, OpenNodeRejectsARecycledInode) {
  ASSERT_OK_AND_ASSIGN(InodeId old_id, Id("file"));
  ASSERT_THAT(OpenNode(ctx_, old_id, O_RDONLY), IsOk());
  if (!Recreate("file")) GTEST_SKIP() << "the inode number was not reused";

  absl::StatusOr<FileDescriptor> fd = OpenNode(ctx_, old_id, O_RDONLY);
  ASSERT_FALSE(fd.ok());
  EXPECT_EQ(ErrnoOf(fd.status()), ESTALE) << fd.status();
  EXPECT_THAT(cache::GetAttr(ctx_, old_id),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "file"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
}

// Records the identity checks OpenNode reports (ProtocolEvents::
// IdentityResolved), for the identity model's trace validation.
class IdentityChecks final : public ProtocolEvents {
 public:
  void IdentityResolved(Context &, events::Ino id,
                        const events::IdentityCheck &check) override {
    checks.emplace_back(id, check);
  }
  std::vector<std::pair<events::Ino, events::IdentityCheck>> checks;
};

// Every reopen by handle reports what it reached: the row's object
// (served), another object with the row's inode number (here: a row whose
// recorded generation is not the object's, as after a recycling the handle
// does not show), or nothing (the handle is stale: the object was freed).
TEST_F(BackingTest, OpenNodeReportsItsIdentityCheck) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  IdentityChecks events;
  ctx_.events = &events;

  ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
  ASSERT_EQ(events.checks.size(), 1u);
  EXPECT_EQ(events.checks[0].first, file);
  const events::IdentityCheck &served = events.checks[0].second;
  EXPECT_EQ(served.outcome, events::IdentityCheck::Outcome::kServed);
  EXPECT_EQ(served.row_ino, attr.backing_ino);
  EXPECT_EQ(served.found_ino, attr.backing_ino);
  EXPECT_EQ(served.row_gen, attr.backing_gen);
  EXPECT_EQ(served.found_gen, attr.backing_gen);
  EXPECT_EQ(served.row_btime_sec, attr.btime.tv_sec);
  EXPECT_EQ(served.found_btime_known, attr.btime.tv_sec != 0);
  EXPECT_EQ(served.found_btime_sec, attr.btime.tv_sec);

  if (attr.backing_gen != 0) {
    ASSERT_OK_AND_ASSIGN(
        sqlite3::Statement * doctor,
        db_.Prepared("UPDATE inodes SET backing_gen = ? WHERE id = ?"));
    ASSERT_THAT(doctor->BindAll(attr.backing_gen + 1, file), IsOk());
    ASSERT_THAT(doctor->ExecuteOnce(), IsOk());
    absl::StatusOr<FileDescriptor> fd = OpenNode(ctx_, file, O_RDONLY);
    EXPECT_EQ(ErrnoOf(fd.status()), ESTALE) << fd.status();
    ASSERT_EQ(events.checks.size(), 2u);
    const events::IdentityCheck &mismatch = events.checks[1].second;
    EXPECT_EQ(mismatch.outcome, events::IdentityCheck::Outcome::kMismatch);
    EXPECT_EQ(mismatch.row_gen, attr.backing_gen + 1);
    EXPECT_EQ(mismatch.found_gen, attr.backing_gen);
    EXPECT_EQ(mismatch.found_ino, attr.backing_ino);
    EXPECT_THAT(cache::GetAttr(ctx_, file),
                StatusIs(absl::StatusCode::kNotFound));
  }

  ASSERT_OK_AND_ASSIGN(InodeId again, Id("file"));
  Recreate("file");
  events.checks.clear();
  absl::StatusOr<FileDescriptor> fd = OpenNode(ctx_, again, O_RDONLY);
  EXPECT_EQ(ErrnoOf(fd.status()), ESTALE) << fd.status();
  ASSERT_EQ(events.checks.size(), 1u);
  EXPECT_EQ(events.checks[0].second.outcome,
            events::IdentityCheck::Outcome::kStaleHandle);
  ctx_.events = &NoProtocolEvents();
}

TEST_F(BackingTest, BackingReadsByInode) {
  // The root is reachable without handle decoding.
  ASSERT_OK_AND_ASSIGN(struct statx root_stx, StatNode(ctx_, kRootInode));
  EXPECT_EQ(root_stx.stx_ino, StatPath(source_).stx_ino);
  EXPECT_THAT(ReadXattrs(ctx_, kRootInode), IsOk());

  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(InodeId link, Id("link"));
  ASSERT_OK_AND_ASSIGN(InodeId fifo, Id("fifo"));
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  ASSERT_OK_AND_ASSIGN(struct statx file_stx, StatNode(ctx_, file));
  EXPECT_EQ(file_stx.stx_size, 5u);
  EXPECT_THAT(ReadSymlink(ctx_, link), IsOkAndHolds("file"));
  EXPECT_THAT(ReadXattrs(ctx_, link), IsOk());
  EXPECT_THAT(ReadXattrs(ctx_, fifo), IsOk());
  if (xattrs_supported_) {
    EXPECT_THAT(ReadXattrs(ctx_, file),
                IsOkAndHolds(Contains(std::make_pair(std::string("user.test"),
                                                     std::string("value")))));
  }
  // A subdirectory is populated through its handle.
  EXPECT_THAT(LookupOrPopulate(ctx_, dir, "inner"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  EXPECT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(true));
}

TEST_F(BackingTest, ReadGeneration) {
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor file,
      syscalls::openat(AT_FDCWD, Path("file"), O_PATH));
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor link,
      syscalls::openat(AT_FDCWD, Path("link"), O_PATH | O_NOFOLLOW));

  EXPECT_THAT(ReadGeneration(*link, S_IFLNK), IsOkAndHolds(0u));
  EXPECT_THAT(ReadGeneration(*file, S_IFIFO), IsOkAndHolds(0u));

  ASSERT_OK_AND_ASSIGN(struct statfs sfs, syscalls::fstatfs(*file));
  if (sfs.f_type != EXT4_SUPER_MAGIC) {
    GTEST_SKIP() << "generations are only known to be nonzero on ext4";
  }
  ASSERT_OK_AND_ASSIGN(uint64_t gen, ReadGeneration(*file, S_IFREG));
  EXPECT_NE(gen, 0u);
  // Stable across calls, which is what identity checks rely on.
  EXPECT_THAT(ReadGeneration(*file, S_IFREG), IsOkAndHolds(gen));
}

// Regression test: tmpfs doesn't implement FS_IOC_GETVERSION (ENOTTY), and
// ReadGeneration() must recognize that via the errno payload
// dcfs::ErrnoToStatus() attaches and report "generation unknown" (0) rather
// than propagating an error. TEST_TMPDIR may be a real disk (see
// test/qemu/guest/init), so this creates the file directly under /tmp,
// which is always tmpfs in the QEMU guest.
TEST_F(BackingTest, ReadGenerationOnTmpfsFileIsZero) {
  std::string path = "/tmp/dcfs_backing_test_tmpfs_XXXXXX";
  ASSERT_OK_AND_ASSIGN(FileDescriptor file, syscalls::mkstemp(path));

  ASSERT_OK_AND_ASSIGN(struct statfs sfs, syscalls::fstatfs(*file));
  ASSERT_EQ(sfs.f_type, TMPFS_MAGIC) << "/tmp is not tmpfs in this guest";

  EXPECT_THAT(ReadGeneration(*file, S_IFREG), IsOkAndHolds(0u));
  EXPECT_THAT(syscalls::unlinkat(AT_FDCWD, path, 0), IsOk());
}

// Registers a fake filesystem "mounted" at root/`name`, with its root inode
// linked there and one child inside it, as PopulateDirectory would have
// recorded a real mount.
struct FakeMount {
  InodeId root = 0;
  InodeId child = 0;
};

absl::StatusOr<FakeMount> AddFakeMount(Context &ctx, std::string_view name) {
  const DeviceId device = OtherDevice();
  ABSL_RETURN_IF_ERROR(cache::AddFilesystem(ctx, device, 0x1234, kRootInode,
                                            std::string(name)));
  struct statx stx {};
  stx.stx_mode = S_IFDIR | 0755;
  stx.stx_ino = 2;
  FileHandle handle{.device = device, .handle_type = 1, .bytes = {1, 2, 3}};
  FakeMount mount;
  ABSL_ASSIGN_OR_RETURN(cache::UpsertResult root,
                        cache::UpsertInode(ctx, handle, stx, 0));
  mount.root = root.id;
  ABSL_RETURN_IF_ERROR(cache::EnsureDirectory(ctx, mount.root));
  ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, kRootInode, name, mount.root));
  stx.stx_mode = S_IFREG | 0644;
  stx.stx_ino = 3;
  handle.bytes = {4, 5, 6};
  ABSL_ASSIGN_OR_RETURN(cache::UpsertResult child,
                        cache::UpsertInode(ctx, handle, stx, 0));
  mount.child = child.id;
  ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, mount.root, "child", child.id));
  return mount;
}

void ExpectFakeMountPurged(Context &ctx, MountFds &mounts,
                           const FakeMount &mount, std::string_view name) {
  EXPECT_THAT(cache::GetFilesystem(ctx, OtherDevice()),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(mounts.Get(OtherDevice()),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::GetAttr(ctx, mount.root),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::GetAttr(ctx, mount.child),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::Lookup(ctx, kRootInode, name),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(cache::IsDirComplete(ctx, kRootInode), IsOkAndHolds(false));
  // The source filesystem is never purged.
  ASSERT_OK_AND_ASSIGN(std::vector<cache::FilesystemRow> filesystems,
                       cache::ListFilesystems(ctx));
  ASSERT_EQ(filesystems.size(), 1u);
  EXPECT_FALSE(filesystems[0].parent_inode.has_value());
}

TEST_F(BackingTest, ReopenFdReopensAnOPathDescriptorForReal) {
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor path,
      syscalls::openat(AT_FDCWD, Path("file"), O_PATH));

  ASSERT_OK_AND_ASSIGN(FileDescriptor reopened, ReopenFd(*path, O_RDWR));
  ASSERT_OK_AND_ASSIGN(struct stat original, syscalls::fstat(*path));
  ASSERT_OK_AND_ASSIGN(struct stat now, syscalls::fstat(*reopened));
  EXPECT_EQ(original.st_ino, now.st_ino);
  ASSERT_OK_AND_ASSIGN(int fd_flags, syscalls::fcntl(*reopened, F_GETFD));
  EXPECT_NE(fd_flags & FD_CLOEXEC, 0);

  // What the reopening is for: xattr calls reject O_PATH descriptors.
  if (!xattrs_supported_) GTEST_SKIP() << "no user xattrs here";
  const std::string value = "test";
  EXPECT_THAT(syscalls::fsetxattr(
                  *reopened, "user.dcfs_reopen",
                  std::span<const uint8_t>(
                      reinterpret_cast<const uint8_t *>(value.data()),
                      value.size()),
                  0),
              IsOk());
}

// ReadXattrsFd goes through /proc/self/fd, so an O_PATH descriptor on a
// symlink reads the symlink's own xattrs, not its target's.
TEST_F(BackingTest, ReadXattrsFdReadsTheObjectNotTheSymlinkTarget) {
  if (!xattrs_supported_) GTEST_SKIP() << "no user xattrs here";
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor file,
      syscalls::openat(AT_FDCWD, Path("file"), O_PATH));
  ASSERT_OK_AND_ASSIGN(auto file_xattrs, ReadXattrsFd(*file));
  EXPECT_THAT(file_xattrs, ::testing::Contains(
                               std::make_pair(std::string("user.test"),
                                              std::string("value"))));

  ASSERT_OK_AND_ASSIGN(
      FileDescriptor link,
      syscalls::openat(AT_FDCWD, Path("link"), O_PATH | O_NOFOLLOW));
  ASSERT_OK_AND_ASSIGN(auto link_xattrs, ReadXattrsFd(*link));
  EXPECT_THAT(link_xattrs, ::testing::IsEmpty());
}

TEST_F(BackingTest, ReadSymlinkFdGrowsItsBufferForALongTarget) {
  const std::string long_target(500, 'a');  // past the first 256 bytes
  ASSERT_THAT(syscalls::symlinkat(long_target, AT_FDCWD, Path("long_link")),
              IsOk());
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor link,
      syscalls::openat(AT_FDCWD, Path("long_link"), O_PATH | O_NOFOLLOW));
  EXPECT_THAT(ReadSymlinkFd(*link), IsOkAndHolds(long_target));
}

TEST_F(BackingTest, StartupPurgeForgetsAFilesystemNoLongerMounted) {
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  // "dir" is a real directory, but nothing is mounted on it.
  ASSERT_OK_AND_ASSIGN(FakeMount mount, AddFakeMount(ctx_, "dir"));
  ASSERT_OK_AND_ASSIGN(FileDescriptor extra, syscalls::dup(source_fd_));
  ASSERT_THAT(mounts_.Insert(OtherDevice(), std::move(extra)), IsOk());
  ASSERT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));

  ASSERT_THAT(StartupPurge(ctx_), IsOk());
  ExpectFakeMountPurged(ctx_, mounts_, mount, "dir");

  // A second run has nothing left to do.
  EXPECT_THAT(StartupPurge(ctx_), IsOk());
}

TEST_F(BackingTest, StartupPurgeForgetsAFilesystemWhoseMountPointIsGone) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mnt"), 0755), IsOk());
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(FakeMount mount, AddFakeMount(ctx_, "mnt"));
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("mnt"), AT_REMOVEDIR),
              IsOk());

  ASSERT_THAT(StartupPurge(ctx_), IsOk());
  ExpectFakeMountPurged(ctx_, mounts_, mount, "mnt");
}

TEST_F(BackingTest, InitRootRejectsACacheForAnotherFilesystem) {
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Connection other_db,
      sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
  ASSERT_THAT(Migrate(other_db, RootIdentity{.device_id = OtherDevice(),
                                             .fstype = 0x1234,
                                             .backing_ino = 2,
                                             .backing_gen = 0}),
              IsOk());
  MountFds other_mounts;
  Context other{other_db, other_mounts, bitgen_};
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd, syscalls::dup(source_fd_));
  EXPECT_THAT(InitRoot(other, std::move(fd)),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_EQ(other_mounts.size(), 0u);
}

// InitRoot registers the source's mount fd once: a second call for the same
// filesystem (the database already has the root) is not an error, and the
// first fd stays registered.
TEST_F(BackingTest, InitRootTwiceKeepsTheFirstMountFd) {
  ASSERT_OK_AND_ASSIGN(FileDescriptor again, syscalls::dup(source_fd_));
  const int again_fd = *again;
  EXPECT_THAT(InitRoot(ctx_, std::move(again)), IsOk());
  EXPECT_EQ(mounts_.size(), 1u);
  EXPECT_THAT(mounts_.Fds(), ElementsAre(source_fd_));
  EXPECT_NE(again_fd, source_fd_);
}

// A directory fsync's failure reaches the caller. /proc's directories have
// no fsync, so fsync(2) on one fails with EINVAL (the reopen, as a
// directory, succeeds).
TEST_F(BackingTest, FsyncDirFdReportsTheFsyncsFailure) {
  EXPECT_THAT(FsyncDirFd(source_fd_, false), IsOk());
  EXPECT_THAT(FsyncDirFd(source_fd_, true), IsOk());
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor proc,
      syscalls::openat(AT_FDCWD, "/proc", O_PATH | O_DIRECTORY));
  absl::Status failed = FsyncDirFd(*proc, false);
  EXPECT_THAT(failed, Not(IsOk()));
  EXPECT_EQ(ErrnoOf(failed), EINVAL) << failed;
  EXPECT_EQ(ErrnoOf(FsyncDirFd(*proc, true)), EINVAL);
}

// --- Crash safety of writable opens (step 4.6) -------------------------------

TEST_F(BackingTest, RefreshAttrsFromFdMarksValid) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, file), IsOk());
  WriteFile(Path("file"), "longer contents");
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor owned,
      syscalls::openat(AT_FDCWD, Path("file"), O_RDONLY));
  const int fd = *owned;

  ASSERT_THAT(RefreshAttrsFromFd(ctx_, file, fd), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 15);
}

TEST_F(BackingTest, AttrsOfAFileOpenForWriteStayUnknown) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  absl::flat_hash_set<int64_t> open_for_write = {file};
  ctx_.open_for_write = &open_for_write;
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor owned,
      syscalls::openat(AT_FDCWD, Path("file"), O_RDONLY));
  const int fd = *owned;
  WriteFile(Path("file"), "longer contents");

  // Every way of recording fresh attributes stores them (so they can be
  // served) but leaves them unknown while the file is open for writing.
  ASSERT_THAT(RefreshAttrsFromFd(ctx_, file, fd), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_FALSE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 15);
  ASSERT_THAT(RefreshAttrs(ctx_, file), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, file),
              IsOkAndHolds(testing::Field(&cache::CachedAttr::valid, false)));
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, file),
              IsOkAndHolds(testing::Field(&cache::CachedAttr::valid, false)));

  // Once the last writable open is gone, they are recorded as current.
  open_for_write.clear();
  ASSERT_THAT(RefreshAttrsFromFd(ctx_, file, fd), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, file),
              IsOkAndHolds(testing::Field(&cache::CachedAttr::valid, true)));
  ctx_.open_for_write = nullptr;
}

// Audit F8: an unlinked file dcfs still holds open has nlink 0, and its row
// only lives until the last close deletes it; a crash in between must not
// leave nlink 0 cached as current.
TEST_F(BackingTest, AttrsWithNoLinksLeftStayUnknown) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor owned,
      syscalls::openat(AT_FDCWD, Path("file"), O_RDONLY));
  const int fd = *owned;
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("file"), 0), IsOk());

  ASSERT_THAT(RefreshAttrsFromFd(ctx_, file, fd), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_FALSE(attr.valid);
  EXPECT_EQ(attr.st.st_nlink, 0u);
}

// --- Out-of-band change detection (step 4.6) ---------------------------------

// Coarse kernel timestamps can make a change made right after the previous
// one leave mtime/ctime unchanged; waiting a few ticks makes the out-of-band
// changes below observable through the timestamps too.
void WaitForNextTimestamp() {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 30'000'000};
  EXPECT_THAT(syscalls::nanosleep(ts), IsOk());
}

// Expects exactly `times` "out-of-band" warnings while it is alive, and
// allows any other log line.
class OutOfBandLog {
 public:
  explicit OutOfBandLog(int times)
      : log_(absl::MockLogDefault::kIgnoreUnexpected) {
    EXPECT_CALL(log_, Log(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(log_, Log(absl::LogSeverity::kWarning, _,
                          HasSubstr("out-of-band")))
        .Times(times);
    log_.StartCapturingLogs();
  }

 private:
  absl::ScopedMockLog log_;
};

TEST_F(BackingTest, OpenNodeDetectsOutOfBandChmod) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  WaitForNextTimestamp();
  ASSERT_THAT(syscalls::fchmodat(AT_FDCWD, Path("file"), 0600, 0), IsOk());
  {
    OutOfBandLog log(1);
    ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
    // Adopted, so a second open has nothing left to report.
    ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
  }
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_mode, S_IFREG | 0600);
}

TEST_F(BackingTest, OpenNodeDetectsOutOfBandAppend) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  WaitForNextTimestamp();
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor fd_owner,
      syscalls::openat(AT_FDCWD, Path("file"), O_WRONLY | O_APPEND));
  const int fd = *fd_owner;
  ASSERT_THAT(syscalls::write(fd, " world", 6), IsOkAndHolds(6u));
  ASSERT_THAT(syscalls::close(std::move(fd_owner)), IsOk());
  {
    OutOfBandLog log(1);
    ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
  }
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_EQ(attr.st.st_size, 11);
  struct statx stx = StatPath(Path("file"));
  EXPECT_EQ(attr.st.st_mtim.tv_sec, stx.stx_mtime.tv_sec);
  EXPECT_EQ(attr.st.st_mtim.tv_nsec, stx.stx_mtime.tv_nsec);
}

TEST_F(BackingTest, OpenNodeDetectsOutOfBandTouch) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  // touch -d 2001-09-09T01:46:40Z
  struct timespec times[2] = {{.tv_sec = 1'000'000'000, .tv_nsec = 0},
                              {.tv_sec = 1'000'000'000, .tv_nsec = 0}};
  ASSERT_THAT(syscalls::utimensat(AT_FDCWD, Path("file"), times, 0), IsOk());
  {
    OutOfBandLog log(1);
    ASSERT_THAT(OpenNode(ctx_, file, O_PATH), IsOk());
  }
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_EQ(attr.st.st_mtim.tv_sec, 1'000'000'000);
}

TEST_F(BackingTest, OpenNodeDetectsANewFileInADirectory) {
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  // Populates `dir` and caches "newfile" as known absent.
  ASSERT_THAT(LookupOrPopulate(ctx_, dir, "newfile"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  ASSERT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(true));
  WaitForNextTimestamp();
  WriteFile(Path("dir/newfile"), "new");
  {
    OutOfBandLog log(1);
    ASSERT_THAT(OpenNode(ctx_, dir, O_RDONLY | O_DIRECTORY), IsOk());
    EXPECT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(false));
    // Relisting finds it, without reporting `dir` (already adopted) or
    // its unchanged children again.
    ASSERT_OK_AND_ASSIGN(LookupResult found,
                         LookupOrPopulate(ctx_, dir, "newfile"));
    EXPECT_EQ(found.kind, LookupResult::Kind::kFound);
  }
  EXPECT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(true));
  EXPECT_THAT(Id("inner", dir), IsOk());
}

TEST_F(BackingTest, OpenNodeDetectsAnOutOfBandXattr) {
  if (!xattrs_supported_) GTEST_SKIP() << "no user xattrs here";
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_THAT(cache::ListXattrs(ctx_, file),
              IsOkAndHolds(Optional(ElementsAre("user.test"))));
  WaitForNextTimestamp();
  ASSERT_THAT(SetUserXattr(Path("file"), "user.added", "x"), IsOk());
  {
    OutOfBandLog log(1);
    ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
  }
  EXPECT_THAT(cache::ListXattrs(ctx_, file), IsOkAndHolds(std::nullopt));
  ASSERT_THAT(RefreshXattrs(ctx_, file), IsOk());
  ASSERT_OK_AND_ASSIGN(std::optional<std::vector<std::string>> names,
                       cache::ListXattrs(ctx_, file));
  ASSERT_TRUE(names.has_value());
  EXPECT_THAT(*names, Contains("user.added"));
  EXPECT_THAT(cache::GetXattr(ctx_, file, "user.added"),
              IsOkAndHolds(Optional(std::string("x"))));
}

TEST_F(BackingTest, RepopulationDetectsAnOutOfBandChangeOnce) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  WaitForNextTimestamp();
  ASSERT_THAT(syscalls::fchmodat(AT_FDCWD, Path("file"), 0600, 0), IsOk());
  ASSERT_THAT(cache::MarkDirComplete(ctx_, kRootInode, false), IsOk());
  {
    OutOfBandLog log(1);
    ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
    ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
  }
  EXPECT_THAT(cache::GetAttr(ctx_, file),
              IsOkAndHolds(testing::Field(&cache::CachedAttr::st,
                                          testing::Field(&stat::st_mode,
                                                         S_IFREG | 0600))));
}

TEST_F(BackingTest, UnchangedNodesReportNothing) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  ASSERT_OK_AND_ASSIGN(InodeId link, Id("link"));
  ASSERT_THAT(Id("inner", dir), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr before, cache::GetAttr(ctx_, file));
  WaitForNextTimestamp();
  {
    OutOfBandLog log(0);
    ASSERT_THAT(OpenNode(ctx_, file, O_RDONLY), IsOk());
    ASSERT_THAT(OpenNode(ctx_, dir, O_RDONLY | O_DIRECTORY), IsOk());
    ASSERT_THAT(OpenNode(ctx_, link, O_PATH | O_NOFOLLOW), IsOk());
    ASSERT_THAT(cache::MarkDirComplete(ctx_, kRootInode, false), IsOk());
    ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  }
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr after, cache::GetAttr(ctx_, file));
  EXPECT_TRUE(after.valid);
  EXPECT_EQ(after.st.st_ctim.tv_nsec, before.st.st_ctim.tv_nsec);
  EXPECT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(true));
  EXPECT_THAT(cache::ListXattrs(ctx_, file), IsOkAndHolds(Optional(_)));
}

// --- Durability: sync points and recovery (step 4.10) -----------------------

TEST_F(BackingTest, SyncBackingClearsTheDirtySetExceptOpenWriters) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(InodeId hl1, Id("hl1"));
  ASSERT_THAT(cache::BeginAttrChange(ctx_, file), IsOk());
  ASSERT_THAT(cache::BeginCreate(ctx_, kRootInode, "new"), IsOk());
  ASSERT_THAT(cache::BeginAttrChange(ctx_, hl1), IsOk());
  ASSERT_THAT(cache::ListDirty(ctx_),
              IsOkAndHolds(testing::UnorderedElementsAre(file, kRootInode, hl1)));

  // hl1 still has a writable open: the kernel may keep writing to it after
  // the sync, so it stays dirty.
  absl::flat_hash_set<int64_t> open_for_write = {hl1};
  ctx_.open_for_write = &open_for_write;
  ASSERT_THAT(SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::ElementsAre(hl1)));
  EXPECT_TRUE(ctx_.dirty.any);
  EXPECT_TRUE(ctx_.dirty.durable.empty());

  open_for_write.clear();
  ASSERT_THAT(SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
  EXPECT_FALSE(ctx_.dirty.any);
  ctx_.open_for_write = nullptr;
}

// formal/ finding sync_during_mutation: a sync point that runs between a
// mutation's phase 1 and its backing syscall must not drop the mutation's
// dirty rows. The syncfs ran before the syscall, so it did not make the
// syscall durable; if the dirty row went, a power loss that kept phase 3
// and lost the syscall would leave the cache ahead with nothing for
// recovery to forget. (Unreachable while dcfs is single-threaded; real
// under coroutines, where a sync point can run while a request waits on
// its syscall.)
TEST_F(BackingTest, SyncPointKeepsAMutationInFlightDirty) {
  ASSERT_THAT(SyncBacking(ctx_), IsOk());
  ASSERT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));

  // Phase 1 of a mkdir of "new" in the root.
  ASSERT_OK_AND_ASSIGN(cache::Mutation mutation,
                       cache::BeginCreate(ctx_, kRootInode, "new"));
  // A sync point now: the mkdir has not been issued yet.
  ASSERT_THAT(SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_),
              IsOkAndHolds(testing::ElementsAre(kRootInode)));
  EXPECT_TRUE(ctx_.dirty.any);

  // Phase 2 and phase 3.
  absl::StatusOr<FileDescriptor> parent = OpenNode(
      ctx_, kRootInode, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_THAT(parent, IsOk());
  const Credentials root{.uid = 0, .gid = 0, .groups = {}};
  ASSERT_THAT(MkdirAt(ctx_, root, **parent, "new", 0755), IsOk());
  ASSERT_OK_AND_ASSIGN(NewChild child, RecordNewChild(ctx_, mutation,
                                                      kRootInode, **parent,
                                                      "new"));
  mutation.End();
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "new"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  // Still dirty, the new child too: no syncfs since the mkdir.
  EXPECT_THAT(cache::ListDirty(ctx_),
              IsOkAndHolds(testing::UnorderedElementsAre(kRootInode,
                                                         child.id)));

  // The next sync point covers the mkdir: now both may go.
  ASSERT_THAT(SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
  EXPECT_FALSE(ctx_.dirty.any);
}

TEST_F(BackingTest, StartRunRecoversTheDirtySetAfterAnUncleanShutdown) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  ASSERT_OK_AND_ASSIGN(InodeId inner, Id("inner", dir));
  ASSERT_OK_AND_ASSIGN(InodeId hl1, Id("hl1"));
  auto valid = [&](InodeId id) {
    absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
    return attr.ok() && attr->valid;
  };

  // A fresh cache: nothing to recover, and the run is now marked running.
  ASSERT_THAT(StartRun(ctx_, "boot-1"), IsOk());
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(false));
  EXPECT_THAT(GetBootId(db_), IsOkAndHolds(Optional(std::string("boot-1"))));
  EXPECT_FALSE(ctx_.dirty.any);
  EXPECT_TRUE(valid(file));

  // A mutation's phase 1 and phase 3 both committed, then the daemon (or
  // the machine) died before any sync point.
  ASSERT_THAT(
      cache::BeginRemove(ctx_, dir, "inner", inner, cache::BeginFill(ctx_)),
      IsOk());
  ASSERT_THAT(RefreshAttrs(ctx_, dir), IsOk());
  ASSERT_THAT(RefreshAttrs(ctx_, inner), IsOk());
  ASSERT_THAT(cache::SetNegative(ctx_, dir, "inner"), IsOk());
  ASSERT_THAT(cache::MarkDirComplete(ctx_, dir, true), IsOk());
  ASSERT_TRUE(valid(dir));
  ASSERT_TRUE(valid(inner));

  {
    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         testing::AllOf(HasSubstr("recovered 2 dirty"),
                               HasSubstr("machine rebooted"))));
    log.StartCapturingLogs();
    ASSERT_THAT(StartRun(ctx_, "boot-2"), IsOk());
  }
  // Exactly the dirty entries are unknown; everything else stays warm.
  EXPECT_FALSE(valid(dir));
  EXPECT_FALSE(valid(inner));
  EXPECT_THAT(cache::IsDirComplete(ctx_, dir), IsOkAndHolds(false));
  EXPECT_THAT(cache::Lookup(ctx_, dir, "inner"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_TRUE(valid(file));
  EXPECT_TRUE(valid(hl1));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "file"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  // The dirty entries stay dirty until Startup has probed their rows (step
  // 12.6b): StartRun returns them for that.
  EXPECT_THAT(cache::ListDirty(ctx_),
              IsOkAndHolds(testing::UnorderedElementsAre(dir, inner)));
  EXPECT_THAT(GetBootId(db_), IsOkAndHolds(Optional(std::string("boot-2"))));

  // And the truth is re-read from the backing filesystem: inner still
  // exists there (the phase 2 unlink never ran in this test).
  EXPECT_THAT(LookupOrPopulate(ctx_, dir, "inner"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
}

// docs/design.md "Logging": a start says how the last run ended and what
// recovery did, a sync point says what it cleared, and a clean shutdown
// says so, all at INFO and once.
TEST_F(BackingTest, TheLifecycleIsLoggedAtInfo) {
  absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
  EXPECT_CALL(log, Log(_, _, _)).Times(AnyNumber());
  EXPECT_CALL(log, Log(absl::LogSeverity::kInfo, _,
                       HasSubstr("recovery: the last run ended")))
      .Times(1);
  EXPECT_CALL(log, Log(absl::LogSeverity::kInfo, _,
                       HasSubstr("sync point: cleared ")))
      .Times(1);
  EXPECT_CALL(log, Log(absl::LogSeverity::kInfo, _,
                       HasSubstr("shutdown: clean")))
      .Times(1);
  log.StartCapturingLogs();
  ASSERT_THAT(StartRun(ctx_, "boot-1"), IsOk());
  ASSERT_THAT(FinishRun(ctx_), IsOk());
}

TEST_F(BackingTest, FinishRunMarksACleanShutdown) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_THAT(StartRun(ctx_, "boot-1"), IsOk());
  ASSERT_THAT(cache::BeginAttrChange(ctx_, file), IsOk());

  // A writable open still outstanding: its entry stays dirty, so the
  // shutdown is not clean.
  absl::flat_hash_set<int64_t> open_for_write = {file};
  ctx_.open_for_write = &open_for_write;
  EXPECT_THAT(FinishRun(ctx_), StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(false));

  open_for_write.clear();
  ASSERT_THAT(FinishRun(ctx_), IsOk());
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(true));
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
  ctx_.open_for_write = nullptr;

  // The next start finds nothing to recover and logs nothing about it.
  {
    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         HasSubstr("did not shut down cleanly")))
        .Times(0);
    log.StartCapturingLogs();
    ASSERT_THAT(StartRun(ctx_, "boot-1"), IsOk());
  }
}

// The cache database as a file in WAL mode, for what an in-memory one cannot
// show: another connection and the checkpoint.
class FileDbBackingTest : public BackingTest {
 protected:
  std::string DbPath() const override {
    return absl::StrCat(std::getenv("TEST_TMPDIR"), "/finish_run.sqlite");
  }
};

// dcfs sets no busy timeout (step 15.6b, "no timers"): the TRUNCATE checkpoint
// of FinishRun, with a reader (an administrator's sqlite3 session, `testutil
// sql`) holding a snapshot, fails at once with Unavailable instead of waiting
// for it. The run then does not end clean (the flag stays unset, the shutdown
// says why), and the next start recovers with nothing dirty to re-read.
TEST_F(FileDbBackingTest, FinishRunWithAReaderOpenFailsAtOnceAndEndsUnclean) {
  ASSERT_THAT(StartRun(ctx_, "boot-1"), IsOk());
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Connection reader,
      sqlite3::ConnectionFactory{.path = DbPath()}.Open());
  ASSERT_THAT(reader.Exec("BEGIN"), IsOk());
  ASSERT_THAT(reader.Exec("SELECT count(*) FROM sqlite_master"), IsOk());

  EXPECT_THAT(FinishRun(ctx_), StatusIs(absl::StatusCode::kUnavailable));
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(false));

  ASSERT_THAT(reader.Exec("ROLLBACK"), IsOk());
  {
    absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
    EXPECT_CALL(log, Log(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(log, Log(absl::LogSeverity::kWarning, _,
                         HasSubstr("did not shut down cleanly (the daemon "
                                   "died; same boot); recovered 0 dirty")))
        .Times(1);
    log.StartCapturingLogs();
    ASSERT_THAT(StartRun(ctx_, "boot-1"), IsOk());
  }
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
}

// --- Runtime boundary refusal (amendment 12) --------------------------------
//
// The source tree here (BackingTest::SetUp) lives on the ext4 disk backing
// this test (see the qemu_cc_test's `disks`), so mounting a real tmpfs
// below it produces a genuine mount-id/st_dev boundary for IsBoundary to
// detect, exactly as a real submount would in production.

class BoundaryTest : public BackingTest {
 protected:
  void SetUp() override {
    BackingTest::SetUp();
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("boundary"), 0755), IsOk());
    ASSERT_THAT(
        syscalls::mount("tmpfs", Path("boundary"), "tmpfs", 0, nullptr),
        IsOk());
    mounted_ = true;
  }

  void TearDown() override {
    if (mounted_) {
      syscalls::umount2(Path("boundary"), MNT_DETACH).IgnoreError();
    }
    BackingTest::TearDown();
  }

  bool mounted_ = false;
};

// Expects exactly `times` "refusing to cache" ERROR log lines while it is
// alive, and allows any other log line (see OutOfBandLog above, same
// pattern).
class RefusalLog {
 public:
  explicit RefusalLog(int times) : log_(absl::MockLogDefault::kIgnoreUnexpected) {
    EXPECT_CALL(log_, Log(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(log_, Log(absl::LogSeverity::kError, _,
                          HasSubstr("refusing to cache")))
        .Times(times);
    log_.StartCapturingLogs();
  }

 private:
  absl::ScopedMockLog log_;
};

// Step 23.5: the listing shows the boundary, as its stub.
TEST_F(BoundaryTest, BoundaryIsListedAsItsStub) {
  absl::StatusOr<Populated> populated;
  {
    RefusalLog log(1);
    populated = PopulateDirectory(ctx_, kRootInode);
  }
  ASSERT_THAT(populated, IsOk());
  EXPECT_TRUE(populated->cached);
  ASSERT_TRUE(populated->entries.contains("boundary"));
  const LookupResult listed = populated->entries.at("boundary");
  EXPECT_EQ(listed.kind, LookupResult::Kind::kRefused);
  EXPECT_TRUE(cache::IsStub(listed.id));
  EXPECT_THAT(ListNames(ctx_, kRootInode), Contains("boundary"));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));
  // The stub has the boundary root's attributes (a tmpfs root: 1777).
  ASSERT_OK_AND_ASSIGN(cache::StubRow stub, cache::GetStub(ctx_, listed.id));
  EXPECT_EQ(stub.name, "boundary");
  EXPECT_EQ(stub.parent, kRootInode);
  ASSERT_OK_AND_ASSIGN(struct stat root,
                       syscalls::fstatat(AT_FDCWD, Path("boundary")));
  EXPECT_EQ(stub.attr.st.st_mode, root.st_mode);
  EXPECT_EQ(stub.attr.st.st_ino, static_cast<uint64_t>(listed.id));
}

// Step 23.5: a boundary is its stub (a nodeid at or above 2^63).
TEST_F(BoundaryTest, LookupOfABoundaryReturnsItsStubWithoutCachingNegative) {
  absl::StatusOr<LookupResult> first;
  {
    RefusalLog log(1);
    first = LookupOrPopulate(ctx_, kRootInode, "boundary");
  }
  ASSERT_THAT(first, IsOkAndHolds(IsLookup(LookupResult::Kind::kRefused)));
  EXPECT_TRUE(cache::IsStub(first->id));
  // A second lookup answers straight from the persisted refusal: no
  // repopulation (the directory is already complete), no second log line,
  // and the same stub rather than ENOENT.
  absl::StatusOr<LookupResult> second;
  {
    RefusalLog log(0);
    second = LookupOrPopulate(ctx_, kRootInode, "boundary");
  }
  ASSERT_THAT(second, IsOkAndHolds(IsLookup(LookupResult::Kind::kRefused)));
  EXPECT_EQ(second->id, first->id);
  // Never cached negative: cache::Lookup on its own (no populate) reports
  // kRefused -- the object exists, so this must never come back kNegative
  // (which would mean dcfs claims it is absent) or kUnknown (which would
  // let some other caller fall through to caching it negative).
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "boundary"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kRefused)));
}

TEST_F(BoundaryTest, BoundaryRefusalPersistsAcrossRestart) {
  {
    RefusalLog log(1);
    ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  }
  // Simulate a daemon restart: a fresh Context, carrying no in-memory state
  // of its own, over the same database and mounts. The refusal must be
  // readable from the cache alone -- never from process memory, since a
  // dentry cached negative in a complete directory would otherwise report
  // ENOENT for something that still exists on the backing filesystem.
  ASSERT_OK_AND_ASSIGN(LookupResult before, cache::Lookup(ctx_, kRootInode,
                                                         "boundary"));
  Context restarted{db_, mounts_, bitgen_};
  absl::StatusOr<LookupResult> after =
      LookupOrPopulate(restarted, kRootInode, "boundary");
  ASSERT_THAT(after, IsOkAndHolds(IsLookup(LookupResult::Kind::kRefused)));
  EXPECT_EQ(after->id, before.id);
}

TEST_F(BoundaryTest, BoundaryDoesNotRegisterAFilesystem) {
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(std::vector<cache::FilesystemRow> filesystems,
                       cache::ListFilesystems(ctx_));
  ASSERT_EQ(filesystems.size(), 1u);  // The source only.
  EXPECT_FALSE(filesystems[0].parent_inode.has_value());
}

// Audit F7: after a failed create-family op, re-resolving its one name
// (DirCacheFS::ReresolveAfterFailure: LookupOrPopulate) must probe just
// that name, not relist the whole directory. A sibling whose attributes
// are unknown shows whether it was re-probed.
TEST_F(BackingTest, ReresolvingOneUnknownNameProbesOnlyThatName) {
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, file), IsOk());
  // Phase 1 of `mkdir dir` (which will fail: EEXIST).
  ASSERT_THAT(cache::BeginCreate(ctx_, kRootInode, "dir"), IsOk());
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "dir"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  EXPECT_THAT(Id("dir"), IsOkAndHolds(dir));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr file_attr, cache::GetAttr(ctx_, file));
  EXPECT_FALSE(file_attr.valid) << "the sibling was re-probed";
  // And a name that turns out not to exist is resolved absent.
  ASSERT_THAT(cache::BeginCreate(ctx_, kRootInode, "nothere"), IsOk());
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "nothere"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  ASSERT_OK_AND_ASSIGN(file_attr, cache::GetAttr(ctx_, file));
  EXPECT_FALSE(file_attr.valid) << "the sibling was re-probed";
}

// Audit F6: a directory whose own dentry is unknown (a rename's phase 1,
// an invalidation of its parent, crash recovery) still has a parent: it is
// resolved from the backing filesystem ("..") instead of reported missing
// (the FUSE layer used to answer ENOENT for "..", breaking readdir and NFS
// reconnection of that directory).
TEST_F(BackingTest, ParentOfAnUnknownDentryIsResolvedFromTheBacking) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("dir/sub"), 0755), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("dir"));
  ASSERT_OK_AND_ASSIGN(InodeId sub, Id("sub", dir));
  EXPECT_THAT(ParentOf(ctx_, sub), IsOkAndHolds(dir));

  // The dentries unknown: resolved to the same rows.
  const std::string dir_name[] = {"dir"};
  const std::string sub_name[] = {"sub"};
  ASSERT_THAT(cache::MarkUnknown(ctx_, kRootInode, dir_name), IsOk());
  ASSERT_THAT(cache::MarkUnknown(ctx_, dir, sub_name), IsOk());
  EXPECT_THAT(ParentOf(ctx_, dir), IsOkAndHolds(kRootInode));
  EXPECT_THAT(ParentOf(ctx_, sub), IsOkAndHolds(dir));

  // The parent's row itself gone (its children's dentries with it): a new
  // row for the same backing directory.
  ASSERT_THAT(cache::InvalidateInode(ctx_, dir), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId parent, ParentOf(ctx_, sub));
  EXPECT_NE(parent, dir);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, parent));
  EXPECT_TRUE(S_ISDIR(attr.st.st_mode));
  EXPECT_EQ(attr.backing_ino, StatPath(Path("dir")).stx_ino);
  EXPECT_THAT(ParentOf(ctx_, sub), IsOkAndHolds(parent));
}


// --- Taking on the caller's identity (step 4.7) ------------------------------

// A parent directory for the credential tests: `mode`, owned by root and
// group `gid`, opened as the create-family functions expect.
FileDescriptor MakeParent(const std::string &path, mode_t mode, gid_t gid) {
  EXPECT_THAT(syscalls::mkdirat(AT_FDCWD, path, 0), IsOk());
  EXPECT_THAT(syscalls::fchownat(AT_FDCWD, path, 0, gid, 0), IsOk());
  EXPECT_THAT(syscalls::fchmodat(AT_FDCWD, path, mode, 0), IsOk());
  absl::StatusOr<FileDescriptor> fd =
      syscalls::openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY);
  EXPECT_THAT(fd, IsOk());
  return fd.ok() ? *std::move(fd) : FileDescriptor();
}

// The thread is root again after every switch: fsuid/fsgid 0 and the
// supplementary groups it started with.
void ExpectRootAgain(const std::vector<gid_t> &groups) {
  EXPECT_EQ(syscalls::setfsuid(static_cast<uid_t>(-1)), 0u);
  EXPECT_EQ(syscalls::setfsgid(static_cast<gid_t>(-1)), 0u);
  EXPECT_THAT(GetGroups(), IsOkAndHolds(groups));
}

TEST_F(BackingTest, CreateFamilyRunsAsTheCaller) {
  ASSERT_OK_AND_ASSIGN(std::vector<gid_t> groups, GetGroups());
  FileDescriptor parent = MakeParent(Path("pub"), 0777, 0);
  const Credentials alice{.uid = 1000, .gid = 1000, .groups = {1000}};

  ASSERT_THAT(MkdirAt(ctx_, alice, *parent, "d", 0755), IsOk());
  ASSERT_THAT(MknodAt(ctx_, alice, *parent, "p", S_IFIFO | 0644, 0), IsOk());
  ASSERT_THAT(SymlinkAt(ctx_, alice, *parent, "l", "d"), IsOk());
  ASSERT_THAT(CreateAt(ctx_, alice, *parent, "f", O_WRONLY, 0644), IsOk());
  for (const char *name : {"d", "p", "l", "f"}) {
    struct statx stx = StatPath(Path(absl::StrCat("pub/", name)));
    EXPECT_EQ(stx.stx_uid, 1000u) << name;
    EXPECT_EQ(stx.stx_gid, 1000u) << name;
  }
  ExpectRootAgain(groups);
}

TEST_F(BackingTest, CallerGetsNoFilesystemCapabilities) {
  ASSERT_OK_AND_ASSIGN(std::vector<gid_t> groups, GetGroups());
  // Root's 0755 directory: root may create in it, the caller may not (no
  // CAP_DAC_OVERRIDE while fsuid is not 0).
  FileDescriptor parent = MakeParent(Path("rootonly"), 0755, 0);
  const Credentials alice{.uid = 1000, .gid = 1000, .groups = {}};
  EXPECT_EQ(ErrnoOf(MkdirAt(ctx_, alice, *parent, "d", 0755)), EACCES);
  EXPECT_EQ(ErrnoOf(CreateAt(ctx_, alice, *parent, "f", O_WRONLY, 0644)
                        .status()),
            EACCES);
  ExpectRootAgain(groups);
  // ... and root is root again: the same mkdir now succeeds.
  const Credentials root{.uid = 0, .gid = 0, .groups = {}};
  EXPECT_THAT(MkdirAt(ctx_, root, *parent, "d", 0755), IsOk());
}

TEST_F(BackingTest, CallerSupplementaryGroupsApply) {
  ASSERT_OK_AND_ASSIGN(std::vector<gid_t> groups, GetGroups());
  FileDescriptor parent = MakeParent(Path("grp"), 0770, 2000);
  const Credentials member{.uid = 1000, .gid = 1000, .groups = {1000, 2000}};
  const Credentials outsider{.uid = 1000, .gid = 1000, .groups = {1000}};
  EXPECT_THAT(MkdirAt(ctx_, member, *parent, "in", 0755), IsOk());
  EXPECT_EQ(ErrnoOf(MkdirAt(ctx_, outsider, *parent, "out", 0755)), EACCES);
  ExpectRootAgain(groups);
}

TEST_F(BackingTest, SetgidParentGroupIsInherited) {
  FileDescriptor parent = MakeParent(Path("sgid"), 02777, 2000);
  const Credentials alice{.uid = 1000, .gid = 1000, .groups = {1000}};
  ASSERT_THAT(MkdirAt(ctx_, alice, *parent, "d", 0755), IsOk());
  struct statx stx = StatPath(Path("sgid/d"));
  EXPECT_EQ(stx.stx_uid, 1000u);
  EXPECT_EQ(stx.stx_gid, 2000u);
  EXPECT_EQ(stx.stx_mode & 07777, 02755);
}

// Records the names of the backing calls that are announced
// (ProtocolEvents::BackingCall), for the invariant checks and the fault
// sweep, which depend on every backing syscall being announced.
class BackingCallNames final : public ProtocolEvents {
 public:
  void BackingCall(Context &, std::string_view what,
                   absl::SourceLocation) override {
    names.emplace_back(what);
  }
  std::vector<std::string> names;
};

TEST_F(BackingTest, UnlinkAndRenameHonorTheStickyBit) {
  ASSERT_OK_AND_ASSIGN(std::vector<gid_t> groups, GetGroups());
  FileDescriptor sticky = MakeParent(Path("sticky"), 01777, 0);
  const Credentials alice{.uid = 1000, .gid = 1000, .groups = {}};
  const Credentials bob{.uid = 1001, .gid = 1001, .groups = {}};
  ASSERT_THAT(CreateAt(ctx_, bob, *sticky, "bf", O_WRONLY, 0644), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("sticky"));

  BackingCallNames calls;
  ctx_.events = &calls;
  EXPECT_EQ(ErrnoOf(UnlinkAt(ctx_, alice, dir, "bf", 0)), EPERM);
  EXPECT_THAT(calls.names, Contains("unlinkat"))
      << "a refused unlink still announces its syscall";
  EXPECT_EQ(ErrnoOf(RenameAt(ctx_, alice, dir, "bf", dir, "stolen", 0)),
            EPERM);
  EXPECT_THAT(calls.names, Contains("renameat2"));
  EXPECT_THAT(RenameAt(ctx_, bob, dir, "bf", dir, "bf2", 0), IsOk());
  calls.names.clear();
  EXPECT_THAT(UnlinkAt(ctx_, bob, dir, "bf2", 0), IsOk());
  EXPECT_THAT(calls.names, Contains("unlinkat"));
  ctx_.events = &NoProtocolEvents();
  ExpectRootAgain(groups);
}

// The raw setgroups system call changes only the calling thread's groups
// (glibc's setgroups() would change every thread's), and setfsuid/setfsgid
// report the previous value, with -1 reading the current one back.
TEST(BackingCredentialsTest, SetgroupsIsPerThread) {
  absl::StatusOr<std::vector<gid_t>> original = GetGroups();
  ASSERT_THAT(original, IsOk());
  std::vector<gid_t> in_thread;
  std::vector<gid_t> in_main_meanwhile;
  std::thread([&] {
    const gid_t groups[] = {4242, 4243};
    ASSERT_THAT(syscalls::setgroups(groups), IsOk());
    absl::StatusOr<std::vector<gid_t>> mine = GetGroups();
    ASSERT_THAT(mine, IsOk());
    in_thread = *mine;
    // Read the main thread's groups from here, while this thread still
    // has its own: /proc/self/task/<main tid>/status is not this thread's.
    std::string status_path =
        "/proc/self/task/" + std::to_string(syscalls::getpid()) + "/status";
    ASSERT_OK_AND_ASSIGN(std::string status_text,
                         testonly::ReadFileToString(status_path));
    for (std::string_view line : absl::StrSplit(status_text, '\n')) {
      if (!absl::ConsumePrefix(&line, "Groups:")) continue;
      for (std::string_view word :
           absl::StrSplit(line, ' ', absl::SkipWhitespace())) {
        uint32_t g = 0;
        ASSERT_TRUE(absl::SimpleAtoi(word, &g)) << word;
        in_main_meanwhile.push_back(static_cast<gid_t>(g));
      }
    }
  }).join();
  EXPECT_EQ(in_thread, (std::vector<gid_t>{4242, 4243}));
  std::vector<gid_t> sorted_original = *original;
  std::sort(sorted_original.begin(), sorted_original.end());
  std::sort(in_main_meanwhile.begin(), in_main_meanwhile.end());
  EXPECT_EQ(in_main_meanwhile, sorted_original);
  EXPECT_THAT(GetGroups(), absl_testing::IsOkAndHolds(*original));
}

}  // namespace
}  // namespace dcfs::backing
