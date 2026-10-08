#include "dcfs/metadata_cache.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cstdint>
#include <random>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
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
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "sqlite3.h"

namespace dcfs::cache {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::Contains;
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
  // Not derived from `seed`: an object's birth time never changes, and
  // UpsertInode treats a different one as a different object.
  stx.stx_btime = {.tv_sec = -5 - static_cast<int64_t>(ino),
                   .tv_nsec = 999'999'999};
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

// Runs `fn` and returns how many SQLite virtual-machine instructions it
// executed, via a progress handler called at every instruction. A stable
// measure of the work the database did (rows scanned, rows sorted), unlike
// time, so a test can assert that a query does not scan or sort a whole
// directory.
template <typename Fn>
int64_t CountVmInstructions(sqlite3::Connection &db, Fn &&fn) {
  int64_t count = 0;
  sqlite3_progress_handler(
      db.Get(), 1,
      [](void *arg) {
        ++*static_cast<int64_t *>(arg);
        return 0;
      },
      &count);
  fn();
  sqlite3_progress_handler(db.Get(), 0, nullptr, nullptr);
  return count;
}

MATCHER_P(IsLookup, kind, "") { return arg.kind == kind; }
MATCHER_P(IsFoundAs, id, "") {
  return arg.kind == LookupResult::Kind::kFound && arg.id == id;
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

  // A whole sync point's worth of clearing, with nothing running between
  // its two halves (as backing::SyncBacking minus the syncfs).
  absl::Status SyncClear(std::span<const InodeId> keep = {}) {
    ABSL_ASSIGN_OR_RETURN(SyncSnapshot synced, BeginSync(ctx_));
    return ClearDirty(ctx_, synced, keep);
  }

  // Links names "e0" .. "e<count-1>" to `child` in `dir` with one SQL
  // statement (thousands of LinkDentry calls exhaust the 256 MB ASan guest
  // through allocator quarantine).
  absl::Status FillDirectory(InodeId dir, int count, InodeId child) {
    return db_.ExecScript(absl::StrCat(
        "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n "
        "WHERE i < ", count - 1, ") "
        "INSERT INTO dentries (parent, name, state, inode) "
        "SELECT ", dir, ", CAST('e' || i AS BLOB), 'present', ", child,
        " FROM n"));
  }

  sqlite3::Connection db_;
  MountFds mounts_;
  // Fixed seed: generations are random, but tests should be reproducible.
  absl::BitGen bitgen_{std::seed_seq{4, 10}};
  Context ctx_{db_, mounts_, bitgen_};
};

TEST_F(MetadataCacheTest, UpsertCreatesThenUpdatesSameIdentity) {
  ASSERT_OK_AND_ASSIGN(UpsertResult first, Make(10));
  EXPECT_TRUE(first.created);
  EXPECT_NE(first.id, kRootInode);
  EXPECT_NE(first.fuse_gen, 0u);

  // Same identity and handle, new attributes: same row, updated in place.
  ASSERT_THAT(MarkAttrsUnknown(ctx_, first.id), IsOk());
  ASSERT_OK_AND_ASSIGN(
      UpsertResult second,
      UpsertInode(ctx_, Handle(kSource, "h10"), Stx(10, S_IFREG, 5), 0));
  EXPECT_FALSE(second.created);
  EXPECT_EQ(second.id, first.id);
  EXPECT_EQ(second.fuse_gen, first.fuse_gen);

  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, first.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_uid, 1005u);
  EXPECT_THAT(GetGeneration(ctx_, first.id), IsOkAndHolds(first.fuse_gen));
}

// Audit F5/F6: (device, ino, generation) alone does not identify an object.
// The generation is 0 for symlinks and special files (and everything on a
// filesystem without FS_IOC_GETVERSION), and btrfs can reissue an identical
// handle after its own power loss; the stored handle bytes and the birth
// time tell those apart.
TEST_F(MetadataCacheTest, DifferentHandleOrBirthTimeIsANewObject) {
  ASSERT_OK_AND_ASSIGN(InodeId dir, MakeDir(kRootInode, "dir", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult old, Make(30, S_IFLNK | 0777));
  ASSERT_THAT(LinkDentry(ctx_, dir, "s", old.id), IsOk());
  ASSERT_THAT(SetSymlink(ctx_, old.id, "old-target"), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, dir, true), IsOk());

  // Same (device, ino, gen 0), different handle bytes (the backing
  // filesystem encodes its own generation there).
  ASSERT_OK_AND_ASSIGN(
      UpsertResult by_handle,
      UpsertInode(ctx_, Handle(kSource, "h30-reborn"), Stx(30, S_IFLNK | 0777),
                  0));
  EXPECT_TRUE(by_handle.created);
  EXPECT_NE(by_handle.id, old.id);
  EXPECT_NE(by_handle.fuse_gen, old.fuse_gen);
  EXPECT_THAT(GetAttr(ctx_, old.id), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Readlink(ctx_, by_handle.id),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Lookup(ctx_, dir, "s"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, dir), IsOkAndHolds(false));
  EXPECT_THAT(GetHandle(ctx_, by_handle.id),
              IsOkAndHolds(Handle(kSource, "h30-reborn")));

  // Same (device, ino, gen) and identical handle bytes, different birth
  // time.
  struct statx reborn = Stx(30, S_IFLNK | 0777);
  reborn.stx_btime.tv_sec += 1000;
  ASSERT_OK_AND_ASSIGN(
      UpsertResult by_btime,
      UpsertInode(ctx_, Handle(kSource, "h30-reborn"), reborn, 0));
  EXPECT_TRUE(by_btime.created);
  EXPECT_NE(by_btime.id, by_handle.id);
  EXPECT_THAT(GetAttr(ctx_, by_handle.id),
              StatusIs(absl::StatusCode::kNotFound));

  // A birth time unknown on either side (0: the filesystem reports none)
  // matches anything.
  struct statx no_btime = reborn;
  no_btime.stx_mask &= ~STATX_BTIME;
  no_btime.stx_btime = {};
  ASSERT_OK_AND_ASSIGN(
      UpsertResult same,
      UpsertInode(ctx_, Handle(kSource, "h30-reborn"), no_btime, 0));
  EXPECT_FALSE(same.created);
  EXPECT_EQ(same.id, by_btime.id);
  ASSERT_OK_AND_ASSIGN(
      UpsertResult again,
      UpsertInode(ctx_, Handle(kSource, "h30-reborn"), reborn, 0));
  EXPECT_FALSE(again.created);
  EXPECT_EQ(again.id, by_btime.id);
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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "x"), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));

  ASSERT_OK_AND_ASSIGN(UpsertResult file, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "x", file.id), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"), IsOkAndHolds(IsFoundAs(file.id)));

  // And back: a negative entry replaces a positive one.
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "x"), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, "x"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "never"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));

  // Names are byte strings, including embedded NULs.
  std::string odd("a\0b\xff", 4);
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, odd, file.id), IsOk());
  EXPECT_THAT(Lookup(ctx_, kRootInode, odd), IsOkAndHolds(IsFoundAs(file.id)));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "a"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));

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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, b, "moved"), IsOkAndHolds(IsFoundAs(f.id)));

  // Replacing an existing target.
  ASSERT_THAT(RenameDentry(ctx_, b, "moved", b, "g"), IsOk());
  EXPECT_THAT(Lookup(ctx_, b, "g"), IsOkAndHolds(IsFoundAs(f.id)));
  EXPECT_THAT(Lookup(ctx_, b, "moved"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  // The replaced inode's row survives (it may have other links).
  EXPECT_THAT(GetAttr(ctx_, g.id), IsOk());

  // Renaming onto itself is a no-op.
  ASSERT_THAT(RenameDentry(ctx_, b, "g", b, "g"), IsOk());
  EXPECT_THAT(Lookup(ctx_, b, "g"), IsOkAndHolds(IsFoundAs(f.id)));

  // A directory's parent follows its dentry.
  ASSERT_THAT(RenameDentry(ctx_, kRootInode, "b", a, "b"), IsOk());
  EXPECT_THAT(ParentOf(ctx_, b), IsOkAndHolds(Optional(a)));

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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, b, "l2"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, a), IsOkAndHolds(false));
  EXPECT_THAT(IsDirComplete(ctx_, b), IsOkAndHolds(false));
  EXPECT_THAT(CountRows(db_, absl::StrCat("xattrs WHERE inode = ", link.id)),
              IsOkAndHolds(0));

  // A directory takes its directories row, xattrs and child dentries with
  // it, but not the child's inode row.
  ASSERT_THAT(DeleteInode(ctx_, d), IsOk());
  EXPECT_THAT(Lookup(ctx_, a, "d"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(CountRows(db_, absl::StrCat("directories WHERE inode = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, absl::StrCat("dentries WHERE parent = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, absl::StrCat("xattrs WHERE inode = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(GetAttr(ctx_, child.id), IsOk());
  // Nothing was ever turned into a negative entry.
  EXPECT_THAT(CountRows(db_, "dentries WHERE state = 'absent'"),
              IsOkAndHolds(0));

  EXPECT_THAT(DeleteInode(ctx_, d), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(InvalidateInode(ctx_, kRootInode),
              StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(GetAttr(ctx_, kRootInode), IsOk());
}

TEST_F(MetadataCacheTest, ForgetNegativeDentriesKeepsPositiveOnes) {
  ASSERT_OK_AND_ASSIGN(InodeId dir, MakeDir(kRootInode, "dir", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult file, Make(30));
  ASSERT_THAT(LinkDentry(ctx_, dir, "f", file.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, dir, "ghost"), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, dir, true), IsOk());

  ASSERT_THAT(ForgetNegativeDentries(ctx_, dir), IsOk());
  EXPECT_THAT(Lookup(ctx_, dir, "ghost"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, dir, "f"), IsOkAndHolds(IsFoundAs(file.id)));
  EXPECT_THAT(IsDirComplete(ctx_, dir), IsOkAndHolds(false));
  EXPECT_THAT(ForgetNegativeDentries(ctx_, 999),
              StatusIs(absl::StatusCode::kNotFound));
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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
  for (InodeId id : {mnt, f2.id, sub, f3.id}) {
    EXPECT_THAT(GetAttr(ctx_, id), StatusIs(absl::StatusCode::kNotFound))
        << id;
  }
  EXPECT_THAT(GetFilesystem(ctx_, kSecond),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(GetFilesystem(ctx_, kThird),
              StatusIs(absl::StatusCode::kNotFound));
  // Only the root's rows remain, plus the now-unknown "mnt".
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(2));
  EXPECT_THAT(CountRows(db_, "dentries"), IsOkAndHolds(2));
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
                IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)))
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

// Phase 6.2: a readdir reply is built from ListDir pages, so one page's cost
// must depend on the page, not on the directory's size: with no index
// serving "parent = ? AND rowid > ? ORDER BY rowid", every page scanned and
// sorted the whole directory, and listing n entries took O(n^2) (a cached
// 10000-entry listing took 3.5 s, 27x the backing filesystem's).
TEST_F(MetadataCacheTest, ListDirPageCostDoesNotGrowWithTheDirectory) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(InodeId small, MakeDir(kRootInode, "small", 31));
  ASSERT_OK_AND_ASSIGN(InodeId big, MakeDir(kRootInode, "big", 32));
  constexpr int kSmall = 100;
  constexpr int kBig = 5000;
  ASSERT_THAT(FillDirectory(small, kSmall, f.id), IsOk());
  ASSERT_THAT(FillDirectory(big, kBig, f.id), IsOk());

  // One page (ListDir reads 64 rows per query), from the start and from
  // near the end, stopping after the first entry delivered.
  auto first_page_cost = [&](InodeId dir, int64_t cursor) {
    return CountVmInstructions(db_, [&] {
      EXPECT_THAT(ListDir(ctx_, dir, cursor,
                          [](std::string_view, InodeId,
                             int64_t) -> absl::StatusOr<bool> { return false; }),
                  IsOk());
    });
  };
  int64_t small_cost = first_page_cost(small, 0);
  ASSERT_GT(small_cost, 0);
  EXPECT_LE(first_page_cost(big, 0), 2 * small_cost);

  int64_t last_cursor = 0;
  ASSERT_THAT(ListDir(ctx_, big, 0,
                      [&](std::string_view, InodeId,
                          int64_t next) -> absl::StatusOr<bool> {
                        last_cursor = next;
                        return true;
                      }),
              IsOk());
  ASSERT_GT(last_cursor, kBig);  // rowids: the directory rows came first
  // A page from the middle of the big directory (cursor of its 2500th
  // entry) is still one page's work.
  int64_t middle = 0;
  int n = 0;
  ASSERT_THAT(ListDir(ctx_, big, 0,
                      [&](std::string_view, InodeId,
                          int64_t next) -> absl::StatusOr<bool> {
                        middle = next;
                        return ++n < kBig / 2;
                      }),
              IsOk());
  EXPECT_LE(first_page_cost(big, middle), 2 * small_cost);
}

// The completeness check every readdir request starts with ("no name of this
// directory is unknown") likewise looked at every entry of the directory.
TEST_F(MetadataCacheTest, IsDirCompleteCostDoesNotGrowWithTheDirectory) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(InodeId small, MakeDir(kRootInode, "small", 31));
  ASSERT_OK_AND_ASSIGN(InodeId big, MakeDir(kRootInode, "big", 32));
  constexpr int kSmall = 100;
  constexpr int kBig = 5000;
  ASSERT_THAT(FillDirectory(small, kSmall, f.id), IsOk());
  ASSERT_THAT(FillDirectory(big, kBig, f.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, small, true), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, big, true), IsOk());

  auto cost = [&](InodeId dir) {
    return CountVmInstructions(db_, [&] {
      EXPECT_THAT(IsDirComplete(ctx_, dir), IsOkAndHolds(true));
    });
  };
  int64_t small_cost = cost(small);
  ASSERT_GT(small_cost, 0);
  EXPECT_LE(cost(big), 2 * small_cost);

  // And it still notices an unknown name.
  ASSERT_THAT(MarkUnknown(ctx_, big, std::vector<std::string>{"e4000"}),
              IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, big), IsOkAndHolds(false));
  ASSERT_THAT(LinkDentry(ctx_, big, "e4000", f.id), IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, big), IsOkAndHolds(true));
}

// A listing shows present names and stubs (refused names, step 23.5), in
// first-linked order, and a name that goes unknown and comes back keeps
// its position (its offset), so a cursor taken before the change still
// resumes in the right place.
TEST_F(MetadataCacheTest, ListDirOrderSurvivesStateChanges) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  for (std::string_view name : {"a", "b", "c", "d", "e"}) {
    ASSERT_THAT(LinkDentry(ctx_, kRootInode, name, f.id), IsOk());
  }
  int64_t after_b = 0;
  int n = 0;
  ASSERT_THAT(ListDir(ctx_, kRootInode, 0,
                      [&](std::string_view, InodeId,
                          int64_t next) -> absl::StatusOr<bool> {
                        after_b = next;
                        return ++n < 2;
                      }),
              IsOk());
  ASSERT_THAT(MarkUnknown(ctx_, kRootInode, std::vector<std::string>{"c"}),
              IsOk());
  ASSERT_THAT(SetNegative(ctx_, kRootInode, "d"), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId stub,
                       SetRefused(ctx_, kRootInode, "e",
                                  Stx(31, S_IFDIR | 0755)));
  EXPECT_TRUE(IsStub(stub));
  EXPECT_THAT(ListNames(ctx_, kRootInode), ElementsAre("a", "b", "e"));
  std::vector<std::string> rest;
  std::vector<InodeId> rest_ids;
  ASSERT_THAT(ListDir(ctx_, kRootInode, after_b,
                      [&](std::string_view name, InodeId child,
                          int64_t) -> absl::StatusOr<bool> {
                        rest.emplace_back(name);
                        rest_ids.push_back(child);
                        return true;
                      }),
              IsOk());
  EXPECT_THAT(rest, ElementsAre("e"));
  EXPECT_THAT(rest_ids, ElementsAre(stub));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "e", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "c", f.id), IsOk());
  EXPECT_THAT(ListNames(ctx_, kRootInode), ElementsAre("a", "b", "c", "e"));
  rest.clear();
  ASSERT_THAT(ListDir(ctx_, kRootInode, after_b,
                      [&](std::string_view name, InodeId,
                          int64_t) -> absl::StatusOr<bool> {
                        rest.emplace_back(name);
                        return true;
                      }),
              IsOk());
  EXPECT_THAT(rest, ElementsAre("c", "e"));
}

// Step 23.5: a refused dentry's stub. SetRefused records it with the
// refusal (nodeids from 2^63 up, a random nonzero generation, the boundary
// root's attributes), keeps it while the name stays refused (refreshing the
// attributes), and drops it with the refusal, however that goes.
TEST_F(MetadataCacheTest, StubsLiveWithTheirRefusals) {
  ASSERT_OK_AND_ASSIGN(InodeId dir, MakeDir(kRootInode, "dir", 40));
  ASSERT_OK_AND_ASSIGN(InodeId mp, SetRefused(ctx_, dir, "mp",
                                              Stx(2, S_IFDIR | 0751, 1)));
  ASSERT_OK_AND_ASSIGN(InodeId sv, SetRefused(ctx_, dir, "sv",
                                              Stx(256, S_IFDIR | 0700, 2)));
  EXPECT_EQ(mp, kFirstStubId);
  EXPECT_EQ(sv, kFirstStubId + 1);
  EXPECT_EQ(static_cast<uint64_t>(mp), kFirstStubNodeid);

  ASSERT_OK_AND_ASSIGN(StubRow row, GetStub(ctx_, mp));
  EXPECT_EQ(row.parent, dir);
  EXPECT_EQ(row.name, "mp");
  EXPECT_TRUE(row.attr.valid);
  EXPECT_NE(row.attr.fuse_gen, 0u);
  EXPECT_EQ(row.attr.st.st_ino, kFirstStubNodeid);
  EXPECT_EQ(row.attr.st.st_mode, S_IFDIR | 0751u);
  EXPECT_EQ(row.attr.st.st_uid, 1001u);
  EXPECT_THAT(Lookup(ctx_, dir, "mp"),
              IsOkAndHolds(::testing::AllOf(
                  IsLookup(LookupResult::Kind::kRefused),
                  ::testing::Field(&LookupResult::id, mp))));

  // Refused again: the same stub, attributes refreshed.
  ASSERT_THAT(SetRefused(ctx_, dir, "mp", Stx(2, S_IFDIR | 0755, 3)),
              IsOkAndHolds(mp));
  ASSERT_OK_AND_ASSIGN(StubRow again, GetStub(ctx_, mp));
  EXPECT_EQ(again.attr.fuse_gen, row.attr.fuse_gen);
  EXPECT_EQ(again.attr.st.st_mode, S_IFDIR | 0755u);
  EXPECT_EQ(again.attr.st.st_uid, 1003u);

  // Merely forgotten (a mutation's phase 1, ForgetNegativeDentries before
  // a relisting): the stub stays, the name reads unknown, and refusing the
  // same name again keeps its nodeid and generation (the kernel's
  // revalidation must not see a new nodeid: it would detach a mount on it).
  ASSERT_THAT(MarkUnknown(ctx_, dir, std::vector<std::string>{"mp"}), IsOk());
  EXPECT_THAT(GetStub(ctx_, mp), IsOk());
  ASSERT_THAT(ForgetNegativeDentries(ctx_, dir), IsOk());
  EXPECT_THAT(GetStub(ctx_, sv), IsOk());
  EXPECT_THAT(Lookup(ctx_, dir, "sv"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  ASSERT_THAT(SetRefused(ctx_, dir, "mp", Stx(2, S_IFDIR | 0751)),
              IsOkAndHolds(mp));
  ASSERT_OK_AND_ASSIGN(StubRow kept, GetStub(ctx_, mp));
  EXPECT_EQ(kept.attr.fuse_gen, row.attr.fuse_gen);

  // Relisted as something else, or gone: no stub.
  ASSERT_THAT(SetNegative(ctx_, dir, "mp"), IsOk());
  EXPECT_THAT(GetStub(ctx_, mp), StatusIs(absl::StatusCode::kNotFound));
  ASSERT_THAT(PruneDentriesNotIn(ctx_, dir, {}), IsOk());
  EXPECT_THAT(GetStub(ctx_, sv), StatusIs(absl::StatusCode::kNotFound));

  // A stub's nodeid is never handed out again (formal/lifetime.tla's
  // NodeidStable): the next one is past every stub's so far, whichever
  // boundary it is for.
  ASSERT_OK_AND_ASSIGN(InodeId again_id, SetRefused(ctx_, dir, "mp",
                                                    Stx(2, S_IFDIR | 0751)));
  EXPECT_EQ(again_id, kFirstStubId + 2);
  // And it goes with its directory.
  ASSERT_THAT(InvalidateInode(ctx_, dir), IsOk());
  EXPECT_THAT(GetStub(ctx_, again_id), StatusIs(absl::StatusCode::kNotFound));
}

// Recovery forgets a dirty directory's dentries, stubs included.
TEST_F(MetadataCacheTest, RecoveryForgetsStubs) {
  ASSERT_OK_AND_ASSIGN(InodeId dir, MakeDir(kRootInode, "dir", 40));
  ASSERT_OK_AND_ASSIGN(InodeId mp, SetRefused(ctx_, dir, "mp",
                                              Stx(2, S_IFDIR | 0751)));
  const InodeId ids[] = {dir};
  ASSERT_THAT(MarkDirty(ctx_, ids), IsOk());
  ASSERT_THAT(RecoverDirty(ctx_), IsOkAndHolds(1));
  EXPECT_THAT(GetStub(ctx_, mp), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Lookup(ctx_, dir, "mp"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
}

// Step 23.3: the access time a read open records (cache::TouchAtime).
TEST_F(MetadataCacheTest, TouchAtimeFollowsTheMountsRule) {
  // Stx's times: atime ...001.111, mtime ...002.222, ctime ...003.333.
  struct statx stx = Stx(50, S_IFREG | 0644);
  ASSERT_OK_AND_ASSIGN(UpsertResult r, UpsertInode(ctx_, Handle(kSource, "h50"),
                                                   stx, 0));
  auto atime = [&]() -> int64_t {
    absl::StatusOr<CachedAttr> attr = GetAttr(ctx_, r.id);
    EXPECT_THAT(attr, IsOk());
    return attr.ok() ? attr->st.st_atim.tv_sec : -1;
  };
  const struct timespec soon = {.tv_sec = 1'700'000'010, .tv_nsec = 5};
  // Not after mtime: updated.
  EXPECT_THAT(TouchAtime(ctx_, r.id, soon), IsOkAndHolds(true));
  EXPECT_EQ(atime(), soon.tv_sec);
  // After mtime and ctime, within the day: kept.
  const struct timespec later = {.tv_sec = soon.tv_sec + 3600};
  EXPECT_THAT(TouchAtime(ctx_, r.id, later), IsOkAndHolds(false));
  EXPECT_EQ(atime(), soon.tv_sec);
  // Review L7: deciding to keep it takes no write transaction (every read
  // OPEN asks): it works on a read-only connection.
  ASSERT_THAT(db_.Exec("PRAGMA query_only = 1"), IsOk());
  EXPECT_THAT(TouchAtime(ctx_, r.id, later), IsOkAndHolds(false));
  ASSERT_THAT(db_.Exec("PRAGMA query_only = 0"), IsOk());
  // A day old: updated.
  const struct timespec next_day = {.tv_sec = soon.tv_sec + 86400};
  EXPECT_THAT(TouchAtime(ctx_, r.id, next_day), IsOkAndHolds(true));
  EXPECT_EQ(atime(), next_day.tv_sec);
  // strictatime: always; noatime: never.
  ctx_.atime = AtimePolicy::kStrict;
  const struct timespec a_second = {.tv_sec = next_day.tv_sec + 1};
  EXPECT_THAT(TouchAtime(ctx_, r.id, a_second), IsOkAndHolds(true));
  EXPECT_EQ(atime(), a_second.tv_sec);
  ctx_.atime = AtimePolicy::kNever;
  EXPECT_THAT(TouchAtime(ctx_, r.id, {.tv_sec = a_second.tv_sec + 99999}),
              IsOkAndHolds(false));
  EXPECT_EQ(atime(), a_second.tv_sec);
  ctx_.atime = AtimePolicy::kRelative;
  // Unknown attributes are left alone (they are re-read, atime included).
  ASSERT_THAT(MarkAttrsUnknown(ctx_, r.id), IsOk());
  EXPECT_THAT(TouchAtime(ctx_, r.id, {.tv_sec = a_second.tv_sec + 99999}),
              IsOkAndHolds(false));
  // No row.
  EXPECT_THAT(TouchAtime(ctx_, 9999, soon),
              StatusIs(absl::StatusCode::kNotFound));
  ctx_.atime = AtimePolicy::kNever;
  EXPECT_THAT(TouchAtime(ctx_, 9999, soon),
              StatusIs(absl::StatusCode::kNotFound));
}

// Review L5: the startup sweep after a crash deletes non-directory rows
// with no link and no name, and nothing else.
TEST_F(MetadataCacheTest, ForgetUnnamedRowsKeepsNamedLinkedAndDirectories) {
  auto make = [&](uint64_t ino, mode_t mode, int64_t nlink) {
    return UpsertInode(ctx_, Handle(kSource, absl::StrCat("u", ino)),
                       Stx(ino, mode, nlink - 3), 0);
  };
  ASSERT_OK_AND_ASSIGN(UpsertResult unnamed, make(60, S_IFREG | 0644, 0));
  ASSERT_OK_AND_ASSIGN(UpsertResult named0, make(61, S_IFREG | 0644, 0));
  ASSERT_OK_AND_ASSIGN(UpsertResult linked, make(62, S_IFREG | 0644, 1));
  ASSERT_OK_AND_ASSIGN(UpsertResult dir0, make(63, S_IFDIR | 0755, 0));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "named0", named0.id), IsOk());
  ASSERT_THAT(ForgetUnnamedRows(ctx_), IsOkAndHolds(1));
  EXPECT_THAT(GetAttr(ctx_, unnamed.id), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(GetAttr(ctx_, named0.id), IsOk());
  EXPECT_THAT(GetAttr(ctx_, linked.id), IsOk());
  EXPECT_THAT(GetAttr(ctx_, dir0.id), IsOk());
  EXPECT_THAT(GetAttr(ctx_, kRootInode), IsOk());
}

// The sweep runs at every start (step 12.4b), so it must not read the
// whole inodes table: its DELETE, as SQLite plans it, searches the partial
// index inodes_unlinked (WHERE nlink = 0; a partial index is used only for
// a query with its predicate literally). The statement is caught as
// ForgetUnnamedRows runs it (sqlite3_trace_v2), so the plan is of the SQL
// the code has.
TEST_F(MetadataCacheTest, ForgetUnnamedRowsSearchesTheUnlinkedIndex) {
  std::vector<std::string> statements;
  ASSERT_EQ(sqlite3_trace_v2(
                db_.Get(), SQLITE_TRACE_STMT,
                [](unsigned, void *out, void *stmt, void *) -> int {
                  static_cast<std::vector<std::string> *>(out)->push_back(
                      sqlite3_sql(static_cast<sqlite3_stmt *>(stmt)));
                  return 0;
                },
                &statements),
            SQLITE_OK);
  ASSERT_THAT(ForgetUnnamedRows(ctx_), IsOk());
  ASSERT_EQ(sqlite3_trace_v2(db_.Get(), 0, nullptr, nullptr), SQLITE_OK);
  std::string sweep;
  for (const std::string &sql : statements) {
    if (sql.starts_with("DELETE FROM inodes")) sweep = sql;
  }
  ASSERT_FALSE(sweep.empty());
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * plan,
      db_.Prepared(absl::StrCat("EXPLAIN QUERY PLAN ", sweep)));
  std::vector<std::string> details;
  ASSERT_THAT(plan->ForEachRow([&](sqlite3::Statement &row) {
    details.push_back(row.Column<std::string>(3));
    return absl::OkStatus();
  }),
              IsOk());
  EXPECT_THAT(details, ::testing::Contains(::testing::HasSubstr(
                           "USING INDEX inodes_unlinked")))
      << ::testing::PrintToString(details);
}

// The FORGET reconciliation's batch phase 1 marks every inode it names
// that still has a row; one whose row went meanwhile (invalidated) costs
// the others nothing.
TEST_F(MetadataCacheTest, BeginAttrChangesSkipsAVanishedRow) {
  ASSERT_OK_AND_ASSIGN(UpsertResult a, Make(70));
  ASSERT_OK_AND_ASSIGN(UpsertResult b, Make(71));
  const InodeId vanished = 7777;  // No such row.
  ASSERT_THAT(GetAttr(ctx_, vanished), StatusIs(absl::StatusCode::kNotFound));
  const InodeId ids[] = {a.id, vanished, b.id};
  absl::StatusOr<Mutation> mutation = BeginAttrChanges(ctx_, ids);
  ASSERT_THAT(mutation.status(), IsOk());
  mutation->End();
  for (InodeId id : {a.id, b.id}) {
    ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, id));
    EXPECT_FALSE(attr.valid) << id;
  }
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsSupersetOf({a.id, b.id})));
}

TEST_F(MetadataCacheTest, FuseGenerations) {
  EXPECT_THAT(GetGeneration(ctx_, kRootInode), IsOkAndHolds(0u));
  // Random, so 100 draws of 32 bits collide with probability ~1e-6.
  absl::flat_hash_set<uint32_t> gens;
  for (uint64_t ino = 100; ino < 200; ++ino) {
    ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(ino));
    EXPECT_NE(r.fuse_gen, 0u);
    EXPECT_THAT(GetGeneration(ctx_, r.id), IsOkAndHolds(r.fuse_gen));
    EXPECT_TRUE(gens.insert(r.fuse_gen).second)
        << "generation " << r.fuse_gen << " drawn twice";
  }
  EXPECT_THAT(GetGeneration(ctx_, 999), StatusIs(absl::StatusCode::kNotFound));
}

// The point of random generations: a power loss can roll back recent
// inserts and let AUTOINCREMENT hand the same id out again, but the reused
// id gets a fresh generation, so an old (id, generation) handle is stale
// instead of resolving to the new object.
TEST_F(MetadataCacheTest, ReusedIdAfterRollbackGetsNewGeneration) {
  UpsertResult before;
  absl::Status rolled_back = db_.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(before, Make(100));
    return absl::AbortedError("simulated loss of the tail of the WAL");
  });
  ASSERT_THAT(rolled_back, StatusIs(absl::StatusCode::kAborted));
  ASSERT_OK_AND_ASSIGN(UpsertResult after, Make(101));
  EXPECT_EQ(after.id, before.id);
  EXPECT_NE(after.fuse_gen, before.fuse_gen);
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
  EXPECT_EQ(attr.btime.tv_sec, -5 - 77);
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
  EXPECT_THAT(ParentOf(ctx_, kRootInode), IsOkAndHolds(Optional(kRootInode)));
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(a, "b", 21));
  EXPECT_THAT(ParentOf(ctx_, a), IsOkAndHolds(Optional(kRootInode)));
  EXPECT_THAT(ParentOf(ctx_, b), IsOkAndHolds(Optional(a)));
  ASSERT_THAT(UnlinkDentry(ctx_, a, "b"), IsOk());
  EXPECT_THAT(ParentOf(ctx_, b), IsOkAndHolds(std::nullopt));
}

TEST_F(MetadataCacheTest, XattrGetSetIndividualValue) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(70));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"), IsOkAndHolds(std::nullopt));

  // An individually cached value is served even while the set is incomplete.
  ASSERT_THAT(SetXattr(ctx_, r.id, "user.a", "1"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              IsOkAndHolds(Optional(std::string("1"))));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
}

TEST_F(MetadataCacheTest, XattrReplaceMakesSetComplete) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(70));
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
}

TEST_F(MetadataCacheTest, XattrSetAndRemoveKeepSetComplete) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(70));
  std::string binary("\x00\xff", 2);
  std::vector<std::pair<std::string, std::string>> all = {
      {"user.b", "2"}, {"user.c", binary}};
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, all), IsOk());

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
}

TEST_F(MetadataCacheTest, XattrMarkUnknownClearsCache) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(70));
  std::vector<std::pair<std::string, std::string>> all = {{"user.b", "2"}};
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, all), IsOk());

  ASSERT_THAT(MarkXattrsUnknown(ctx_, r.id), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.b"), IsOkAndHolds(std::nullopt));
}

TEST_F(MetadataCacheTest, XattrEmptySetIsComplete) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(70));
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, {}), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(::testing::IsEmpty())));
}

TEST_F(MetadataCacheTest, XattrOpsOnMissingInode) {
  EXPECT_THAT(ListXattrs(ctx_, 999), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(SetXattr(ctx_, 999, "a", "b"),
              StatusIs(absl::StatusCode::kNotFound));
}

// Each xattr name has its own present / absent / unknown state, and the
// set's completeness only means "a name with no row is absent": phase 1 of
// a Setxattr/Removexattr makes just that one name unknown, and phase 3
// makes it present or absent, without ever clearing completeness.
TEST_F(MetadataCacheTest, XattrStatesArePerName) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(71));
  const std::vector<std::pair<std::string, std::string>> all = {
      {"user.a", "1"}, {"user.b", "2"}};
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, all), IsOk());

  // Phase 1 of setxattr(user.new), a name with no row in a complete set.
  // A reader that runs now (while phase 2 is under way) must see user.new
  // as unknown, not absent, and must not get a listing that omits it; but
  // every other name keeps its known state, absent ones included.
  ASSERT_THAT(BeginXattrChange(ctx_, r.id, "user.new"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.new"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              IsOkAndHolds(Optional(std::string("1"))));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.never"),
              StatusIs(absl::StatusCode::kNotFound));
  // Phase 3: present. The set is complete again with no refresh.
  ASSERT_THAT(SetXattr(ctx_, r.id, "user.new", "3"), IsOk());
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(ElementsAre("user.a", "user.b",
                                                "user.new"))));

  // Phase 1 of removexattr(user.a), a present name: unknown, then absent.
  ASSERT_THAT(BeginXattrChange(ctx_, r.id, "user.a"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  ASSERT_THAT(RemoveXattr(ctx_, r.id, "user.a"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(ElementsAre("user.b", "user.new"))));

  // ForgetXattr alone (no dirty-set bookkeeping) is the same phase-1 state.
  ASSERT_THAT(ForgetXattr(ctx_, r.id, "user.b"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.b"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.never"),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
}

// With the set incomplete, an explicit absent row is still known absent (a
// removexattr's phase 3 needs no refresh to answer a later getxattr), and a
// name with no row is unknown.
TEST_F(MetadataCacheTest, XattrAbsentIsKnownWhileTheSetIsIncomplete) {
  ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(72));
  ASSERT_THAT(BeginXattrChange(ctx_, r.id, "user.a"), IsOk());
  ASSERT_THAT(RemoveXattr(ctx_, r.id, "user.a"), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.other"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(ListXattrs(ctx_, r.id), IsOkAndHolds(std::nullopt));
  // A refresh replaces every row, absent ones included.
  const std::vector<std::pair<std::string, std::string>> all = {
      {"user.a", "back"}};
  ASSERT_THAT(ReplaceXattrs(ctx_, r.id, all), IsOk());
  EXPECT_THAT(GetXattr(ctx_, r.id, "user.a"),
              IsOkAndHolds(Optional(std::string("back"))));
  EXPECT_THAT(ListXattrs(ctx_, r.id),
              IsOkAndHolds(Optional(ElementsAre("user.a"))));
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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));

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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
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
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(Lookup(ctx_, a, "drop"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, a, "neg_drop"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  // Other directories and the inode row are untouched.
  EXPECT_THAT(Lookup(ctx_, b, "drop"), IsOkAndHolds(IsFoundAs(f.id)));
  EXPECT_THAT(GetAttr(ctx_, f.id), IsOk());

  // An empty listing forgets everything.
  ASSERT_THAT(PruneDentriesNotIn(ctx_, a, {}), IsOk());
  EXPECT_THAT(ListNames(ctx_, a), ::testing::IsEmpty());
  EXPECT_THAT(CountRows(db_, absl::StrCat("dentries WHERE parent = ", a)),
              IsOkAndHolds(0));
}

// --- The durable dirty set ----------------------------------------------------

absl::StatusOr<int> Synchronous(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                        db.Prepared("PRAGMA synchronous"));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  RET_CHECK(has_row);
  int level = stmt->Column<int>(0);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return level;
}

constexpr int kSynchronousNormal = 1;
constexpr int kSynchronousFull = 2;

TEST_F(MetadataCacheTest, BeginMutationIsDurableUntilIdsAreKnownDirty) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ctx_.dirty = {};
  ctx_.dirty.any = false;

  const InodeId both[] = {f.id, g.id};
  absl::StatusOr<int> level;
  ASSERT_THAT(BeginMutation(ctx_, both, [&] {
                level = Synchronous(db_);
                return MarkAttrsUnknown(ctx_, f.id);
              }),
              IsOk());
  // Committed with synchronous=FULL (a WAL fsync), in the same transaction
  // as the body.
  EXPECT_THAT(level, IsOkAndHolds(kSynchronousFull));
  EXPECT_THAT(Synchronous(db_), IsOkAndHolds(kSynchronousNormal));
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id, g.id)));
  EXPECT_TRUE(ctx_.dirty.any);
  EXPECT_TRUE(ctx_.dirty.durable.contains(f.id));
  EXPECT_TRUE(ctx_.dirty.durable.contains(g.id));

  // Everything already durably dirty: no fsync needed.
  const InodeId just_g[] = {g.id};
  ASSERT_THAT(BeginMutation(ctx_, just_g, [&] {
                level = Synchronous(db_);
                return absl::OkStatus();
              }),
              IsOk());
  EXPECT_THAT(level, IsOkAndHolds(kSynchronousNormal));

  // One new id is enough to need it again.
  ASSERT_OK_AND_ASSIGN(UpsertResult h, Make(32));
  const InodeId g_and_h[] = {g.id, h.id};
  ASSERT_THAT(BeginMutation(ctx_, g_and_h, [&] {
                level = Synchronous(db_);
                return absl::OkStatus();
              }),
              IsOk());
  EXPECT_THAT(level, IsOkAndHolds(kSynchronousFull));
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id, g.id, h.id)));

  // A failing body records nothing.
  ASSERT_OK_AND_ASSIGN(UpsertResult i, Make(33));
  const InodeId just_i[] = {i.id};
  EXPECT_THAT(BeginMutation(ctx_, just_i,
                            [] { return absl::InternalError("no"); }),
              StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id, g.id, h.id)));
  EXPECT_FALSE(ctx_.dirty.durable.contains(i.id));

  // A durable commit cannot nest inside a caller's transaction.
  EXPECT_THAT(db_.Transaction([&] {
    return BeginMutation(ctx_, just_i, [] { return absl::OkStatus(); })
        .status();
  }),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// Phase 1 of every mutation kind names exactly the inodes it changes, and
// marks their state unknown, durably.
TEST_F(MetadataCacheTest, EveryMutationKindDirtiesWhatItChanges) {
  ASSERT_OK_AND_ASSIGN(InodeId a, MakeDir(kRootInode, "a", 20));
  ASSERT_OK_AND_ASSIGN(InodeId b, MakeDir(kRootInode, "b", 21));
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_THAT(LinkDentry(ctx_, a, "f", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, b, "g", g.id), IsOk());
  ASSERT_THAT(SetXattr(ctx_, f.id, "user.k", "v"), IsOk());
  ASSERT_THAT(SetXattr(ctx_, f.id, "user.other", "w"), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, a, true), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, b, true), IsOk());

  auto reset = [&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(SyncClear());
    for (auto [id, ino] : {std::pair{a, 20}, std::pair{b, 21},
                           std::pair{f.id, 30}, std::pair{g.id, 31}}) {
      ABSL_RETURN_IF_ERROR(UpdateAttr(ctx_, id, Stx(ino, S_IFREG)));
    }
    ABSL_RETURN_IF_ERROR(MarkDirComplete(ctx_, a, true));
    return MarkDirComplete(ctx_, b, true);
  };
  auto valid = [&](InodeId id) {
    absl::StatusOr<CachedAttr> attr = GetAttr(ctx_, id);
    return attr.ok() && attr->valid;
  };

  // Create of "new" in a.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(SetNegative(ctx_, a, "new"), IsOk());
  ASSERT_THAT(BeginCreate(ctx_, a, "new"), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(a)));
  EXPECT_TRUE(ctx_.dirty.durable.contains(a));
  EXPECT_THAT(Lookup(ctx_, a, "new"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, a), IsOkAndHolds(false));

  // Unlink of a/f.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(BeginRemove(ctx_, a, "f", f.id, BeginFill(ctx_)), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(a, f.id)));
  EXPECT_THAT(Lookup(ctx_, a, "f"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_FALSE(valid(a));
  EXPECT_FALSE(valid(f.id));
  EXPECT_TRUE(valid(b));
  ASSERT_THAT(LinkDentry(ctx_, a, "f", f.id), IsOk());

  // Rename of a/f over b/g.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(
      BeginRename(ctx_, a, "f", b, "g", f.id, g.id, BeginFill(ctx_)),
      IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(a, b, f.id, g.id)));
  EXPECT_THAT(Lookup(ctx_, a, "f"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, b, "g"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  for (InodeId id : {a, b, f.id, g.id}) EXPECT_FALSE(valid(id)) << id;
  // And without a destination.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(BeginRename(ctx_, a, "x", a, "y", f.id, std::nullopt,
                          BeginFill(ctx_)), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(a, f.id)));
  ASSERT_THAT(LinkDentry(ctx_, a, "f", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, b, "g", g.id), IsOk());

  // Link of f as b/h.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(BeginLink(ctx_, f.id, b, "h"), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(b, f.id)));
  EXPECT_FALSE(valid(b));
  EXPECT_FALSE(valid(f.id));
  EXPECT_TRUE(valid(a));
  EXPECT_THAT(IsDirComplete(ctx_, b), IsOkAndHolds(false));
  ASSERT_THAT(SetNegative(ctx_, b, "h"), IsOk());

  // Setattr / writable open / write / fallocate of g.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(BeginAttrChange(ctx_, g.id), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(g.id)));
  EXPECT_FALSE(valid(g.id));
  EXPECT_THAT(IsDirComplete(ctx_, b), IsOkAndHolds(true));
  // With side-effect xattrs: each becomes unknown, even one with no row in
  // a complete set; the others keep their state.
  ASSERT_THAT(reset(), IsOk());
  const std::vector<std::pair<std::string, std::string>> g_xattrs = {
      {"user.keep", "1"}, {"security.capability", "cap"}};
  ASSERT_THAT(ReplaceXattrs(ctx_, g.id, g_xattrs), IsOk());
  const std::string_view side_effects[] = {"security.capability",
                                           "system.posix_acl_access"};
  ASSERT_THAT(BeginAttrChange(ctx_, g.id, side_effects), IsOk());
  EXPECT_FALSE(valid(g.id));
  EXPECT_THAT(GetXattr(ctx_, g.id, "security.capability"),
              IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, g.id, "system.posix_acl_access"),
              IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, g.id, "user.keep"),
              IsOkAndHolds(Optional(std::string("1"))));
  EXPECT_THAT(GetXattr(ctx_, g.id, "user.never"),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(ListXattrs(ctx_, g.id), IsOkAndHolds(std::nullopt));

  // Setxattr / removexattr of user.k on f.
  ASSERT_THAT(reset(), IsOk());
  ASSERT_THAT(BeginXattrChange(ctx_, f.id, "user.k"), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id)));
  EXPECT_FALSE(valid(f.id));
  EXPECT_THAT(GetXattr(ctx_, f.id, "user.k"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, f.id, "user.other"),
              IsOkAndHolds(Optional(std::string("w"))));
}

// formal/ finding rename_stale_source: a rename's phase 1 verifies that
// nothing it names changed since its caller resolved the source and the
// destination; if something did, it writes nothing and begins nothing.
TEST_F(MetadataCacheTest, BeginRenameRefusesAStaleResolution) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult x, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult y, Make(31));
  ASSERT_OK_AND_ASSIGN(UpsertResult z, Make(32));
  ASSERT_THAT(LinkDentry(ctx_, d, "x", x.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, d, "y", y.id), IsOk());
  ASSERT_THAT(SyncClear(), IsOk());
  auto unchanged = [&] {
    for (const char *name : {"x", "y"}) {
      EXPECT_THAT(Lookup(ctx_, d, name),
                  IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
    }
    EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
    EXPECT_FALSE(ctx_.fills.inflight.contains(d));
  };

  // Each inode the rename names, changed by a mutation that began and
  // ended after the snapshot: d (the parent), x (the source), y (the
  // destination).
  for (InodeId changed : {d, x.id, y.id}) {
    SCOPED_TRACE(changed);
    const FillSnapshot resolved = BeginFill(ctx_);
    ASSERT_THAT(BeginAttrChange(ctx_, changed), IsOk());  // Ends at once.
    ASSERT_THAT(SyncClear(), IsOk());
    EXPECT_THAT(BeginRename(ctx_, d, "x", d, "y", x.id, y.id, resolved),
                StatusIs(absl::StatusCode::kAborted));
    unchanged();
  }
  // A mutation of the parent that was already in flight at the snapshot
  // (and still is): what the resolve saw may be about to change.
  {
    ASSERT_OK_AND_ASSIGN(Mutation other, BeginCreate(ctx_, d, "w"));
    const FillSnapshot resolved = BeginFill(ctx_);
    EXPECT_THAT(BeginRename(ctx_, d, "x", d, "y", x.id, y.id, resolved),
                StatusIs(absl::StatusCode::kAborted));
    other.End();
    ASSERT_THAT(SyncClear(), IsOk());
  }
  // An unrelated inode's mutation does not matter.
  const FillSnapshot resolved = BeginFill(ctx_);
  ASSERT_THAT(BeginAttrChange(ctx_, z.id), IsOk());
  ASSERT_OK_AND_ASSIGN(Mutation rename,
                       BeginRename(ctx_, d, "x", d, "y", x.id, y.id, resolved));
  EXPECT_TRUE(rename.Owns(d));
  EXPECT_THAT(Lookup(ctx_, d, "x"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
}

// Review of R4, finding 2: an unlink's phase 1 verifies the same way that
// neither the parent nor the child changed since its caller resolved the
// child.
TEST_F(MetadataCacheTest, BeginRemoveRefusesAStaleResolution) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult x, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult z, Make(32));
  ASSERT_THAT(LinkDentry(ctx_, d, "x", x.id), IsOk());
  ASSERT_THAT(SyncClear(), IsOk());
  auto unchanged = [&] {
    EXPECT_THAT(Lookup(ctx_, d, "x"),
                IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
    EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
    EXPECT_FALSE(ctx_.fills.inflight.contains(d));
    EXPECT_FALSE(ctx_.fills.inflight.contains(x.id));
  };

  // The parent, and the child, each changed by a mutation that began and
  // ended after the snapshot.
  for (InodeId changed : {d, x.id}) {
    SCOPED_TRACE(changed);
    const FillSnapshot resolved = BeginFill(ctx_);
    ASSERT_THAT(BeginAttrChange(ctx_, changed), IsOk());  // Ends at once.
    ASSERT_THAT(SyncClear(), IsOk());
    EXPECT_THAT(BeginRemove(ctx_, d, "x", x.id, resolved),
                StatusIs(absl::StatusCode::kAborted));
    unchanged();
  }
  // A mutation of the parent in flight since before the snapshot.
  {
    ASSERT_OK_AND_ASSIGN(Mutation other, BeginCreate(ctx_, d, "w"));
    const FillSnapshot resolved = BeginFill(ctx_);
    EXPECT_THAT(BeginRemove(ctx_, d, "x", x.id, resolved),
                StatusIs(absl::StatusCode::kAborted));
    other.End();
    ASSERT_THAT(SyncClear(), IsOk());
  }
  // An unrelated inode's mutation does not matter.
  const FillSnapshot resolved = BeginFill(ctx_);
  ASSERT_THAT(BeginAttrChange(ctx_, z.id), IsOk());
  ASSERT_OK_AND_ASSIGN(Mutation unlink,
                       BeginRemove(ctx_, d, "x", x.id, resolved));
  EXPECT_TRUE(unlink.Owns(d));
  EXPECT_TRUE(unlink.Owns(x.id));
  EXPECT_THAT(Lookup(ctx_, d, "x"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
}

TEST_F(MetadataCacheTest, MarkDirtyIsNotDurableAndClearDirtyKeeps) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_FALSE(ctx_.dirty.any);

  const InodeId ids[] = {f.id, g.id};
  ASSERT_THAT(MarkDirty(ctx_, ids), IsOk());
  EXPECT_TRUE(ctx_.dirty.any);
  EXPECT_FALSE(ctx_.dirty.durable.contains(f.id));
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id, g.id)));

  const InodeId keep[] = {g.id};
  ASSERT_THAT(SyncClear(keep), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(g.id)));
  EXPECT_TRUE(ctx_.dirty.any);
  EXPECT_TRUE(ctx_.dirty.durable.empty());
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
  EXPECT_FALSE(ctx_.dirty.any);
}

// formal/ finding sync_during_mutation: a sync point clears only the rows
// its syncfs covers. A row stays if a mutation of its inode was in flight
// at any moment between BeginSync (just before the syncfs) and ClearDirty,
// or if it was added after BeginSync.
TEST_F(MetadataCacheTest, ClearDirtyKeepsWhatChangedDuringTheSync) {
  std::vector<InodeId> ids;
  for (uint64_t ino = 30; ino < 37; ++ino) {
    ASSERT_OK_AND_ASSIGN(UpsertResult r, Make(ino));
    ids.push_back(r.id);
  }
  const InodeId done = ids[0], across = ids[1], ended = ids[2],
                again = ids[3], late = ids[4], added = ids[5],
                kept = ids[6];
  ASSERT_THAT(SyncClear(), IsOk());

  // Before the sync point: `done` was mutated (and finished); `across` and
  // `ended` have a mutation in flight; `again` was mutated and finished;
  // `kept` has a writable open.
  ASSERT_THAT(BeginAttrChange(ctx_, done), IsOk());  // Ends at once.
  ASSERT_OK_AND_ASSIGN(Mutation m_across, BeginAttrChange(ctx_, across));
  ASSERT_OK_AND_ASSIGN(Mutation m_ended, BeginAttrChange(ctx_, ended));
  ASSERT_THAT(BeginAttrChange(ctx_, again), IsOk());
  ASSERT_THAT(BeginAttrChange(ctx_, kept), IsOk());

  ASSERT_OK_AND_ASSIGN(SyncSnapshot synced, BeginSync(ctx_));
  EXPECT_THAT(synced.dirty, ElementsAre(done, across, ended, again, kept));
  // While the syncfs runs: `ended`'s mutation ends (its syscall may have
  // come after the syncfs started), `again` is mutated again, `late` gets
  // a mutation that is still in flight, and `added` is made dirty by a
  // phase 3 (MarkDirty).
  m_ended.End();
  ASSERT_THAT(BeginAttrChange(ctx_, again), IsOk());
  ASSERT_OK_AND_ASSIGN(Mutation m_late, BeginAttrChange(ctx_, late));
  const InodeId added_ids[] = {added};
  ASSERT_THAT(MarkDirty(ctx_, added_ids), IsOk());

  const InodeId keep[] = {kept};
  int64_t cleared = -1;
  ASSERT_THAT(ClearDirty(ctx_, synced, keep, &cleared), IsOk());
  EXPECT_EQ(cleared, 1) << "the slow path counts the rows it deleted";
  // Only `done` was covered by the syncfs.
  EXPECT_THAT(ListDirty(ctx_),
              IsOkAndHolds(ElementsAre(across, ended, again, late, added,
                                       kept)));
  EXPECT_TRUE(ctx_.dirty.any);
  EXPECT_TRUE(ctx_.dirty.durable.empty());

  // The next sync point, with the mutations still in flight then over.
  ASSERT_THAT(SyncClear(keep), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(across, late, kept)));
  m_across.End();
  m_late.End();
  ASSERT_THAT(SyncClear(keep), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(kept)));
  EXPECT_TRUE(ctx_.dirty.any);
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
  EXPECT_FALSE(ctx_.dirty.any);
}

// A writable open's writes lie between its phase 1 and the last release
// (EndWrites), which dcfs never sees one by one. Each half of the sync
// point's protection, alone:
//  - an inode open for writing when BeginSync ran keeps its row even if the
//    open is gone by ClearDirty and nothing told the guards (the backstop);
//  - EndWrites is a guard event: a release after BeginSync keeps the row
//    even if the snapshot did not list the open, and a fill snapshot from
//    before the release cannot record anything.
TEST_F(MetadataCacheTest, ClearDirtyKeepsWhatWasOpenForWritingDuringTheSync) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_THAT(SyncClear(), IsOk());
  absl::flat_hash_set<int64_t> open_for_write;
  ctx_.open_for_write = &open_for_write;

  // The backstop: f is open for writing at BeginSync, and released (with
  // no EndWrites) before ClearDirty.
  open_for_write.insert(f.id);
  ASSERT_THAT(BeginAttrChange(ctx_, f.id), IsOk());  // BeginWriting.
  ASSERT_OK_AND_ASSIGN(SyncSnapshot synced, BeginSync(ctx_));
  EXPECT_THAT(synced.open_for_write, ElementsAre(f.id));
  open_for_write.erase(f.id);
  ASSERT_THAT(ClearDirty(ctx_, synced, {}), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id)));
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));

  // The guard event: g's release (EndWrites) comes after a BeginSync that
  // did not see g open.
  ASSERT_THAT(BeginAttrChange(ctx_, g.id), IsOk());
  const FillSnapshot fill = BeginFill(ctx_);
  ASSERT_OK_AND_ASSIGN(synced, BeginSync(ctx_));
  EXPECT_THAT(synced.open_for_write, ::testing::IsEmpty());
  EndWrites(ctx_, g.id);
  EXPECT_FALSE(CanFill(ctx_, fill, g.id));
  EXPECT_TRUE(CanFill(ctx_, fill, f.id));
  EXPECT_TRUE(CanFill(ctx_, BeginFill(ctx_), g.id));
  ASSERT_THAT(ClearDirty(ctx_, synced, {}), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(g.id)));
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
  ctx_.open_for_write = nullptr;
}

// A sync point during which nothing moved (no mutation began or ended, no
// writable open ended, nothing in flight): ClearDirty takes a fast path
// (one bulk delete), which must clear exactly what the per-row rule would.
// Kept: `keep`, and what was open for writing at BeginSync; not added:
// a `keep` inode that was not dirty.
TEST_F(MetadataCacheTest, ClearDirtyWhenNothingMovedIsExact) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_OK_AND_ASSIGN(UpsertResult h, Make(32));
  ASSERT_OK_AND_ASSIGN(UpsertResult k, Make(33));
  ASSERT_THAT(SyncClear(), IsOk());
  for (InodeId id : {f.id, g.id, h.id}) {
    ASSERT_THAT(BeginAttrChange(ctx_, id), IsOk());  // Ends at once.
  }
  absl::flat_hash_set<int64_t> open_for_write = {h.id};
  ctx_.open_for_write = &open_for_write;
  ASSERT_OK_AND_ASSIGN(SyncSnapshot synced, BeginSync(ctx_));
  open_for_write.clear();
  ctx_.open_for_write = nullptr;
  ASSERT_EQ(ctx_.fills.seq, synced.fills.seq);
  ASSERT_TRUE(ctx_.fills.inflight.empty());

  const InodeId keep[] = {g.id, k.id};
  ASSERT_THAT(ClearDirty(ctx_, synced, keep), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(g.id, h.id)));
  EXPECT_TRUE(ctx_.dirty.any);
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
  EXPECT_FALSE(ctx_.dirty.any);
}

// The clock alone does not tell: a mutation in flight since before
// BeginSync moves nothing during the syncfs, but its syscall may come
// after it, so its row stays.
TEST_F(MetadataCacheTest, ClearDirtyKeepsAMutationInFlightWhenNothingMoved) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult m, Make(31));
  ASSERT_THAT(SyncClear(), IsOk());
  ASSERT_THAT(BeginAttrChange(ctx_, f.id), IsOk());
  ASSERT_OK_AND_ASSIGN(Mutation in_flight, BeginAttrChange(ctx_, m.id));
  ASSERT_OK_AND_ASSIGN(SyncSnapshot synced, BeginSync(ctx_));
  ASSERT_EQ(ctx_.fills.seq, synced.fills.seq);
  int64_t cleared = -1;
  ASSERT_THAT(ClearDirty(ctx_, synced, {}, &cleared), IsOk());
  EXPECT_EQ(cleared, 1) << "the fast path counts the rows it did not put back";
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(m.id)));
  in_flight.End();
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
}

// If the fill guards forgot which inodes were mutated since BeginSync
// (FillGuards::touched was pruned, raising the floor past the snapshot),
// a sync point cannot tell which rows its syncfs covers, and keeps them
// all. A small prune bound (FillGuards::max_touched) gets there with three
// mutations instead of 65536.
TEST_F(MetadataCacheTest, ClearDirtyKeepsEverythingPastTheFloor) {
  ctx_.fills.max_touched = 2;
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(31));
  ASSERT_OK_AND_ASSIGN(UpsertResult h, Make(32));
  ASSERT_THAT(SyncClear(), IsOk());
  ASSERT_THAT(BeginAttrChange(ctx_, f.id), IsOk());
  ASSERT_OK_AND_ASSIGN(SyncSnapshot synced, BeginSync(ctx_));
  const FillSnapshot before = BeginFill(ctx_);

  // During the syncfs: g's mutation fills `touched` to its bound, and h's
  // (a new inode) prunes it.
  ASSERT_THAT(BeginAttrChange(ctx_, g.id), IsOk());
  ASSERT_LE(ctx_.fills.floor, synced.fills.seq);
  ASSERT_THAT(BeginAttrChange(ctx_, h.id), IsOk());
  EXPECT_GT(ctx_.fills.floor, synced.fills.seq);
  EXPECT_LE(ctx_.fills.touched.size(), 2u);
  EXPECT_FALSE(CanFill(ctx_, before, f.id));
  EXPECT_TRUE(CanFill(ctx_, BeginFill(ctx_), f.id));

  // f was not mutated since the snapshot, but the guards no longer know.
  ASSERT_THAT(ClearDirty(ctx_, synced, {}), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(ElementsAre(f.id, g.id, h.id)));
}

TEST_F(MetadataCacheTest, RecoverDirtyForgetsExactlyTheDirtyEntries) {
  // Dirty: directory d (with a child and a negative entry), file f (with
  // xattrs, linked from the root and from d), symlink s. Clean: directory
  // c (with a child), file k.
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(InodeId c, MakeDir(kRootInode, "c", 21));
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  ASSERT_OK_AND_ASSIGN(UpsertResult s, Make(31, S_IFLNK | 0777));
  ASSERT_OK_AND_ASSIGN(UpsertResult k, Make(32));
  ASSERT_OK_AND_ASSIGN(UpsertResult dchild, Make(33));
  ASSERT_OK_AND_ASSIGN(UpsertResult cchild, Make(34));
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "f", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, d, "f2", f.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "s", s.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, kRootInode, "k", k.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, d, "x", dchild.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, d, "neg"), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, c, "y", cchild.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, c, "neg"), IsOk());
  const std::vector<std::pair<std::string, std::string>> f_xattrs = {
      {"user.a", "1"}};
  const std::vector<std::pair<std::string, std::string>> k_xattrs = {
      {"user.b", "2"}};
  ASSERT_THAT(ReplaceXattrs(ctx_, f.id, f_xattrs), IsOk());
  ASSERT_THAT(ReplaceXattrs(ctx_, k.id, k_xattrs), IsOk());
  ASSERT_THAT(SetSymlink(ctx_, s.id, "target"), IsOk());
  for (InodeId dir : {kRootInode, d, c}) {
    ASSERT_THAT(MarkDirComplete(ctx_, dir, true), IsOk());
  }
  ASSERT_THAT(SyncClear(), IsOk());
  const InodeId dirty[] = {d, f.id, s.id, 999 /* no row any more */};
  ASSERT_THAT(MarkDirty(ctx_, dirty), IsOk());

  EXPECT_THAT(RecoverDirty(ctx_), IsOkAndHolds(4));
  // The dirty set stays until a sync point (step 12.6b): the crashed run's
  // backing changes may not be durable yet.
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::UnorderedElementsAre(
                                   d, f.id, s.id, 999)));
  EXPECT_TRUE(ctx_.dirty.any);

  // d: attributes unknown, listing forgotten (positive and negative) and
  // incomplete, and its own dentry forgotten, so the root is incomplete.
  ASSERT_OK_AND_ASSIGN(CachedAttr d_attr, GetAttr(ctx_, d));
  EXPECT_FALSE(d_attr.valid);
  EXPECT_THAT(CountRows(db_, absl::StrCat("dentries WHERE parent = ", d)),
              IsOkAndHolds(0));
  EXPECT_THAT(IsDirComplete(ctx_, d), IsOkAndHolds(false));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "d"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
  // f: attributes and xattrs unknown (rows gone), every dentry to it gone.
  ASSERT_OK_AND_ASSIGN(CachedAttr f_attr, GetAttr(ctx_, f.id));
  EXPECT_FALSE(f_attr.valid);
  EXPECT_THAT(ListXattrs(ctx_, f.id), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(GetXattr(ctx_, f.id, "user.a"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(CountRows(db_, absl::StrCat("dentries WHERE inode = ", f.id)),
              IsOkAndHolds(0));
  // s: target forgotten.
  EXPECT_THAT(Readlink(ctx_, s.id), StatusIs(absl::StatusCode::kNotFound));
  // The rows themselves (and their generations) are kept.
  EXPECT_THAT(GetGeneration(ctx_, f.id), IsOkAndHolds(f.fuse_gen));
  EXPECT_THAT(GetAttr(ctx_, dchild.id), IsOk());

  // Everything clean is untouched.
  for (InodeId id : {c, k.id, cchild.id, dchild.id}) {
    ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, id));
    EXPECT_TRUE(attr.valid) << id;
  }
  EXPECT_THAT(IsDirComplete(ctx_, c), IsOkAndHolds(true));
  EXPECT_THAT(Lookup(ctx_, c, "y"), IsOkAndHolds(IsFoundAs(cchild.id)));
  EXPECT_THAT(Lookup(ctx_, c, "neg"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "c"), IsOkAndHolds(IsFoundAs(c)));
  EXPECT_THAT(Lookup(ctx_, kRootInode, "k"), IsOkAndHolds(IsFoundAs(k.id)));
  EXPECT_THAT(ListXattrs(ctx_, k.id),
              IsOkAndHolds(Optional(ElementsAre("user.b"))));

  // Recovering again (a crash during recovery) finds the same rows and
  // changes nothing more; after a sync point nothing is dirty and there is
  // nothing to do.
  EXPECT_THAT(RecoverDirty(ctx_), IsOkAndHolds(4));
  EXPECT_THAT(IsDirComplete(ctx_, c), IsOkAndHolds(true));
  ASSERT_THAT(SyncClear(), IsOk());
  EXPECT_THAT(ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
  EXPECT_THAT(RecoverDirty(ctx_), IsOkAndHolds(0));
}

// Audit F5: a create-family op changes the parent's mtime/ctime (and nlink,
// for mkdir), so its phase 1 must mark the parent's attributes unknown,
// like BeginRemove/BeginRename/BeginLink do. Simulates a crash (or any
// reader) between phase 1 and phase 3 of a mkdir: the parent's pre-create
// attributes must not be served as current.
TEST_F(MetadataCacheTest, BeginCreateMarksTheParentsAttributesUnknown) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(CachedAttr before, GetAttr(ctx_, d));
  ASSERT_TRUE(before.valid);
  ASSERT_THAT(BeginCreate(ctx_, d, "new"), IsOk());
  ASSERT_OK_AND_ASSIGN(CachedAttr after, GetAttr(ctx_, d));
  EXPECT_FALSE(after.valid);
}

// Audit F4: a mutation's phase 3 must not undo something another request
// made unknown in between. With per-name dentry states there is no
// completeness to restore any more; this simulates the same interleaving:
// phase 1 of a create in d, then a concurrent request that invalidates
// sibling s, then phase 3. "s" -- which exists -- must stay unknown, not
// become absent.
TEST_F(MetadataCacheTest, PhaseThreeDoesNotUndoAnotherInvalidation) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult s, Make(21));
  ASSERT_THAT(LinkDentry(ctx_, d, "s", s.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, d, true), IsOk());

  ASSERT_THAT(BeginCreate(ctx_, d, "new"), IsOk());
  ASSERT_THAT(InvalidateInode(ctx_, s.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, d, "new"), IsOk());  // phase 3
  EXPECT_THAT(Lookup(ctx_, d, "s"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(IsDirComplete(ctx_, d), IsOkAndHolds(false));

  // The same with an out-of-band change detected meanwhile: new names may
  // have appeared, so the listing is incomplete.
  ASSERT_THAT(BeginCreate(ctx_, d, "new2"), IsOk());
  ASSERT_THAT(ForgetNegativeDentries(ctx_, d), IsOk());
  ASSERT_THAT(SetNegative(ctx_, d, "new2"), IsOk());
  EXPECT_THAT(ChildrenComplete(ctx_, d), IsOkAndHolds(false));
  EXPECT_THAT(Lookup(ctx_, d, "other"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
}

// Audit F1: a fill (read the backing filesystem, then record what it read)
// must not overwrite a newer mutation's result, nor cache data read while a
// mutation was changing it. These simulate the coroutine interleavings by
// running a fill's snapshot, a mutation's phases and the fill's commit in
// the problematic order; they prove the commit-time check, not that every
// caller uses it (the backing layer's fills all take the snapshot before
// their first syscall).
TEST_F(MetadataCacheTest, FillAfterACompletedMutationDoesNotOverwrite) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(30));
  // The fill snapshots, then (suspended in its statx) reads the old mode.
  FillSnapshot fill = BeginFill(ctx_);
  // Meanwhile a chmod runs start to finish: phase 1, the syscall, phase 3.
  {
    ASSERT_OK_AND_ASSIGN(Mutation chmod, BeginAttrChange(ctx_, f.id));
    chmod.End();
    FillSnapshot phase3 = BeginFill(ctx_);
    ASSERT_THAT(FillAttr(ctx_, phase3, f.id, Stx(30, S_IFREG | 0600, 2)),
                IsOkAndHolds(true));
  }
  // The stale fill commits: it must not overwrite the chmod's result.
  EXPECT_THAT(FillAttr(ctx_, fill, f.id, Stx(30, S_IFREG | 0644, 1)),
              IsOkAndHolds(false));
  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, f.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_mode, S_IFREG | 0600);
  // Nor the xattrs, symlink target, or a single xattr.
  const std::vector<std::pair<std::string, std::string>> old = {{"user.a", "1"}};
  EXPECT_THAT(FillXattrs(ctx_, fill, f.id, old), IsOkAndHolds(false));
  EXPECT_THAT(FillXattr(ctx_, fill, f.id, "user.a", "1"), IsOkAndHolds(false));
  EXPECT_THAT(FillSymlink(ctx_, fill, f.id, "t"), IsOkAndHolds(false));
  EXPECT_THAT(GetXattr(ctx_, f.id, "user.a"), IsOkAndHolds(std::nullopt));
}

TEST_F(MetadataCacheTest, FillDuringAMutationDoesNotCache) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(31));
  ASSERT_OK_AND_ASSIGN(Mutation setxattr, BeginXattrChange(ctx_, f.id, "user.k"));
  // A fill that starts and commits while the mutation is in flight.
  FillSnapshot during = BeginFill(ctx_);
  EXPECT_THAT(FillXattr(ctx_, during, f.id, "user.k", "old"),
              IsOkAndHolds(false));
  EXPECT_THAT(GetXattr(ctx_, f.id, "user.k"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(FillAttr(ctx_, during, f.id, Stx(31, S_IFREG)),
              IsOkAndHolds(false));
  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, f.id));
  EXPECT_FALSE(attr.valid);
  // Its phase 3 may write, since nothing else touched f.
  EXPECT_TRUE(setxattr.Owns(f.id));
  setxattr.End();
  EXPECT_FALSE(setxattr.Owns(f.id));
  // A fill that started during the mutation and commits after it ended
  // read possibly pre-syscall data: still not cached.
  EXPECT_THAT(FillAttr(ctx_, during, f.id, Stx(31, S_IFREG)),
              IsOkAndHolds(false));
  // Other inodes are unaffected.
  ASSERT_OK_AND_ASSIGN(UpsertResult g, Make(32));
  EXPECT_THAT(FillAttr(ctx_, during, g.id, Stx(32, S_IFREG)),
              IsOkAndHolds(true));
}

TEST_F(MetadataCacheTest, OverlappingMutationsDoNotOwnTheirPhase3) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(33));
  ASSERT_OK_AND_ASSIGN(Mutation first, BeginXattrChange(ctx_, f.id, "user.k"));
  ASSERT_OK_AND_ASSIGN(Mutation second, BeginAttrChange(ctx_, f.id));
  // `first`'s read-back predates `second`'s syscall; `second`'s phase 3
  // runs while `first` may still change f.
  EXPECT_FALSE(first.Owns(f.id));
  EXPECT_FALSE(second.Owns(f.id));
  first.End();
  EXPECT_FALSE(second.Owns(f.id)) << "first began before second's phase 1 "
                                     "and was in flight during it";
}

TEST_F(MetadataCacheTest, FillWithNoConcurrentMutationCaches) {
  ASSERT_OK_AND_ASSIGN(UpsertResult f, Make(34));
  {
    // A mutation that ended before the fill's snapshot does not matter.
    ASSERT_OK_AND_ASSIGN(Mutation done, BeginAttrChange(ctx_, f.id));
  }
  FillSnapshot fill = BeginFill(ctx_);
  EXPECT_THAT(FillAttr(ctx_, fill, f.id, Stx(34, S_IFREG | 0640)),
              IsOkAndHolds(true));
  ASSERT_OK_AND_ASSIGN(CachedAttr attr, GetAttr(ctx_, f.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_mode, S_IFREG | 0640);
  const std::vector<std::pair<std::string, std::string>> xattrs = {
      {"user.a", "1"}};
  EXPECT_THAT(FillXattrs(ctx_, fill, f.id, xattrs), IsOkAndHolds(true));
  EXPECT_THAT(ListXattrs(ctx_, f.id),
              IsOkAndHolds(Optional(ElementsAre("user.a"))));
  EXPECT_THAT(FillXattr(ctx_, fill, f.id, "user.b", std::nullopt),
              IsOkAndHolds(true));
  EXPECT_THAT(FillSymlink(ctx_, fill, f.id, "t"), IsOkAndHolds(true));
}

// Audit F8: deleting an inode row must never turn a dentry that pointed at
// it into a negative ("absent") entry: the name still exists as far as
// anyone knows, it is just no longer cached. Deleted directly, bypassing
// InvalidateInode's own bookkeeping, as a cascade (e.g. PurgeFilesystem)
// would.
TEST_F(MetadataCacheTest, DeletingAnInodeRowLeavesItsDentriesUnknown) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult x, Make(21));
  ASSERT_THAT(LinkDentry(ctx_, d, "x", x.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, d, true), IsOk());
  ASSERT_THAT(db_.Exec(absl::StrCat("DELETE FROM inodes WHERE id = ", x.id)),
              IsOk());
  EXPECT_THAT(Lookup(ctx_, d, "x"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  // A listing with an unknown name in it is not complete.
  EXPECT_THAT(IsDirComplete(ctx_, d), IsOkAndHolds(false));
}

// Dentries have explicit per-name states: phase 1 of a create marks just
// that name unknown, leaving the directory's other names -- including the
// ones known absent because the listing is complete -- as they were.
TEST_F(MetadataCacheTest, PhaseOneMarksOnlyItsOwnNameUnknown) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult x, Make(21));
  ASSERT_THAT(LinkDentry(ctx_, d, "x", x.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, d, true), IsOk());

  ASSERT_THAT(BeginCreate(ctx_, d, "new"), IsOk());
  EXPECT_THAT(Lookup(ctx_, d, "new"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, d, "other"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(Lookup(ctx_, d, "x"), IsOkAndHolds(IsFoundAs(x.id)));
  // Not listable while "new" is unknown...
  EXPECT_THAT(IsDirComplete(ctx_, d), IsOkAndHolds(false));
  // ...and complete again once phase 3 resolves it, with nothing to
  // restore.
  ASSERT_THAT(SetNegative(ctx_, d, "new"), IsOk());
  EXPECT_THAT(IsDirComplete(ctx_, d), IsOkAndHolds(true));

  // Invalidating a child makes just its name unknown, too.
  ASSERT_THAT(InvalidateInode(ctx_, x.id), IsOk());
  EXPECT_THAT(Lookup(ctx_, d, "x"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  EXPECT_THAT(Lookup(ctx_, d, "other"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
}

// Audit-races F8: a readdir resumed from a cursor (a dentry rowid) must
// not see an unchanged name again because a mutation re-inserted its row
// with a new rowid: a rename over an existing name, or a failed rmdir
// (phase 1 marks the name unknown, the failure path resolves it again).
TEST_F(MetadataCacheTest, ListDirCursorSurvivesRenameOverAndFailedRemove) {
  ASSERT_OK_AND_ASSIGN(InodeId d, MakeDir(kRootInode, "d", 20));
  ASSERT_OK_AND_ASSIGN(UpsertResult a, Make(21));
  ASSERT_OK_AND_ASSIGN(UpsertResult b, Make(22));
  ASSERT_OK_AND_ASSIGN(UpsertResult c, Make(24));
  ASSERT_THAT(LinkDentry(ctx_, d, "a", a.id), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, d, "b", b.id), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId e, MakeDir(d, "e", 23));
  ASSERT_THAT(LinkDentry(ctx_, d, "c", c.id), IsOk());
  ASSERT_THAT(MarkDirComplete(ctx_, d, true), IsOk());

  // A reader has listed up to and including "b".
  auto list_after = [&](int64_t cursor) {
    std::vector<std::string> names;
    EXPECT_THAT(ListDir(ctx_, d, cursor,
                        [&](std::string_view name, InodeId, int64_t) {
                          names.emplace_back(name);
                          return true;
                        }),
                IsOk());
    return names;
  };
  int64_t cursor = 0;
  ASSERT_THAT(ListDir(ctx_, d, 0,
                      [&](std::string_view name, InodeId, int64_t next) {
                        cursor = next;
                        return name != "b";
                      }),
              IsOk());
  ASSERT_THAT(list_after(cursor), ElementsAre("e", "c"));

  // rename(d/a, d/b) over the existing b: phase 1, then phase 3.
  ASSERT_THAT(
      BeginRename(ctx_, d, "a", d, "b", a.id, b.id, BeginFill(ctx_)),
      IsOk());
  ASSERT_THAT(LinkDentry(ctx_, d, "b", a.id), IsOk());
  ASSERT_THAT(SetNegative(ctx_, d, "a"), IsOk());
  EXPECT_THAT(list_after(cursor), ElementsAre("e", "c"));

  // rmdir(d/e) failing (ENOTEMPTY): phase 1, then the name re-resolved.
  ASSERT_THAT(BeginRemove(ctx_, d, "e", e, BeginFill(ctx_)), IsOk());
  ASSERT_THAT(LinkDentry(ctx_, d, "e", e), IsOk());
  EXPECT_THAT(list_after(cursor), ElementsAre("e", "c"));
}

// Audit-races "not a race": an inode generation of 0 means "could not be
// read" (backing::ReadGeneration: no FS_IOC_GETVERSION, EACCES, ...), and
// VerifyBackingIdentity already treats it as "unknown, same object". So
// must UpsertInode: a transient failure to read the generation of an
// object cached with its real one must not split it into a new row (and
// ESTALE the old nodeid). The handle bytes still tell objects apart.
TEST_F(MetadataCacheTest, UnknownGenerationDoesNotSplitAnInode) {
  ASSERT_OK_AND_ASSIGN(UpsertResult first, Make(40, S_IFREG | 0644, kSource, 7));
  ASSERT_OK_AND_ASSIGN(UpsertResult again, Make(40, S_IFREG | 0644, kSource, 0));
  EXPECT_FALSE(again.created);
  EXPECT_EQ(again.id, first.id);
  EXPECT_THAT(GetAttr(ctx_, first.id), IsOk());
  // A different handle for the same inode number is still a different
  // (recycled) object, generation known or not.
  ASSERT_OK_AND_ASSIGN(
      UpsertResult other,
      UpsertInode(ctx_, Handle(kSource, "h40-recycled"), Stx(40, S_IFREG), 0));
  EXPECT_TRUE(other.created);
  EXPECT_NE(other.id, first.id);
}

}  // namespace
}  // namespace dcfs::cache
