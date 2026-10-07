#include "dcfs/mounts_below.h"

#include <sys/mount.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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
    ASSERT_NE(::mkdtemp(templ.data()), nullptr) << std::strerror(errno);
    source_ = templ;
    ASSERT_EQ(::mkdir(Path("plain").c_str(), 0755), 0);
    ASSERT_EQ(::mkdir(Path("mnt").c_str(), 0755), 0);
  }

  void TearDown() override {
    if (mounted_) ::umount2(Path("mnt").c_str(), MNT_DETACH);
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
  ASSERT_EQ(::mount("tmpfs", Path("mnt").c_str(), "tmpfs", 0, nullptr), 0)
      << std::strerror(errno);
  mounted_ = true;
  EXPECT_THAT(MountsBelow(source_), IsOkAndHolds(UnorderedElementsAre(Path("mnt"))));
}

// A mount ON the source itself (root_fs === --source, e.g. dcfs pointed
// directly at a filesystem's mount point, or a bind mount of the source
// onto itself) is not "below" it -- amendment 12's startup check must not
// refuse this configuration.
TEST_F(MountsBelowTest, AMountOnTheSourceItselfIsNotReported) {
  ASSERT_EQ(::mount(source_.c_str(), source_.c_str(), nullptr, MS_BIND, nullptr),
            0)
      << std::strerror(errno);
  auto result = MountsBelow(source_);
  ::umount2(source_.c_str(), MNT_DETACH);
  EXPECT_THAT(result, IsOkAndHolds(IsEmpty()));
}

}  // namespace
}  // namespace dcfs
