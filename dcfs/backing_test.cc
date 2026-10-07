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
#include <filesystem>
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
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/credentials.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs::backing {
namespace {

namespace fs = std::filesystem;

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
  struct statx stx {};
  EXPECT_EQ(::statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW,
                    STATX_BASIC_STATS | STATX_BTIME, &stx),
            0)
      << path << ": " << std::strerror(errno);
  return stx;
}

void WriteFile(const std::string &path, std::string_view contents) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0) << path << ": " << std::strerror(errno);
  ASSERT_EQ(::write(fd, contents.data(), contents.size()),
            static_cast<ssize_t>(contents.size()));
  ::close(fd);
}

class BackingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    std::string templ = absl::StrCat(tmpdir, "/backing_XXXXXX");
    ASSERT_NE(::mkdtemp(templ.data()), nullptr) << std::strerror(errno);
    source_ = templ;

    // The source tree every test starts from.
    WriteFile(Path("file"), "hello");
    ASSERT_EQ(::mkdir(Path("dir").c_str(), 0755), 0);
    WriteFile(Path("dir/inner"), "inner");
    ASSERT_EQ(::symlink("file", Path("link").c_str()), 0);
    ASSERT_EQ(::mkfifo(Path("fifo").c_str(), 0644), 0);
    WriteFile(Path("hl1"), "linked");
    ASSERT_EQ(::link(Path("hl1").c_str(), Path("hl2").c_str()), 0);
    xattrs_supported_ = ::setxattr(Path("file").c_str(), "user.test", "value",
                                   5, 0) == 0;

    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
    // A real (non-O_PATH) fd: InitRoot registers it as the source
    // filesystem's mount fd, and open_by_handle_at's mount fd argument
    // rejects O_PATH (fs/fhandle.c get_path_from_fd()).
    int source_fd =
        ::open(source_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(source_fd, 0);
    FileDescriptor owned(source_fd);
    ASSERT_OK_AND_ASSIGN(RootIdentity root, ProbeRoot(ctx_, source_fd));
    ASSERT_THAT(Migrate(db_, root), IsOk());
    ASSERT_THAT(InitRoot(ctx_, std::move(owned)), IsOk());
    source_fd_ = source_fd;  // Now owned by mounts_.
  }

  void TearDown() override {
    Unlock();
    if (!source_.empty()) {
      std::error_code ec;
      fs::remove_all(source_, ec);
    }
  }

  std::string Path(std::string_view rel) const {
    return absl::StrCat(source_, "/", rel);
  }

  // Removes all permissions from everything in the source tree (children
  // before their directories, the source itself last), so that any backing
  // access from here on fails with EACCES for this unprivileged test.
  void Lock() {
    std::vector<fs::path> paths;
    for (const fs::directory_entry &entry :
         fs::recursive_directory_iterator(source_)) {
      paths.push_back(entry.path());
    }
    std::reverse(paths.begin(), paths.end());
    paths.push_back(source_);
    for (const fs::path &path : paths) {
      struct stat st {};
      ASSERT_EQ(::lstat(path.c_str(), &st), 0);
      if (S_ISLNK(st.st_mode)) continue;
      locked_.emplace_back(path.string(), st.st_mode & 07777);
      ASSERT_EQ(::chmod(path.c_str(), 0), 0);
    }
  }

  void Unlock() {
    for (auto it = locked_.rbegin(); it != locked_.rend(); ++it) {
      ::chmod(it->first.c_str(), it->second);
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
    EXPECT_EQ(::unlink(Path(name).c_str()), 0);
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

  ASSERT_EQ(::unlink(Path("fifo").c_str()), 0);
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
  int file_fd = ::open(Path("file").c_str(), O_PATH | O_CLOEXEC);
  ASSERT_GE(file_fd, 0);
  FileDescriptor file(file_fd);
  int link_fd = ::open(Path("link").c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  ASSERT_GE(link_fd, 0);
  FileDescriptor link(link_fd);

  EXPECT_THAT(ReadGeneration(*link, S_IFLNK), IsOkAndHolds(0u));
  EXPECT_THAT(ReadGeneration(*file, S_IFIFO), IsOkAndHolds(0u));

  struct statfs sfs {};
  ASSERT_EQ(::fstatfs(*file, &sfs), 0);
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
  char path[] = "/tmp/dcfs_backing_test_tmpfs_XXXXXX";
  int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  FileDescriptor file(fd);

  struct statfs sfs {};
  ASSERT_EQ(::fstatfs(*file, &sfs), 0);
  ASSERT_EQ(sfs.f_type, TMPFS_MAGIC) << "/tmp is not tmpfs in this guest";

  EXPECT_THAT(ReadGeneration(*file, S_IFREG), IsOkAndHolds(0u));
  ::unlink(path);
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
  int path_fd = ::open(Path("file").c_str(), O_PATH | O_CLOEXEC);
  ASSERT_GE(path_fd, 0);
  FileDescriptor path(path_fd);

  ASSERT_OK_AND_ASSIGN(FileDescriptor reopened, ReopenFd(*path, O_RDWR));
  ASSERT_OK_AND_ASSIGN(struct stat original, syscalls::fstat(*path));
  ASSERT_OK_AND_ASSIGN(struct stat now, syscalls::fstat(*reopened));
  EXPECT_EQ(original.st_ino, now.st_ino);
  EXPECT_NE(::fcntl(*reopened, F_GETFD) & FD_CLOEXEC, 0);

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
  int file_fd = ::open(Path("file").c_str(), O_PATH | O_CLOEXEC);
  ASSERT_GE(file_fd, 0);
  FileDescriptor file(file_fd);
  ASSERT_OK_AND_ASSIGN(auto file_xattrs, ReadXattrsFd(*file));
  EXPECT_THAT(file_xattrs, ::testing::Contains(
                               std::make_pair(std::string("user.test"),
                                              std::string("value"))));

  int link_fd = ::open(Path("link").c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  ASSERT_GE(link_fd, 0);
  FileDescriptor link(link_fd);
  ASSERT_OK_AND_ASSIGN(auto link_xattrs, ReadXattrsFd(*link));
  EXPECT_THAT(link_xattrs, ::testing::IsEmpty());
}

TEST_F(BackingTest, ReadSymlinkFdGrowsItsBufferForALongTarget) {
  const std::string long_target(500, 'a');  // past the first 256 bytes
  ASSERT_EQ(::symlink(long_target.c_str(), Path("long_link").c_str()), 0);
  int fd = ::open(Path("long_link").c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileDescriptor link(fd);
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
  ASSERT_EQ(::mkdir(Path("mnt").c_str(), 0755), 0);
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(FakeMount mount, AddFakeMount(ctx_, "mnt"));
  ASSERT_EQ(::rmdir(Path("mnt").c_str()), 0);

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

// --- Crash safety of writable opens (step 4.6) -------------------------------

TEST_F(BackingTest, RefreshAttrsFromFdMarksValid) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, file), IsOk());
  WriteFile(Path("file"), "longer contents");
  int fd = ::open(Path("file").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileDescriptor owned(fd);

  ASSERT_THAT(RefreshAttrsFromFd(ctx_, file, fd), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, file));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 15);
}

TEST_F(BackingTest, AttrsOfAFileOpenForWriteStayUnknown) {
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  absl::flat_hash_set<int64_t> open_for_write = {file};
  ctx_.open_for_write = &open_for_write;
  int fd = ::open(Path("file").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileDescriptor owned(fd);
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
  int fd = ::open(Path("file").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileDescriptor owned(fd);
  ASSERT_EQ(::unlink(Path("file").c_str()), 0);

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
  ::nanosleep(&ts, nullptr);
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
  ASSERT_EQ(::chmod(Path("file").c_str(), 0600), 0);
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
  int fd = ::open(Path("file").c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::write(fd, " world", 6), 6);
  ::close(fd);
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
  ASSERT_EQ(::utimensat(AT_FDCWD, Path("file").c_str(), times, 0), 0);
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
  ASSERT_EQ(::setxattr(Path("file").c_str(), "user.added", "x", 1, 0), 0);
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
  ASSERT_EQ(::chmod(Path("file").c_str(), 0600), 0);
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
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
  EXPECT_THAT(GetBootId(db_), IsOkAndHolds(Optional(std::string("boot-2"))));

  // And the truth is re-read from the backing filesystem: inner still
  // exists there (the phase 2 unlink never ran in this test).
  EXPECT_THAT(LookupOrPopulate(ctx_, dir, "inner"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
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
    ASSERT_EQ(::mkdir(Path("boundary").c_str(), 0755), 0);
    ASSERT_EQ(::mount("tmpfs", Path("boundary").c_str(), "tmpfs", 0, nullptr),
              0)
        << std::strerror(errno);
    mounted_ = true;
  }

  void TearDown() override {
    if (mounted_) ::umount2(Path("boundary").c_str(), MNT_DETACH);
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
  struct stat root {};
  ASSERT_EQ(::stat(Path("boundary").c_str(), &root), 0);
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
  ASSERT_EQ(::mkdir(Path("dir/sub").c_str(), 0755), 0);
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
  EXPECT_EQ(::mkdir(path.c_str(), 0), 0) << std::strerror(errno);
  EXPECT_EQ(::chown(path.c_str(), 0, gid), 0) << std::strerror(errno);
  EXPECT_EQ(::chmod(path.c_str(), mode), 0) << std::strerror(errno);
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  EXPECT_GE(fd, 0) << std::strerror(errno);
  return FileDescriptor(fd);
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

TEST_F(BackingTest, CallerIdentityThatDoesNotTakeIsRefused) {
  ASSERT_OK_AND_ASSIGN(std::vector<gid_t> groups, GetGroups());
  FileDescriptor parent = MakeParent(Path("pub"), 0777, 0);
  // -1 is what the kernel would send for an id with no mapping; setfsuid
  // and setfsgid silently ignore it, so only reading back catches it.
  const Credentials bad_uid{
      .uid = static_cast<uid_t>(-1), .gid = 1000, .groups = {}};
  const Credentials bad_gid{
      .uid = 1000, .gid = static_cast<gid_t>(-1), .groups = {}};
  EXPECT_EQ(ErrnoOf(MkdirAt(ctx_, bad_uid, *parent, "d", 0755)), EPERM);
  ExpectRootAgain(groups);
  EXPECT_EQ(ErrnoOf(MkdirAt(ctx_, bad_gid, *parent, "d", 0755)), EPERM);
  ExpectRootAgain(groups);
  EXPECT_EQ(::access(Path("pub/d").c_str(), F_OK), -1);
}

TEST_F(BackingTest, UnlinkAndRenameHonorTheStickyBit) {
  ASSERT_OK_AND_ASSIGN(std::vector<gid_t> groups, GetGroups());
  FileDescriptor sticky = MakeParent(Path("sticky"), 01777, 0);
  const Credentials alice{.uid = 1000, .gid = 1000, .groups = {}};
  const Credentials bob{.uid = 1001, .gid = 1001, .groups = {}};
  ASSERT_THAT(CreateAt(ctx_, bob, *sticky, "bf", O_WRONLY, 0644), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId dir, Id("sticky"));

  EXPECT_EQ(ErrnoOf(UnlinkAt(ctx_, alice, dir, "bf", 0)), EPERM);
  EXPECT_EQ(ErrnoOf(RenameAt(ctx_, alice, dir, "bf", dir, "stolen", 0)),
            EPERM);
  EXPECT_THAT(RenameAt(ctx_, bob, dir, "bf", dir, "bf2", 0), IsOk());
  EXPECT_THAT(UnlinkAt(ctx_, bob, dir, "bf2", 0), IsOk());
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
        "/proc/self/task/" + std::to_string(::getpid()) + "/status";
    FILE *f = std::fopen(status_path.c_str(), "r");
    ASSERT_NE(f, nullptr);
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      if (std::strncmp(line, "Groups:", 7) != 0) continue;
      char *p = line + 7;
      char *end;
      for (unsigned long g = std::strtoul(p, &end, 10); end != p;
           g = std::strtoul(p, &end, 10)) {
        in_main_meanwhile.push_back(static_cast<gid_t>(g));
        p = end;
      }
    }
    std::fclose(f);
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
