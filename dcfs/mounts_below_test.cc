#include "dcfs/mounts_below.h"

#include <fcntl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

class MountsBelowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    std::string templ = absl::StrCat(tmpdir, "/mounts_below_XXXXXX");
    ASSERT_OK_AND_ASSIGN(source_, syscalls::mkdtemp(templ));
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("plain"), 0755), IsOk());
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mnt"), 0755), IsOk());
  }

  void TearDown() override {
    if (mounted_) {
      syscalls::umount2(Path("mnt"), MNT_DETACH).IgnoreError();
    }
  }

  std::string Path(std::string_view rel) const {
    return absl::StrCat(source_, "/", rel);
  }

  std::string source_;
  bool mounted_ = false;
};

TEST_F(MountsBelowTest, NothingBelowAPlainDirectory) {
  EXPECT_THAT(MountsBelow(source_), IsOkAndHolds(IsEmpty()));
}

TEST_F(MountsBelowTest, ReportsATmpfsMountedBelowTheSource) {
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt"), "tmpfs", 0, nullptr),
              IsOk());
  mounted_ = true;
  EXPECT_THAT(MountsBelow(source_), IsOkAndHolds(UnorderedElementsAre(Path("mnt"))));
}

// A mount ON the source itself (root_fs === --source, e.g. dcfs pointed
// directly at a filesystem's mount point, or a bind mount of the source
// onto itself) is not "below" it -- amendment 12's startup check must not
// refuse this configuration.
TEST_F(MountsBelowTest, AMountOnTheSourceItselfIsNotReported) {
  ASSERT_THAT(
      syscalls::mount(source_.c_str(), source_, nullptr, MS_BIND, nullptr),
      IsOk());
  auto result = MountsBelow(source_);
  EXPECT_THAT(syscalls::umount2(source_, MNT_DETACH), IsOk());
  EXPECT_THAT(result, IsOkAndHolds(IsEmpty()));
}

// --- The parser, on canned /proc/self/mountinfo text ---------------------

// A mountinfo line with the five leading fields the parser reads, `point` as
// field 5 (already escaped by the caller), some optional fields and the
// separator.
std::string Line(std::string_view point,
                 std::string_view optional = "shared:1") {
  return absl::StrCat("36 35 98:0 / ", point, " rw,noatime ", optional,
                      " - ext4 /dev/root rw\n");
}

TEST(MountPointsBelowTest, OnlyMountsStrictlyBelowTheSource) {
  std::string info =
      absl::StrCat(Line("/"), Line("/src"), Line("/src/a"), Line("/srcother"),
                   Line("/src/a/b"), Line("/elsewhere/src/c"), Line("/sr"));
  EXPECT_THAT(MountPointsBelow(info, "/src"),
              ::testing::ElementsAre("/src/a", "/src/a/b"));
}

TEST(MountPointsBelowTest, ARootSourceTakesEveryOtherMount) {
  std::string info = absl::StrCat(Line("/"), Line("/proc"), Line("/a/b"));
  EXPECT_THAT(MountPointsBelow(info, "/"),
              ::testing::ElementsAre("/proc", "/a/b"));
}

TEST(MountPointsBelowTest, OctalEscapesAreDecoded) {
  std::string info =
      absl::StrCat(Line("/src/with\\040space"), Line("/src/tab\\011in"),
                   Line("/src/new\\012line"), Line("/src/back\\134slash"));
  EXPECT_THAT(MountPointsBelow(info, "/src"),
              ::testing::ElementsAre("/src/with space", "/src/tab\tin",
                                     "/src/new\nline", "/src/back\\slash"));
}

// A backslash that does not start a three-digit octal escape is a plain
// character, wherever the field ends.
TEST(MountPointsBelowTest, ABackslashThatIsNotAnEscapeIsKept) {
  std::string info = absl::StrCat(Line("/src/a\\08x"), Line("/src/b\\04"),
                                  Line("/src/c\\"), Line("/src/d\\0409"));
  EXPECT_THAT(MountPointsBelow(info, "/src"),
              ::testing::ElementsAre("/src/a\\08x", "/src/b\\04", "/src/c\\",
                                     "/src/d 9"));
}

TEST(MountPointsBelowTest, ASourceNameThatNeedsEscapingStillMatches) {
  // The source is canonical (decoded); the file's field is escaped.
  std::string info = absl::StrCat(Line("/my\\040src"), Line("/my\\040src/m"));
  EXPECT_THAT(MountPointsBelow(info, "/my src"),
              ::testing::ElementsAre("/my src/m"));
}

TEST(MountPointsBelowTest, ShortLinesAndBlankLinesAreIgnored) {
  std::string info =
      absl::StrCat("\n", "36 35 98:0 /\n", "36 35 98:0 /src/short\n", "   \n",
                   Line("/src/ok"), "\n\n");
  EXPECT_THAT(MountPointsBelow(info, "/src"),
              ::testing::ElementsAre("/src/ok"));
}

TEST(MountPointsBelowTest, ExactlyFiveFieldsIsEnough) {
  EXPECT_THAT(MountPointsBelow("1 2 3:4 / /src/five\n", "/src"),
              ::testing::ElementsAre("/src/five"));
}

TEST(MountPointsBelowTest, ExtraSpacesBetweenFieldsAreSkipped) {
  EXPECT_THAT(MountPointsBelow("1  2   3:4 / /src/x rw - ext4 a b\n", "/src"),
              ::testing::ElementsAre("/src/x"));
}

TEST(MountPointsBelowTest, NoTrailingNewlineIsFine) {
  EXPECT_THAT(MountPointsBelow("1 2 3:4 / /src/x rw - ext4 a b", "/src"),
              ::testing::ElementsAre("/src/x"));
}

TEST(MountPointsBelowTest, EmptyInputHasNothingBelow) {
  EXPECT_THAT(MountPointsBelow("", "/src"), IsEmpty());
}

// --- The real mountinfo ------------------------------------------------

TEST_F(MountsBelowTest, NestedMountsAreAllReported) {
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt"), "tmpfs", 0, nullptr),
              IsOk());
  mounted_ = true;
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mnt/inner"), 0755), IsOk());
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt/inner"), "tmpfs", 0, nullptr),
              IsOk());
  EXPECT_THAT(MountsBelow(source_), IsOkAndHolds(UnorderedElementsAre(
                                        Path("mnt"), Path("mnt/inner"))));
}

// The kernel escapes a mount point's space, tab, newline and backslash; a
// mount below a directory named with all four is still found, by its real
// name.
TEST_F(MountsBelowTest, AMountPointWithSpecialCharactersIsReportedByName) {
  const std::string name = "sp ace\ttab\nnl\\bs";
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path(name), 0755), IsOk());
  ASSERT_THAT(syscalls::mount("tmpfs", Path(name), "tmpfs", 0, nullptr),
              IsOk());
  auto result = MountsBelow(source_);
  EXPECT_THAT(syscalls::umount2(Path(name), MNT_DETACH), IsOk());
  EXPECT_THAT(result, IsOkAndHolds(UnorderedElementsAre(Path(name))));
}

// A path that is not canonical (a "..", a symlink) names the same source.
TEST_F(MountsBelowTest, ThePathIsCanonicalizedFirst) {
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt"), "tmpfs", 0, nullptr),
              IsOk());
  mounted_ = true;
  EXPECT_THAT(MountsBelow(Path("plain/..")),
              IsOkAndHolds(UnorderedElementsAre(Path("mnt"))));
}

TEST_F(MountsBelowTest, ASourceThatDoesNotExistIsAnError) {
  EXPECT_THAT(MountsBelow(Path("missing")),
              absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

// With "/" as the source, every other mount is below it, and "/" itself is
// not.
TEST_F(MountsBelowTest, TheRootSourceSeesTheGuestsMounts) {
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt"), "tmpfs", 0, nullptr),
              IsOk());
  mounted_ = true;
  absl::StatusOr<std::vector<std::string>> below = MountsBelow("/");
  ASSERT_THAT(below, IsOk());
  EXPECT_THAT(*below, ::testing::Contains(Path("mnt")));
  EXPECT_THAT(*below, ::testing::Not(::testing::Contains("/")));
}

// --- ForcedReadOnly (step 11.5) ----------------------------------------------

constexpr std::string_view kMountinfo =
    "22 1 8:1 / / rw,relatime shared:1 - ext4 /dev/vda rw\n"
    "30 22 253:0 / /src rw,relatime shared:5 - btrfs /dev/mapper/b "
    "ro,space_cache=v2\n"
    "31 22 253:1 / /ro rw,relatime - ext4 /dev/vdc rw,errors=remount-ro\n"
    "32 22 253:2 / /both ro,relatime - xfs /dev/vdd ro,attr2\n"
    "33 22 253:3 / /mntro ro,relatime - ext4 /dev/vde rw\n"
    "34 22 0:5 / /broken rw\n"
    "35 22 253:4 / /emerg rw,relatime - ext4 /dev/vdf "
    "rw,errors=remount-ro,emergency_ro\n";

TEST(ForcedReadOnlyInTest, AReadOnlySuperblockUnderAReadWriteMountIsForced) {
  EXPECT_TRUE(ForcedReadOnlyIn(kMountinfo, 30));
  EXPECT_TRUE(ForcedReadOnlyIn(kMountinfo, 35));  // ext4's emergency_ro.
}

TEST(ForcedReadOnlyInTest, AnythingElseIsNot) {
  EXPECT_FALSE(ForcedReadOnlyIn(kMountinfo, 22));  // Read-write.
  EXPECT_FALSE(ForcedReadOnlyIn(kMountinfo, 31));  // "ro" only in a name.
  EXPECT_FALSE(ForcedReadOnlyIn(kMountinfo, 32));  // Mounted read-only.
  EXPECT_FALSE(ForcedReadOnlyIn(kMountinfo, 33));  // A read-only bind.
  EXPECT_FALSE(ForcedReadOnlyIn(kMountinfo, 34));  // No "-" separator.
  EXPECT_FALSE(ForcedReadOnlyIn(kMountinfo, 99));  // Not listed.
}

// A tmpfs whose superblock is remounted read-only under a read-write bind
// of it: what an error-forced read-only filesystem looks like.
TEST_F(MountsBelowTest, ForcedReadOnlyReadsTheMountOfADescriptor) {
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt"), "tmpfs", 0, nullptr),
              IsOk());
  mounted_ = true;
  ASSERT_THAT(syscalls::mount(Path("mnt").c_str(), Path("plain"), nullptr, MS_BIND,
                              nullptr),
              IsOk());
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor bind,
      syscalls::openat(AT_FDCWD, Path("plain"), O_RDONLY | O_DIRECTORY));
  EXPECT_THAT(ForcedReadOnly(*bind), IsOkAndHolds(false));
  ASSERT_THAT(syscalls::mount("tmpfs", Path("mnt"), "tmpfs",
                              MS_REMOUNT | MS_RDONLY, nullptr),
              IsOk());
  absl::StatusOr<bool> forced = ForcedReadOnly(*bind);
  EXPECT_THAT(syscalls::umount2(Path("plain"), MNT_DETACH), IsOk());
  EXPECT_THAT(forced, IsOkAndHolds(true));
}

}  // namespace
}  // namespace dcfs
