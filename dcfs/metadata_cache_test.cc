#include "dcfs/metadata_cache.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/file_handle.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/ret_check.h"
#include "dcfs/sqlite.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// This version of Abseil's status_matchers.h doesn't provide
// ASSERT_OK_AND_ASSIGN, so define the usual helper locally (scoped to this
// file only) -- same as dcfs/migrate_test.cc.
#define DCFS_TEST_CONCAT_INNER(x, y) x##y
#define DCFS_TEST_CONCAT(x, y) DCFS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                        \
  ASSERT_OK_AND_ASSIGN_IMPL(                                    \
      DCFS_TEST_CONCAT(_status_or_value_, __LINE__), lhs, rexpr)
#define ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());        \
  lhs = std::move(statusor).value()

namespace dcfs::cache {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::Optional;
using ::testing::Pair;

DeviceId TestDeviceId(uint8_t fill) {
  DeviceId id;
  id.uuid.fill(fill);
  return id;
}

const DeviceId kSource = TestDeviceId(0xAB);
const DeviceId kSecond = TestDeviceId(0xCD);
const DeviceId kThird = TestDeviceId(0xEF);

constexpr uint64_t kRootIno = 2;

FileHandle Handle(const DeviceId &device, std::string_view bytes,
                  int handle_type = 1) {
  return FileHandle{
      .device = device,
      .handle_type = handle_type,
      .bytes = std::vector<uint8_t>(bytes.begin(), bytes.end()),
  };
}

// A fabricated statx with a distinct value in every field Upsert/UpdateAttr
// store, derived from `ino` and `seed` so tests can tell versions apart.
struct statx Stx(uint64_t ino, mode_t mode, int64_t seed = 0) {
  struct statx stx {};
  stx.stx_mask = STATX_BASIC_STATS | STATX_BTIME;
  stx.stx_ino = ino;
  stx.stx_mode = mode;
  stx.stx_nlink = 3 + seed;
  stx.stx_uid = 1000 + seed;
  stx.stx_gid = 2000 + seed;
  stx.stx_rdev_major = 8;
  stx.stx_rdev_minor = 17 + seed;
  stx.stx_size = (uint64_t{1} << 40) + seed;
  stx.stx_blocks = 4096 + seed;
  stx.stx_blksize = 512;
  stx.stx_atime = {.tv_sec = 1'700'000'001 + seed, .tv_nsec = 111};
  stx.stx_mtime = {.tv_sec = 1'700'000'002 + seed, .tv_nsec = 222};
  stx.stx_ctime = {.tv_sec = 1'700'000'003 + seed, .tv_nsec = 333};
  stx.stx_btime = {.tv_sec = -5 - seed, .tv_nsec = 999'999'999};
  return stx;
}

absl::StatusOr<int64_t> CountRows(sqlite3::Connection &db,
                                  std::string_view from_where) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared(absl::StrCat("SELECT COUNT(*) FROM ", from_where)));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  RET_CHECK(has_row);
  int64_t count = stmt->Column<int64_t>(0);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return count;
}

std::vector<std::string> ListNames(Context &ctx, InodeId dir) {
  std::vector<std::string> names;
  absl::Status status =
      ListDir(ctx, dir, 0, [&](std::string_view name, InodeId, int64_t) {
        names.emplace_back(name);
        return true;
      });
  EXPECT_THAT(status, IsOk());
  return names;
}

MATCHER_P(IsLookup, kind, "") { return arg.kind == kind; }
MATCHER_P(IsFoundAs, id, "") {
  return arg.kind == LookupResult::kFound && arg.id == id;
}

class MetadataCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
    ASSERT_THAT(Migrate(db_, RootIdentity{.device_id = kSource,
                                          .fstype = 0xEF53,
                                          .backing_ino = kRootIno,
                                          .backing_gen = 0}),
                IsOk());
  }

  // Creates a row for (device, ino, gen) with a handle derived from `ino`.
  absl::StatusOr<UpsertResult> Make(uint64_t ino, mode_t mode = S_IFREG | 0644,
                                    const DeviceId &device = kSource,
                                    uint64_t gen = 0) {
    return UpsertInode(ctx_, Handle(device, absl::StrCat("h", ino)),
                       Stx(ino, mode), gen);
  }

  // Creates a directory row linked as (parent, name).
  absl::StatusOr<InodeId> MakeDir(InodeId parent, std::string_view name,
                                  uint64_t ino,
                                  const DeviceId &device = kSource) {
    ABSL_ASSIGN_OR_RETURN(UpsertResult r, Make(ino, S_IFDIR | 0755, device));
    ABSL_RETURN_IF_ERROR(LinkDentry(ctx_, parent, name, r.id));
    ABSL_RETURN_IF_ERROR(MarkDirComplete(ctx_, r.id, false));
    return r.id;
  }

  sqlite3::Connection db_;
  MountFds mounts_;
  Context ctx_{db_, mounts_};
};

TEST_F(MetadataCacheTest, UpsertCreatesThenUpdatesSameIdentity) {
  ASSERT_OK_AND_ASSIGN(UpsertResult first, Make(10));
  EXPECT_TRUE(first.created);
  EXPECT_NE(first.id, kRootInode);
  EXPECT_NE(first.fuse_gen, 0u);

  // Same identity, new attributes and handle: same row, updated in place.
  ASSERT_THAT(MarkAttrsUnknown(ctx_, first.id), IsOk());
  ASSERT_OK_AND_ASSIGN(
      UpsertResult second,
      UpsertInode(ctx_, Handle(kSource, "new", 7), Stx(10, S_IFREG, 5), 0));
  EXPECT_FALSE(second.created);
  EXPECT_EQ(second.id, first.id);
  EXPECT_EQ(second.fuse_gen, first.fuse_gen);

  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, first.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_uid, 1005u);
  EXPECT_THAT(GetHandle(ctx_, first.id),
              IsOkAndHolds(Handle(kSource, "new", 7)));
  EXPECT_THAT(GetGeneration(ctx_, first.id), IsOkAndHolds(first.fuse_gen));
}

TEST_F(MetadataCacheTest, UpsertOnUnregisteredFilesystemFails) {
  EXPECT_THAT(Make(10, S_IFREG, kSecond),
              StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(1));
}

TEST_F(MetadataCacheTest, HardLinksShareOneRow) {
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(kRootInode, "b", 21));
  ASSERT_OK_AND_ASSIGN(UpsertResult file, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, a, "one", file.id), IsOk());

  // Discovering the second link re-upserts the same backing identity.
  ASSERT_OK_AND_ASSIGN(UpsertResult again, Make(30));
  EXPECT_FALSE(again.created);
  EXPECT_EQ(again.id, file.id);
  ASSERT_THAT(LinkDentry(ctx_, b, "two", again.id), IsOk());

  EXPECT_THAT(Lookup(ctx_, a, "one"), IsOkAndHolds(IsFoundAs(file.id)));
  EXPECT_THAT(Lookup(ctx_, b, "two"), IsOkAndHolds(IsFoundAs(file.id)));
  EXPECT_THAT(CountRows(db_, "inodes WHERE backing_ino = 30"),
              IsOkAndHolds(1));
}

TEST_F(MetadataCacheTest, NegativeEntries) {
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "x"), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));

  ASSERT_OK_AND_ASSIGN(UpsertResult file, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "x", file.id), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"), IsOkAndHolds(IsFoundAs(file.id)));

  // And back: a negative entry replaces a positive one.
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "x"), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "never"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));

  // Names are byte strings, including embedded NULs.
  std::string odd("a\0b\xff", 4);
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, odd, file.id), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, odd), IsOkAndHolds(IsFoundAs(file.id)));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "a"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));

  EXPECT_THAT(SetNegative(ctx_, 999, "x"),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(LinkDentry(ctx_, kRootInode, "y", 999),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, RenameAcrossParents) {
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(kRootInode, "b", 21));
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_THAT(LinkDentry(ctx_, a, "f", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, b, "g", g.id), IsOk());

  ASSERT_THAT(RenameDentry(ctx_, a, "f", b, "moved"), IsOk());
  EXPECT_THAT(Lookup(ctx_, a, "f"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, b, "moved"), IsOkAndHolds(IsFoundAs(f.id)));

  // Replacing an existing target.
  ASSERT_THAT(RenameDentry(ctx_, b, "moved", b, "g"), IsOk());
  EXPECT_THAT(Lookup(ctx_, b, "g"), IsOkAndHolds(IsFoundAs(f.id)));
  EXPECT_THAT(Lookup(ctx_, b, "moved"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  // The replaced inode's row survives (it may have other links).
  EXPECT_THAT(GetAttr(ctx_, g.id), IsOk());

  // Renaming onto itself is a no-op.
  ASSERT_THAT(RenameDentry(ctx_, b, "g", b, "g"), IsOk());
  EXPECT_THAT(Lookup(ctx_, b, "g"), IsOkAndHolds(IsFoundAs(f.id)));

  // A directory's parent follows its dentry.
  ASSERT_THAT(RenameDentry(ctx_, kRootInode, "b", a, "b"), IsOk());
  EXPECT_THAT(ParentOf(ctx_, b), IsOkAndHolds(a));

  EXPECT_THAT(RenameDentry(ctx_, a, "missing", b, "x"),
              StatusIs(absl::StatusCode::kNotFound));
  ASSERT_THAT(SetNegative(ctx_, a, "neg"), IsOk());
  EXPECT_THAT(RenameDentry(ctx_, a, "neg", b, "x"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, UnlinkDentryLeavesInodeRow) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "f", f.id), IsOk());
  ASSERT_THAT(UnlinkDentry(ctx_, kRootInode, "f"), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "f"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(GetAttr(ctx_, f.id), IsOk());
  // Absent: no-op.
  EXPECT_THAT(UnlinkDentry(ctx_, kRootInode, "f"), IsOk());
}

TEST_F(MetadataCacheTest, DeleteInodeRemovesEverything) {
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(kRootInode, "b", 21));
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(a, "d", 22));
  ASSERT_OK_AND_ASSIGN(UpsertResult child, Make(23));
  ASSERT_THAT(LinkDentry(ctx_, d, "child", child.id), IsOk());
  ASSERT_OK_AND_ASSIGN(UpsertResult link, Make(24, S_IFLNK | 0777));
  ASSERT_THAT(LinkDentry(ctx_, a, "l1", link.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, b, "l2", link.id), IsOk());
  ASSERT_THAT(SetSymlink(ctx_, link.id, "target"), IsOk());
  ASSERT_THAT(SetXattr(ctx_, link.id, "user.k", "v"), IsOk());
  ASSERT_THAT(SetXattr(ctx_, d, "user.k", "v"), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, a, true), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, b, true), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, d, true), IsOk());

  ASSERT_THAT(DeleteInode(ctx_, link.id), IsOk());
  EXPECT_THAT(GetAttr(ctx_, link.id), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Readlink(ctx_, link.id), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Lookup(ctx_, a, "l1"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, b, "l2"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, a), IsOkAndHolds(false));
  EXPECT_THAT(IsDirComplete(ctx_, b), IsOkAndHolds(false));
  EXPECT_THAT(CountRows(db_, absl::StrCat("xattrs WHERE inode = ", link.id)),
              IsOkAndHolds(0));

  // A directory takes its directories row, xattrs and child dentries with
  // it, but not the child's inode row.
  ASSERT_THAT(DeleteInode(ctx_, d), IsOk());
  EXPECT_THAT(Lookup(ctx_, a, "d"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(CountRows(db_, absl::StrCat("directories WHERE inode = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, absl::StrCat("dentries WHERE parent = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, absl::StrCat("xattrs WHERE inode = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(GetAttr(ctx_, child.id), IsOk());
  // Nothing was ever turned into a negative entry.
  EXPECT_THAT(CountRows(db_, "dentries WHERE inode IS NULL"), IsOkAndHolds(0));

  EXPECT_THAT(DeleteInode(ctx_, d), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(InvalidateInode(ctx_, kRootInode),
              StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(GetAttr(ctx_, kRootInode), IsOk());
}

TEST_F(MetadataCacheTest, RecycledBackingInodeGetsNewRow) {
  ASSERT_OK_AND_ASSIGN(InodeId dir, MakeDir(kRootInode, "dir", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult old, Make(30, S_IFREG, kSource, 1));
  ASSERT_THAT(LinkDentry(ctx_, dir, "f", old.id), IsOk());
  ASSERT_THAT(SetXattr(ctx_, old.id, "user.k", "v"), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, dir, true), IsOk());

  ASSERT_OK_AND_ASSIGN(UpsertResult fresh, Make(30, S_IFREG, kSource, 2));
  EXPECT_TRUE(fresh.created);
  EXPECT_NE(fresh.id, old.id);
  EXPECT_NE(fresh.fuse_gen, old.fuse_gen);
  EXPECT_THAT(GetAttr(ctx_, old.id), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Lookup(ctx_, dir, "f"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, dir), IsOkAndHolds(false));
  EXPECT_THAT(CountRows(db_, "xattrs"), IsOkAndHolds(0));
  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, fresh.id));
  EXPECT_EQ(attr.backing_gen, 2u);
  EXPECT_EQ(attr.backing_ino, 30u);
}

TEST_F(MetadataCacheTest, PurgeFilesystem) {
  // Root rows that must survive.
  ASSERT_OK_AND_ASSIGN(UpsertResult keep, Make(40));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "keep", keep.id), IsOk());
  ASSERT_THAT(SetXattr(ctx_, keep.id, "user.k", "v"), IsOk());

  // Second filesystem mounted at /mnt.
  ASSERT_THAT(AddFilesystem(ctx_, kSecond, 0x9123683E, kRootInode, "mnt"),
              IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId mnt, MakeDir(kRootInode, "mnt", 256, kSecond));
  ASSERT_OK_AND_ASSIGN(UpsertResult f2, Make(300, S_IFREG, kSecond));
  ASSERT_THAT(LinkDentry(ctx_, mnt, "f", f2.id), IsOk());
  ASSERT_THAT(SetXattr(ctx_, f2.id, "user.k", "v"), IsOk());
  ASSERT_THAT(SetNegative(ctx_, mnt, "neg"), IsOk());

  // Third filesystem mounted at /mnt/sub.
  ASSERT_THAT(AddFilesystem(ctx_, kThird, 0x58465342, mnt, "sub"), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId sub, MakeDir(mnt, "sub", 2, kThird));
  ASSERT_OK_AND_ASSIGN(UpsertResult f3, Make(500, S_IFREG, kThird));
  ASSERT_THAT(LinkDentry(ctx_, sub, "f", f3.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, kRootInode, true), IsOk());

  EXPECT_THAT(PurgeFilesystem(ctx_, kSource),
              StatusIs(absl::StatusCode::kInternal));

  ASSERT_THAT(PurgeFilesystem(ctx_, kSecond), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "mnt"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
  for (InodeId id : {mnt, f2.id, sub, f3.id}) {
    EXPECT_THAT(GetAttr(ctx_, id), StatusIs(absl::StatusCode::kNotFound))
        << id;
  }
  EXPECT_THAT(GetFilesystem(ctx_, kSecond),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(GetFilesystem(ctx_, kThird),
              StatusIs(absl::StatusCode::kNotFound));
  // Only the root's rows remain.
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(2));
  EXPECT_THAT(CountRows(db_, "dentries"), IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "xattrs"), IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "directories"), IsOkAndHolds(1));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "keep"),
              IsOkAndHolds(IsFoundAs(keep.id)));

  EXPECT_THAT(PurgeFilesystem(ctx_, kSecond),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, Filesystems) {
  ASSERT_OK_AND_ASSIGN(FilesystemRow source, GetFilesystem(ctx_, kSource));
  EXPECT_EQ(source.device, kSource);
  EXPECT_EQ(source.fstype, 0xEF53);
  EXPECT_EQ(source.parent_inode, std::nullopt);
  EXPECT_EQ(source.boundary_name, std::nullopt);

  ASSERT_THAT(AddFilesystem(ctx_, kSecond, 42, kRootInode, "mnt"), IsOk());
  EXPECT_THAT(AddFilesystem(ctx_, kSecond, 42, kRootInode, "mnt"),
              StatusIs(absl::StatusCode::kAlreadyExists));
  EXPECT_THAT(AddFilesystem(ctx_, kThird, 42, 999, "x"),
              StatusIs(absl::StatusCode::kNotFound));

  ASSERT_OK_AND_ASSIGN(std::vector<FilesystemRow> all, ListFilesystems(ctx_));
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].device, kSource);
  EXPECT_EQ(all[1].device, kSecond);
  EXPECT_EQ(all[1].fstype, 42);
  EXPECT_EQ(all[1].parent_inode, std::optional<InodeId>(kRootInode));
  EXPECT_EQ(all[1].boundary_name, std::optional<std::string>("mnt"));
}

TEST_F(MetadataCacheTest, MarkUnknownIsPhaseOne) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "f", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "g", g.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "n"), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "stays", g.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, kRootInode, true), IsOk());

  std::vector<std::string> names = {"f", "g", "n", "absent"};
  ASSERT_THAT(MarkUnknown(ctx_, kRootInode, names), IsOk());
  for (const std::string &name : names) {
    EXPECT_THAT(Lookup(ctx_, kRootInode, name),
                IsOkAndHolds(IsLookup(LookupResult::kUnknown)))
        << name;
  }
  EXPECT_THAT(Lookup(ctx_, kRootInode, "stays"),
              IsOkAndHolds(IsFoundAs(g.id)));
  EXPECT_THAT(IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
  EXPECT_THAT(GetAttr(ctx_, f.id), IsOk());
  EXPECT_THAT(GetAttr(ctx_, g.id), IsOk());
}

TEST_F(MetadataCacheTest, IsDirComplete) {
  ASSERT_OK_AND_ASSIGN(UpsertResult d, Make(20, S_IFDIR));
  // No directories row yet: incomplete.
  EXPECT_THAT(IsDirComplete(ctx_, d.id), IsOkAndHolds(false));
  ASSERT_THAT(MarkDirComplete(ctx_, d.id, true), IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, d.id), IsOkAndHolds(true));
  ASSERT_THAT(MarkDirComplete(ctx_, d.id, false), IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, d.id), IsOkAndHolds(false));
  EXPECT_THAT(MarkDirComplete(ctx_, 999, true),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, ListDirPagesWithCursor) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "one", f.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "negative"), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "two", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "three", f.id), IsOk());

  std::vector<std::string> seen;
  int64_t cursor = 0;
  for (int page = 0; page < 5; ++page) {
    int calls = 0;
    ASSERT_THAT(ListDir(ctx_, kRootInode, cursor,
                        [&](std::string_view name, InodeId child,
                            int64_t next) -> absl::StatusOr<bool> {
                          ++calls;
                          EXPECT_EQ(child, f.id);
                          seen.emplace_back(name);
                          cursor = next;
                          return false;  // page size 1
                        }),
                IsOk());
    if (calls == 0) break;
  }
  EXPECT_THAT(seen, ElementsAre("one", "two", "three"));

  // Replacing an entry in place keeps its position.
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "one", f.id), IsOk());
  EXPECT_THAT(ListNames(ctx_, kRootInode), ElementsAre("one", "two", "three"));

  // Callback errors propagate.
  EXPECT_THAT(ListDir(ctx_, kRootInode, 0,
                      [](std::string_view, InodeId,
                         int64_t) -> absl::StatusOr<bool> {
                        return absl::AbortedError("stop");
                      }),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(MetadataCacheTest, ListDirSpansBatchesAndAllowsWritesInCallback) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  constexpr int kEntries = 150;  // more than one internal batch
  for (int i = 0; i < kEntries; ++i) {
    ASSERT_THAT(LinkDentry(ctx_, kRootInode, absl::StrCat("e", i), f.id),
                IsOk());
  }
  int count = 0;
  ASSERT_THAT(ListDir(ctx_, kRootInode, 0,
                      [&](std::string_view name, InodeId,
                          int64_t) -> absl::StatusOr<bool> {
                        EXPECT_EQ(name, absl::StrCat("e", count));
                        ++count;
                        // Reads and writes from inside the callback.
                        ABSL_RETURN_IF_ERROR(
                            Lookup(ctx_, kRootInode, name).status());
                        ABSL_RETURN_IF_ERROR(
                            SetNegative(ctx_, kRootInode,
                                        absl::StrCat("neg", count)));
                        return true;
                      }),
              IsOk());
  EXPECT_EQ(count, kEntries);
}

TEST_F(MetadataCacheTest, FuseGenerations) {
  EXPECT_THAT(GetGeneration(ctx_, kRootInode), IsOkAndHolds(0u));
  std::vector<uint32_t> gens;
  for (uint64_t ino = 100; ino < 120; ++ino) {
    ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(ino));
    EXPECT_NE(r.fuse_gen, 0u);
    EXPECT_THAT(GetGeneration(ctx_, r.id), IsOkAndHolds(r.fuse_gen));
    for (uint32_t g : gens) EXPECT_NE(g, r.fuse_gen);
    gens.push_back(r.fuse_gen);
  }
  EXPECT_THAT(GetGeneration(ctx_, 999), StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, UpsertRootUpdatesInPlace) {
  ASSERT_OK_AND_ASSIGN(int64_t before, CountRows(db_, "inodes"));
  ASSERT_THAT(UpsertRoot(ctx_, Handle(kSource, "root", 3),
                         Stx(kRootIno, S_IFDIR | 0755), 7),
              IsOk());
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(before));
  EXPECT_THAT(GetGeneration(ctx_, kRootInode), IsOkAndHolds(0u));
  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, kRootInode));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.backing_gen, 7u);
  EXPECT_EQ(attr.st.st_mode, S_IFDIR | 0755);
  EXPECT_THAT(GetHandle(ctx_, kRootInode),
              IsOkAndHolds(Handle(kSource, "root", 3)));

  // A handle on another device is refused.
  ASSERT_THAT(AddFilesystem(ctx_, kSecond, 1, kRootInode, "mnt"), IsOk());
  EXPECT_THAT(UpsertRoot(ctx_, Handle(kSecond, "x"), Stx(2, S_IFDIR), 0),
              StatusIs(absl::StatusCode::kInternal));
}

TEST_F(MetadataCacheTest, GetAttrRoundTripsEveryField) {
  // Fresh root: no attributes yet.
  ASSERT_OK_AND_ASSIGN(CachedAttr root, GetAttr(ctx_, kRootInode));
  EXPECT_FALSE(root.valid);
  EXPECT_EQ(root.fuse_gen, 0u);
  EXPECT_EQ(root.device, kSource);
  EXPECT_EQ(root.st.st_ino, kRootIno);

  struct statx stx = Stx(77, S_IFCHR | 0600, 9);
  ASSERT_OK_AND_ASSIGN(UpsertResult r,
                       UpsertInode(ctx_, Handle(kSource, "h"), stx, 12));
  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, r.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.fuse_gen, r.fuse_gen);
  EXPECT_EQ(attr.device, kSource);
  EXPECT_EQ(attr.backing_ino, 77u);
  EXPECT_EQ(attr.backing_gen, 12u);
  EXPECT_EQ(attr.st.st_dev, 0u);
  EXPECT_EQ(attr.st.st_ino, 77u);
  EXPECT_EQ(attr.st.st_mode, S_IFCHR | 0600);
  EXPECT_EQ(attr.st.st_nlink, stx.stx_nlink);
  EXPECT_EQ(attr.st.st_uid, stx.stx_uid);
  EXPECT_EQ(attr.st.st_gid, stx.stx_gid);
  EXPECT_EQ(attr.st.st_rdev, makedev(8, 26));
  EXPECT_EQ(static_cast<uint64_t>(attr.st.st_size), stx.stx_size);
  EXPECT_EQ(static_cast<uint64_t>(attr.st.st_blocks), stx.stx_blocks);
  EXPECT_EQ(attr.st.st_blksize, 512);
  EXPECT_EQ(attr.st.st_atim.tv_sec, stx.stx_atime.tv_sec);
  EXPECT_EQ(attr.st.st_atim.tv_nsec, 111);
  EXPECT_EQ(attr.st.st_mtim.tv_sec, stx.stx_mtime.tv_sec);
  EXPECT_EQ(attr.st.st_mtim.tv_nsec, 222);
  EXPECT_EQ(attr.st.st_ctim.tv_sec, stx.stx_ctime.tv_sec);
  EXPECT_EQ(attr.st.st_ctim.tv_nsec, 333);
  EXPECT_EQ(attr.btime.tv_sec, -14);
  EXPECT_EQ(attr.btime.tv_nsec, 999'999'999);

  ASSERT_THAT(MarkAttrsUnknown(ctx_, r.id), IsOk());
  EXPECT_THAT(GetAttr(ctx_, r.id),
              IsOkAndHolds(::testing::Field(&CachedAttr::valid, false)));
  ASSERT_THAT(UpdateAttr(ctx_, r.id, Stx(77, S_IFCHR | 0644, 1)), IsOk());
  ASSERT_OK_AND_ASSIGN(attr, GetAttr(ctx_, r.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_mode, S_IFCHR | 0644);
  EXPECT_EQ(attr.st.st_uid, 1001u);

  EXPECT_THAT(MarkAttrsUnknown(ctx_, 999),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(UpdateAttr(ctx_, 999, stx),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(GetAttr(ctx_, 999), StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, Symlinks) {
  ASSERT_OK_AND_ASSIGN(UpsertResult l, Make(50, S_IFLNK | 0777));
  EXPECT_THAT(Readlink(ctx_, l.id), StatusIs(absl::StatusCode::kNotFound));
  ASSERT_THAT(SetSymlink(ctx_, l.id, "../a"), IsOk());
  EXPECT_THAT(Readlink(ctx_, l.id), IsOkAndHolds("../a"));
  ASSERT_THAT(SetSymlink(ctx_, l.id, "/b"), IsOk());
  EXPECT_THAT(Readlink(ctx_, l.id), IsOkAndHolds("/b"));
  EXPECT_THAT(SetSymlink(ctx_, 999, "x"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, Handles) {
  // The root row starts without a handle.
  EXPECT_THAT(GetHandle(ctx_, kRootInode),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(GetHandle(ctx_, 999), StatusIs(absl::StatusCode::kNotFound));
  std::string bytes("\x00\x01\xfe", 3);
  ASSERT_OK_AND_ASSIGN(
      UpsertResult r,
      UpsertInode(ctx_, Handle(kSource, bytes, 0x81), Stx(60, S_IFREG), 0));
  EXPECT_THAT(GetHandle(ctx_, r.id),
              IsOkAndHolds(Handle(kSource, bytes, 0x81)));
}

TEST_F(MetadataCacheTest, ParentOf) {
  EXPECT_THAT(ParentOf(ctx_, kRootInode), IsOkAndHolds(kRootInode));
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(a, "b", 21));
  EXPECT_THAT(ParentOf(ctx_, a), IsOkAndHolds(kRootInode));
  EXPECT_THAT(ParentOf(ctx_, b), IsOkAndHolds(a));
  ASSERT_THAT(UnlinkDentry(ctx_, a, "b"), IsOk());
  EXPECT_THAT(ParentOf(ctx_, b), StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, Xattrs) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(70));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"), IsOkAndHolds(std::nullopt));

  // An individually cached value is served even while the set is incomplete.
  ASSERT_THAT(SetXattr(ctx_, r.id, "user.a", "1"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              IsOkAndHolds(Optional(std::string("1"))));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));

  std::string binary("\x00\xff", 2);
  std::vector<std::pair<std::string, std::string>> all = {
      {"user.b", "2"}, {"user.c", binary}};
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, all), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(ElementsAre("user.b", "user.c"))));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.c"), IsOkAndHolds(Optional(binary)));
  // Complete, so an absent name is known not to exist.
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              StatusIs(absl::StatusCode::kNotFound));

  ASSERT_THAT(SetXattr(ctx_, r.id, "user.b", "22"), IsOk());
  ASSERT_THAT(SetXattr(ctx_, r.id, "user.d", "4"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.b"),
              IsOkAndHolds(Optional(std::string("22"))));
  ASSERT_THAT(RemoveXattr(ctx_, r.id, "user.c"), IsOk());
  ASSERT_THAT(RemoveXattr(ctx_, r.id, "user.zz"), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(ElementsAre("user.b", "user.d"))));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.c"),
              StatusIs(absl::StatusCode::kNotFound));

  ASSERT_THAT(MarkXattrsUnknown(ctx_, r.id), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.b"), IsOkAndHolds(std::nullopt));

  // An empty set is complete too.
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, {}), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(::testing::IsEmpty())));

  EXPECT_THAT(ListXattrs(ctx_, 999), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(SetXattr(ctx_, 999, "a", "b"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, WriteRollsBackWithCallersTransaction) {
  absl::Status status = db_.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(Make(80).status());
    ABSL_RETURN_IF_ERROR(SetNegative(ctx_, kRootInode, "n"));
    return absl::AbortedError("caller gives up");
  });
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kAborted));
  EXPECT_FALSE(db_.InTransaction());
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(1));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "n"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));

  // And a failing nested write unwinds only itself.
  status = db_.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(SetNegative(ctx_, kRootInode, "kept"));
    EXPECT_THAT(PurgeFilesystem(ctx_, kSource),
                StatusIs(absl::StatusCode::kInternal));
    EXPECT_THAT(UpsertRoot(ctx_, Handle(kThird, "x"), Stx(2, S_IFDIR), 0),
                StatusIs(absl::StatusCode::kInternal));
    return absl::OkStatus();
  });
  EXPECT_THAT(status, IsOk());
  EXPECT_FALSE(db_.InTransaction());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "kept"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));
}

TEST_F(MetadataCacheTest, EnsureDirectoryKeepsExistingCompleteness) {
  ASSERT_OK_AND_ASSIGN(UpsertResult dir, Make(20, S_IFDIR | 0755));
  EXPECT_THAT(CountRows(db_, absl::StrCat("directories WHERE inode = ", dir.id)),
              IsOkAndHolds(0));
  ASSERT_THAT(EnsureDirectory(ctx_, dir.id), IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, dir.id), IsOkAndHolds(false));
  EXPECT_THAT(CountRows(db_, absl::StrCat("directories WHERE inode = ", dir.id)),
              IsOkAndHolds(1));

  ASSERT_THAT(MarkDirComplete(ctx_, dir.id, true), IsOk());
  ASSERT_THAT(EnsureDirectory(ctx_, dir.id), IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, dir.id), IsOkAndHolds(true));

  EXPECT_THAT(EnsureDirectory(ctx_, 999),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(MetadataCacheTest, PruneDentriesNotIn) {
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(kRootInode, "b", 21));
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, a, "keep", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, a, "drop", f.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, a, "neg_keep"), IsOk());
  ASSERT_THAT(SetNegative(ctx_, a, "neg_drop"), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, b, "drop", f.id), IsOk());

  const std::vector<std::string> names = {"keep", "neg_keep", "not_cached"};
  ASSERT_THAT(PruneDentriesNotIn(ctx_, a, names), IsOk());
  EXPECT_THAT(Lookup(ctx_, a, "keep"), IsOkAndHolds(IsFoundAs(f.id)));
  EXPECT_THAT(Lookup(ctx_, a, "neg_keep"),
              IsOkAndHolds(IsLookup(LookupResult::kNegative)));
  EXPECT_THAT(Lookup(ctx_, a, "drop"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, a, "neg_drop"),
              IsOkAndHolds(IsLookup(LookupResult::kUnknown)));
  // Other directories and the inode row are untouched.
  EXPECT_THAT(Lookup(ctx_, b, "drop"), IsOkAndHolds(IsFoundAs(f.id)));
  EXPECT_THAT(GetAttr(ctx_, f.id), IsOk());

  // An empty listing forgets everything.
  ASSERT_THAT(PruneDentriesNotIn(ctx_, a, {}), IsOk());
  EXPECT_THAT(ListNames(ctx_, a), ::testing::IsEmpty());
  EXPECT_THAT(CountRows(db_, absl::StrCat("dentries WHERE parent = ", a)),
              IsOkAndHolds(0));
}

}  // namespace
}  // namespace dcfs::cache
