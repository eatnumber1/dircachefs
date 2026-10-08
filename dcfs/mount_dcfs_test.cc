// Step 15.1: the mount.dcfs wrapper's pure parts: the mount(8) helper
// command line, the split of its options into dcfs's and the underlying
// mount's, the exit statuses, the startup report the daemon sends the
// waiting wrapper, and the mountinfo check for a remount. The wrapper's
// behavior as a program is test/qemu/guest/mount_dcfs.sh.
#include "dcfs/mount_dcfs.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Optional;
using ::testing::Pair;

using Strings = std::vector<std::string>;

// --- mount(8)'s helper command line ----------------------------------------

TEST(ParseHelperArgsTest, SourceMountpointAndOptions) {
  EXPECT_THAT(
      ParseHelperArgs(Strings{"/dev/sdb1", "/data", "-o", "noatime,ro"}),
      IsOkAndHolds(AllOf(
          Field(&HelperArgs::source, "/dev/sdb1"),
          Field(&HelperArgs::mountpoint, "/data"),
          Field(&HelperArgs::options, ElementsAre("noatime", "ro")))));
}

TEST(ParseHelperArgsTest, OptionsMayComeBeforeThePositionals) {
  EXPECT_THAT(ParseHelperArgs(Strings{"-o", "ro", "UUID=aaaa", "/data"}),
              IsOkAndHolds(AllOf(Field(&HelperArgs::source, "UUID=aaaa"),
                                 Field(&HelperArgs::mountpoint, "/data"),
                                 Field(&HelperArgs::options,
                                       ElementsAre("ro")))));
}

TEST(ParseHelperArgsTest, RepeatedOptionsAccumulateInOrder) {
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-o", "a,b", "-o", "c"}),
              IsOkAndHolds(Field(&HelperArgs::options,
                                 ElementsAre("a", "b", "c"))));
}

TEST(ParseHelperArgsTest, SingleLetterFlagsMayBeGrouped) {
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-sfnv"}),
              IsOkAndHolds(AllOf(Field(&HelperArgs::sloppy, true),
                                 Field(&HelperArgs::fake, true),
                                 Field(&HelperArgs::no_mtab, true),
                                 Field(&HelperArgs::verbose, 1))));
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-v", "-v"}),
              IsOkAndHolds(Field(&HelperArgs::verbose, 2)));
}

TEST(ParseHelperArgsTest, NamespaceTakesAnArgument) {
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-N", "/proc/1/ns/mnt"}),
              IsOkAndHolds(Field(&HelperArgs::mount_namespace,
                                 Optional(Eq("/proc/1/ns/mnt")))));
}

TEST(ParseHelperArgsTest, AttachedOptionArgument) {
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-oro,sync"}),
              IsOkAndHolds(Field(&HelperArgs::options,
                                 ElementsAre("ro", "sync"))));
}

TEST(ParseHelperArgsTest, VersionNeedsNoPositionals) {
  EXPECT_THAT(ParseHelperArgs(Strings{"-V"}),
              IsOkAndHolds(Field(&HelperArgs::version, true)));
}

TEST(ParseHelperArgsTest, MissingMountpointIsAUsageError) {
  EXPECT_THAT(ParseHelperArgs(Strings{"/dev/sdb1"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("MOUNTPOINT")));
  EXPECT_THAT(ParseHelperArgs(Strings{}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("SOURCE")));
}

TEST(ParseHelperArgsTest, ExtraPositionalIsAUsageError) {
  EXPECT_THAT(ParseHelperArgs(Strings{"a", "b", "c"}),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("c")));
}

TEST(ParseHelperArgsTest, UnknownFlagAndMissingArgumentAreUsageErrors) {
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-x"}),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("-x")));
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-o"}),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("-o")));
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-N"}),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("-N")));
}

// --- the option split (decision 6) -----------------------------------------

TEST(SplitHelperOptionsTest, PrefixedOptionsGoToDcfsTheRestStayNative) {
  EXPECT_THAT(
      SplitHelperOptions(Strings{"noatime", "dcfs.fstype=ext4", "ro",
                                 "dcfs.foreground", "subvol=vol"}),
      IsOkAndHolds(AllOf(
          Field(&HelperOptions::native_options,
                ElementsAre("noatime", "ro", "subvol=vol")),
          Field(&HelperOptions::foreground, true),
          Field(&HelperOptions::native_type, Optional(Eq("ext4"))))));
}

TEST(SplitHelperOptionsTest, RoIsTheUnderlyingMountsAndDcfsRoIsDcfs) {
  EXPECT_THAT(SplitHelperOptions(Strings{"ro"}),
              IsOkAndHolds(AllOf(Field(&HelperOptions::read_only, false),
                                 Field(&HelperOptions::native_options,
                                       ElementsAre("ro")))));
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.ro"}),
              IsOkAndHolds(AllOf(Field(&HelperOptions::read_only, true),
                                 Field(&HelperOptions::native_options,
                                       IsEmpty()))));
}

TEST(SplitHelperOptionsTest, FstypeValues) {
  auto backing = [](const Strings &options) {
    return SplitHelperOptions(options);
  };
  EXPECT_THAT(backing({}), IsOkAndHolds(Field(&HelperOptions::backing,
                                              HelperOptions::Backing::kNative)));
  EXPECT_THAT(backing({"dcfs.fstype=none"}),
              IsOkAndHolds(Field(&HelperOptions::backing,
                                 HelperOptions::Backing::kNone)));
  EXPECT_THAT(backing({"dcfs.fstype=bind"}),
              IsOkAndHolds(Field(&HelperOptions::backing,
                                 HelperOptions::Backing::kBind)));
  EXPECT_THAT(backing({"dcfs.fstype=xfs"}),
              IsOkAndHolds(AllOf(
                  Field(&HelperOptions::backing,
                        HelperOptions::Backing::kNative),
                  Field(&HelperOptions::native_type, Optional(Eq("xfs"))))));
  EXPECT_THAT(backing({"dcfs.fstype=fuse.sshfs"}),
              IsOkAndHolds(Field(&HelperOptions::native_type,
                                 Optional(Eq("fuse.sshfs")))));
  // Absent: mount(8) autodetects the native type.
  EXPECT_THAT(backing({}), IsOkAndHolds(Field(&HelperOptions::native_type,
                                              std::nullopt)));
}

TEST(SplitHelperOptionsTest, EmptyFstypeIsAnError) {
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.fstype="}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("dcfs.fstype")));
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.fstype"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("dcfs.fstype")));
}

TEST(SplitHelperOptionsTest, UnknownDcfsOptionNamesItself) {
  EXPECT_THAT(SplitHelperOptions(Strings{"noatime", "dcfs.bogus"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("dcfs.bogus")));
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.bogus=1"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("dcfs.bogus")));
}

TEST(SplitHelperOptionsTest, FlagsKeepTheirValuesInOrder) {
  EXPECT_THAT(
      SplitHelperOptions(Strings{"dcfs.sync_interval_sec=2",
                                 "dcfs.attr_timeout_sec=0.5",
                                 "dcfs.stderrthreshold=0"}),
      IsOkAndHolds(Field(
          &HelperOptions::flags,
          ElementsAre(Pair("sync_interval_sec", "2"),
                      Pair("attr_timeout_sec", "0.5"),
                      Pair("stderrthreshold", "0")))));
}

TEST(SplitHelperOptionsTest, BooleanFlagsMayBeBare) {
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.allow_other"}),
              IsOkAndHolds(Field(&HelperOptions::flags,
                                 ElementsAre(Pair("allow_other", "true")))));
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.allow_other=0"}),
              IsOkAndHolds(Field(&HelperOptions::flags,
                                 ElementsAre(Pair("allow_other", "0")))));
}

TEST(SplitHelperOptionsTest, ValuedFlagWithoutValueIsAnError) {
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.sync_interval_sec"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("dcfs.sync_interval_sec")));
}

TEST(SplitHelperOptionsTest, FuseOptAccumulates) {
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.fuse_opt=max_read=65536",
                                         "dcfs.fuse_opt=suid"}),
              IsOkAndHolds(Field(&HelperOptions::fuse_options,
                                 ElementsAre("max_read=65536", "suid"))));
}

TEST(SplitHelperOptionsTest, CacheDb) {
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.cache_db=/cache/a.db"}),
              IsOkAndHolds(Field(&HelperOptions::cache_db,
                                 Optional(Eq("/cache/a.db")))));
}

// Step 15.3 owns the cache path derived from the instance identity; until
// then dcfs.cache_dir is refused rather than silently ignored.
TEST(SplitHelperOptionsTest, CacheDirIsNotYetSupported) {
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.cache_dir=/var/cache/dcfs"}),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("dcfs.cache_dir")));
}

TEST(SplitHelperOptionsTest, RemountIsRecognizedAndNotPassedOn) {
  EXPECT_THAT(SplitHelperOptions(Strings{"remount", "dcfs.ro", "noatime"}),
              IsOkAndHolds(AllOf(
                  Field(&HelperOptions::remount, true),
                  Field(&HelperOptions::native_options,
                        ElementsAre("noatime")))));
}

// --- exit statuses and the startup report ----------------------------------

TEST(ExitStatusTest, DcfsFailuresExitOne) {
  EXPECT_EQ(ExitStatusFor(absl::InvalidArgumentError("x")), 1);
  EXPECT_EQ(ExitStatusFor(absl::FailedPreconditionError("x")), 1);
  EXPECT_EQ(ExitStatusFor(absl::InternalError("x")), 1);
}

TEST(ExitStatusTest, NativeMountFailureKeepsItsOwnStatus) {
  absl::Status failed = NativeMountError(32, "mount: wrong fs type");
  EXPECT_THAT(failed, StatusIs(absl::StatusCode::kFailedPrecondition,
                               HasSubstr("wrong fs type")));
  EXPECT_EQ(ExitStatusFor(failed), 32);
  EXPECT_EQ(ExitStatusFor(NativeMountError(1, "x")), 1);
}

TEST(StartupReportTest, ReadyRoundTrips) {
  StartupReport report = DecodeStartupReport(EncodeStartupReport(
      StartupReport{.ready = true, .exit_status = 0, .message = ""}));
  EXPECT_TRUE(report.ready);
  EXPECT_EQ(report.exit_status, 0);
}

TEST(StartupReportTest, FailureCarriesStatusAndMessage) {
  StartupReport report = DecodeStartupReport(EncodeStartupReport(
      StartupReport{.ready = false,
                    .exit_status = 32,
                    .message = "Cache database /c.db is in use\nsecond line"}));
  EXPECT_FALSE(report.ready);
  EXPECT_EQ(report.exit_status, 32);
  EXPECT_EQ(report.message, "Cache database /c.db is in use\nsecond line");
}

// A daemon that died before reporting closes its end with nothing written.
TEST(StartupReportTest, NothingReceivedIsAFailure) {
  StartupReport report = DecodeStartupReport("");
  EXPECT_FALSE(report.ready);
  EXPECT_NE(report.exit_status, 0);
  EXPECT_THAT(report.message, HasSubstr("before it was ready"));
}

TEST(StartupReportTest, GarbageIsAFailureNotAReady) {
  StartupReport report = DecodeStartupReport("Z");
  EXPECT_FALSE(report.ready);
  EXPECT_NE(report.exit_status, 0);
}

// --- remount ---------------------------------------------------------------

constexpr char kMountinfo[] =
    "26 1 8:1 / / rw,relatime shared:1 - ext4 /dev/vda rw\n"
    "40 26 0:35 / /data rw,nosuid,nodev,relatime shared:9 - fuse.dcfs "
    "/dev/vdb rw,user_id=0\n"
    "41 26 0:36 / /plain rw,nosuid,nodev - fuse.sshfs h:/x rw\n"
    "42 26 0:37 / /with\\040space rw - fuse.dcfs UUID=aaaa rw\n";

TEST(IsDcfsMountTest, MatchesOnlyFuseDcfsAtThatMountpoint) {
  EXPECT_TRUE(IsDcfsMount(kMountinfo, "/data"));
  EXPECT_FALSE(IsDcfsMount(kMountinfo, "/plain"));
  EXPECT_FALSE(IsDcfsMount(kMountinfo, "/"));
  EXPECT_FALSE(IsDcfsMount(kMountinfo, "/nothing"));
  EXPECT_FALSE(IsDcfsMount(kMountinfo, "/dat"));
}

TEST(IsDcfsMountTest, DecodesEscapedMountpoints) {
  EXPECT_TRUE(IsDcfsMount(kMountinfo, "/with space"));
}

}  // namespace
}  // namespace dcfs
