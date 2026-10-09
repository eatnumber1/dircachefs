#include "dcfs/absolute_paths.h"

#include <cstdlib>
#include <string>

#include "absl/status/status_matchers.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "fcntl.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

// Runs in a fresh directory of TEST_TMPDIR holding a directory `rel`.
class AbsolutePathsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(
        dir_, syscalls::mkdtemp(
                  std::string(std::getenv("TEST_TMPDIR")) + "/paths.XXXXXX"));
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_ + "/rel", 0700), IsOk());
    ASSERT_THAT(syscalls::chdir(dir_), IsOk());
    ASSERT_OK_AND_ASSIGN(real_, syscalls::realpath(dir_));
  }
  void TearDown() override { syscalls::chdir("/").IgnoreError(); }

  std::string dir_;
  std::string real_;
};

HelperOptions Options(HelperOptions::Backing backing) {
  HelperOptions options;
  options.backing = backing;
  return options;
}

TEST_F(AbsolutePathsTest, MountpointAndCacheDatabaseBecomeAbsolute) {
  HelperArgs args = {.source = "/dev/x", .mountpoint = "rel"};
  HelperOptions options = Options(HelperOptions::Backing::kNative);
  options.cache_db = "cache.db";
  ASSERT_THAT(MakePathsAbsolute(args, options), IsOk());
  EXPECT_EQ(args.mountpoint, real_ + "/rel");
  EXPECT_EQ(*options.cache_db, real_ + "/cache.db");
  EXPECT_EQ(args.source, "/dev/x");
}

TEST_F(AbsolutePathsTest, NoneAndBindSourcesAreAlwaysPaths) {
  for (auto backing :
       {HelperOptions::Backing::kNone, HelperOptions::Backing::kBind}) {
    HelperArgs args = {.source = "rel", .mountpoint = "/m"};
    HelperOptions options = Options(backing);
    ASSERT_THAT(MakePathsAbsolute(args, options), IsOk());
    EXPECT_EQ(args.source, real_ + "/rel");
  }
}

// Specs that are not paths of this directory stay as written: a ZFS dataset,
// a virtiofs or 9p tag, tmpfs, a network export, a tag.
TEST_F(AbsolutePathsTest, ANativeSpecThatIsNotARelativePathStaysAsWritten) {
  for (const char *spec : {"pool/fs", "tmpfs", "myfs", "UUID=aaaa",
                           "host:/export", "/dev/vdb", "rel/missing"}) {
    HelperArgs args = {.source = spec, .mountpoint = "/m"};
    HelperOptions options = Options(HelperOptions::Backing::kNative);
    ASSERT_THAT(MakePathsAbsolute(args, options), IsOk());
    EXPECT_EQ(args.source, spec);
  }
}

TEST_F(AbsolutePathsTest, ANativeSpecThatIsARelativeDirectoryBecomesAbsolute) {
  HelperArgs args = {.source = "rel", .mountpoint = "/m"};
  HelperOptions options = Options(HelperOptions::Backing::kNative);
  ASSERT_THAT(MakePathsAbsolute(args, options), IsOk());
  EXPECT_EQ(args.source, real_ + "/rel");
}

}  // namespace
}  // namespace dcfs
