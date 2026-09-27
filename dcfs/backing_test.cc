#include "dcfs/backing.h"

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// This version of Abseil's status_matchers.h doesn't provide
// ASSERT_OK_AND_ASSIGN, so define the usual helper locally (scoped to this
// file only) -- same as dcfs/metadata_cache_test.cc.
#define DCFS_TEST_CONCAT_INNER(x, y) x##y
#define DCFS_TEST_CONCAT(x, y) DCFS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                        \
  ASSERT_OK_AND_ASSIGN_IMPL(                                    \
      DCFS_TEST_CONCAT(_status_or_value_, __LINE__), lhs, rexpr)
#define ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());        \
  lhs = std::move(statusor).value()

namespace dcfs::backing {
namespace {

namespace fs = std::filesystem;

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Optional;
using cache::InodeId;
using cache::kRootInode;
using cache::LookupResult;

MATCHER_P(IsLookup, kind, "") { return arg.kind == kind; }

// A stand-in for GetDeviceId on kernels without FS_IOC_GETFSUUID: stable
// per filesystem (and per Btrfs subvolume, via st_dev), which is all the
// backing layer needs from a DeviceId.
absl::StatusOr<DeviceId> FakeDeviceId(int fd) {
  struct statfs sfs {};
  if (::fstatfs(fd, &sfs) != 0) return ErrnoToStatus(errno, "fstatfs");
  struct statx stx {};
  if (::statx(fd, "", AT_EMPTY_PATH, 0, &stx) != 0) {
    return ErrnoToStatus(errno, "statx");
  }
  DeviceId id;
  static_assert(sizeof(sfs.f_fsid) == 8);
  std::memcpy(id.uuid.data(), &sfs.f_fsid, 8);
  std::memcpy(id.uuid.data() + 8, &stx.stx_dev_major, 4);
  std::memcpy(id.uuid.data() + 12, &stx.stx_dev_minor, 4);
  return id;
}

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

  // Whether open_by_handle_at works here: it needs CAP_DAC_READ_SEARCH,
  // which an unprivileged test run lacks.
  bool HandlesWork() {
    absl::StatusOr<DeviceId> device = FakeDeviceId(source_fd_);
    if (!device.ok()) return false;
    absl::StatusOr<FileHandle> handle = FileHandle::FromFd(source_fd_, *device);
    if (!handle.ok()) return false;
    return handle->Open(mounts_, O_PATH).ok();
  }

  absl::StatusOr<InodeId> Id(std::string_view name, InodeId dir = kRootInode) {
    absl::StatusOr<LookupResult> result = LookupOrPopulate(ctx_, dir, name);
    if (!result.ok()) return result.status();
    if (result->kind != LookupResult::kFound) {
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
  Context ctx_{db_, mounts_, &FakeDeviceId};
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
              IsOkAndHolds(IsLookup(LookupResult::kFound)));
  const std::vector<std::string> names = {"dir", "fifo", "file",
                                          "hl1", "hl2",  "link"};
  std::vector<struct statx> expected;
  for (const std::string &name : names) expected.push_back(StatPath(Path(name)));

  Lock();
  if (::geteuid() != 0) {
    // The control: the backing filesystem really is unreachable now.
    EXPECT_EQ(ErrnoOf(StatNode(ctx_, kRootInode).status()), EACCES);
  }

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
    EXPECT_EQ(attr.device, *FakeDeviceId(source_fd_));
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
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "missing"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));

  Lock();
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "missing"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));
  // The listing is complete, so another absent name needs no I/O either.
  EXPECT_THAT(LookupOrPopulate(ctx_, kRootInode, "also_missing"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));
}

TEST_F(BackingTest, RepopulationTracksChangesOnDisk) {
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId file, Id("file"));
  ASSERT_THAT(cache::SetNegative(ctx_, kRootInode, "ghost"), IsOk());

  ASSERT_EQ(::unlink(Path("fifo").c_str()), 0);
  WriteFile(Path("new"), "new");
  ASSERT_THAT(PopulateDirectory(ctx_, kRootInode), IsOk());

  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "fifo"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "ghost"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "new"),
              IsOkAndHolds(IsLookup(LookupResult::kFound)));
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
  if (!HandlesWork()) GTEST_SKIP() << "needs CAP_DAC_READ_SEARCH";
  ASSERT_OK_AND_ASSIGN(InodeId old_id, Id("file"));
  ASSERT_THAT(OpenNode(ctx_, old_id, O_RDONLY), IsOk());
  if (!Recreate("file")) GTEST_SKIP() << "the inode number was not reused";

  absl::StatusOr<FileDescriptor> fd = OpenNode(ctx_, old_id, O_RDONLY);
  ASSERT_FALSE(fd.ok());
  EXPECT_EQ(ErrnoOf(fd.status()), ESTALE) << fd.status();
  EXPECT_THAT(cache::GetAttr(ctx_, old_id),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "file"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
}

TEST_F(BackingTest, BackingReadsByInode) {
  // The root is reachable without handle decoding.
  ASSERT_OK_AND_ASSIGN(struct statx root_stx, StatNode(ctx_, kRootInode));
  EXPECT_EQ(root_stx.stx_ino, StatPath(source_).stx_ino);
  EXPECT_THAT(ReadXattrs(ctx_, kRootInode), IsOk());

  if (!HandlesWork()) GTEST_SKIP() << "needs CAP_DAC_READ_SEARCH";
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
              IsOkAndHolds(IsLookup(LookupResult::kFound)));
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
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(cache::IsDirComplete(ctx, kRootInode), IsOkAndHolds(false));
  // The source filesystem is never purged.
  ASSERT_OK_AND_ASSIGN(std::vector<cache::FilesystemRow> filesystems,
                       cache::ListFilesystems(ctx));
  ASSERT_EQ(filesystems.size(), 1u);
  EXPECT_FALSE(filesystems[0].parent_inode.has_value());
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
  Context other{other_db, other_mounts, &FakeDeviceId};
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd, syscalls::dup(source_fd_));
  EXPECT_THAT(InitRoot(other, std::move(fd)),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_EQ(other_mounts.size(), 0u);
}

TEST_F(BackingTest, RealDeviceIdFunctionIsTheDefault) {
  MountFds mounts;
  Context real{db_, mounts};
  EXPECT_EQ(real.device_id_fn, &GetDeviceId);
  absl::StatusOr<DeviceId> direct = GetDeviceId(source_fd_);
  if (direct.ok()) GTEST_SKIP() << "this kernel supports FS_IOC_GETFSUUID";
  // Linux 6.8 (this host) predates FS_IOC_GETFSUUID.
  EXPECT_THAT(ProbeRoot(real, source_fd_),
              StatusIs(absl::StatusCode::kUnimplemented));
}

}  // namespace
}  // namespace dcfs::backing
