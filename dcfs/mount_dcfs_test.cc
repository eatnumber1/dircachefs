// Step 15.1: the mount.dcfs wrapper's pure parts: the mount(8) helper
// command line, the split of its options into dcfs's and the underlying
// mount's, the exit statuses, the startup report the daemon sends the
// waiting wrapper, and the mountinfo check for a remount. The wrapper's
// behavior as a program is test/qemu/guest/mount_dcfs.sh.
#include "dcfs/mount_dcfs.h"

#include <sys/mount.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

ABSL_FLAG(double, test_interval_sec, 5, "stands in for a dcfs flag");

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
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

// Step 15.8: dcfs always mounts with allow_other (the kernel enforces the mode
// bits with default_permissions, so the option only lets other users reach the
// mount). Naming it is refused, as a usage error, so stale lines are noticed.
TEST(SplitHelperOptionsTest, AllowOtherIsRefusedAsRedundant) {
  for (const char *option : {"dcfs.allow_other", "dcfs.allow_other=0",
                             "dcfs.allow_other=1", "allow_other"}) {
    SCOPED_TRACE(option);
    absl::StatusOr<HelperOptions> split = SplitHelperOptions(Strings{option});
    EXPECT_THAT(split, StatusIs(absl::StatusCode::kInvalidArgument,
                                HasSubstr("always allows other users")));
    EXPECT_THAT(split, StatusIs(absl::StatusCode::kInvalidArgument,
                                HasSubstr("remove")));
    EXPECT_EQ(ExitStatusFor(split.status()), 1);
  }
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

// The underlying mount is made by mount(8) from the native options; a type
// of none makes none, and a remount cannot change it: say so rather than
// drop `ro` and mount read-write.
TEST(SplitHelperOptionsTest, NoneRefusesNativeOptionsNamingThem) {
  EXPECT_THAT(SplitHelperOptions(Strings{"ro", "noatime", "dcfs.fstype=none"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("ro"), HasSubstr("noatime"),
                             HasSubstr("dcfs.fstype=none"))));
  EXPECT_THAT(SplitHelperOptions(Strings{"dcfs.fstype=none", "dcfs.ro"}),
              IsOkAndHolds(Field(&HelperOptions::read_only, true)));
}

// libmount hands a helper rw or ro, and what fstab said (nofail, _netdev,
// noauto, defaults, user options, x-systemd.*): the none form takes those
// silently, so an fstab line for it and the README's example work.
TEST(SplitHelperOptionsTest, NoneAcceptsWhatLibmountAdds) {
  EXPECT_THAT(
      SplitHelperOptions(Strings{
          "rw", "defaults", "nofail", "_netdev", "noauto", "auto", "user",
          "users", "owner", "group", "nouser", "x-systemd.requires=/mnt/a",
          "dcfs.fstype=none", "dcfs.cache_db=/c.db"}),
      IsOkAndHolds(Field(&HelperOptions::backing,
                         HelperOptions::Backing::kNone)));
}

TEST(SplitHelperOptionsTest, NoneStillRefusesOthersByName) {
  EXPECT_THAT(SplitHelperOptions(Strings{"rw", "ro", "nofail", "noatime",
                                         "dcfs.fstype=none"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("ro, noatime"),
                             Not(HasSubstr("nofail")),
                             Not(HasSubstr("rw")))));
}

// libmount merges fstab's options into a remount, so a native ro there is
// what the line always had, not a request: ignored (main.cc logs a warning
// naming UnhonoredNativeOptions).
TEST(SplitHelperOptionsTest, RemountAcceptsNativeOptions) {
  absl::StatusOr<HelperOptions> options = SplitHelperOptions(
      Strings{"remount", "ro", "noatime", "nofail", "dcfs.fstype=none"});
  ASSERT_THAT(options, absl_testing::IsOk());
  EXPECT_THAT(UnhonoredNativeOptions(*options), ElementsAre("ro", "noatime"));
}

TEST(UnhonoredNativeOptionsTest, ANativeMountHonorsEverything) {
  absl::StatusOr<HelperOptions> options =
      SplitHelperOptions(Strings{"ro", "noatime", "dcfs.fstype=ext4"});
  ASSERT_THAT(options, absl_testing::IsOk());
  EXPECT_THAT(UnhonoredNativeOptions(*options), testing::IsEmpty());
}

TEST(SplitHelperOptionsTest, RemountIsRecognizedAndNotPassedOn) {
  EXPECT_THAT(SplitHelperOptions(Strings{"remount", "dcfs.ro", "noatime"}),
              IsOkAndHolds(AllOf(
                  Field(&HelperOptions::remount, true),
                  Field(&HelperOptions::native_options,
                        ElementsAre("noatime")))));
}

// --- exit statuses and the startup report ----------------------------------

// mount(8) takes a helper's status verbatim and 1 means "incorrect
// invocation or permissions": 1 for usage and non-root, 32 for a failed start.
TEST(ExitStatusTest, UsageAndPermissionRefusalsExitOne) {
  EXPECT_EQ(ExitStatusFor(MarkUsageError(absl::InvalidArgumentError("x"))), 1);
  EXPECT_EQ(ExitStatusFor(MarkUsageError(absl::PermissionDeniedError("x"))), 1);
  EXPECT_EQ(ExitStatusFor(ParseHelperArgs(Strings{"only-source"}).status()),
            1);
  EXPECT_EQ(ExitStatusFor(SplitHelperOptions(Strings{"dcfs.bogus"}).status()),
            1);
  EXPECT_EQ(ExitStatusFor(ApplyFlagOption("test_interval_sec", "soon")), 1);
}

// By origin, not by code: a failed start whose cause is an errno that maps to
// InvalidArgument, PermissionDenied or Unimplemented (EINVAL from mount(2),
// EACCES, ENOSYS from open_tree) is still a failed start.
TEST(ExitStatusTest, AFailedStartExits32WhateverItsCode) {
  EXPECT_EQ(ExitStatusFor(absl::FailedPreconditionError("x")), 32);
  EXPECT_EQ(ExitStatusFor(absl::InternalError("x")), 32);
  EXPECT_EQ(ExitStatusFor(absl::NotFoundError("x")), 32);
  EXPECT_EQ(ExitStatusFor(absl::InvalidArgumentError("EINVAL from mount")), 32);
  EXPECT_EQ(ExitStatusFor(absl::PermissionDeniedError("EACCES")), 32);
  EXPECT_EQ(ExitStatusFor(absl::UnimplementedError("ENOSYS")), 32);
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
  EXPECT_EQ(report.exit_status, 32);
  EXPECT_THAT(report.message, HasSubstr("before it was ready"));
}

TEST(StartupReportTest, GarbageIsAFailureNotAReady) {
  StartupReport report = DecodeStartupReport("Z");
  EXPECT_FALSE(report.ready);
  EXPECT_EQ(report.exit_status, 32);
}

// --- remount ---------------------------------------------------------------

constexpr char kMountinfo[] =
    "26 1 8:1 / / rw,relatime shared:1 - ext4 /dev/vda rw\n"
    "40 26 0:35 / /data rw,nosuid,nodev,relatime shared:9 - fuse.dcfs "
    "/dev/vdb rw,user_id=0\n"
    "41 26 0:36 / /plain rw,nosuid,nodev - fuse.sshfs h:/x rw\n"
    "42 26 0:37 / /with\\040space rw - fuse.dcfs UUID=aaaa rw\n";

bool IsDcfsMount(std::string_view mountpoint) {
  return RemountFlags(kMountinfo, mountpoint, false).has_value();
}

TEST(RemountFlagsTest, OnlyAFuseDcfsMountAtThatMountpointHasFlags) {
  EXPECT_TRUE(IsDcfsMount("/data"));
  EXPECT_FALSE(IsDcfsMount("/plain"));
  EXPECT_FALSE(IsDcfsMount("/"));
  EXPECT_FALSE(IsDcfsMount("/nothing"));
  EXPECT_FALSE(IsDcfsMount("/dat"));
}

TEST(RemountFlagsTest, DecodesEscapedMountpoints) {
  EXPECT_TRUE(IsDcfsMount("/with space"));
}

TEST(RemountFlagsTest, KeepsThePerMountFlagsAndTogglesReadOnly) {
  constexpr char kInfo[] =
      "40 26 0:35 / /data rw,nosuid,nodev,noatime shared:9 - fuse.dcfs "
      "/dev/vdb rw\n";
  std::optional<unsigned long> rw = RemountFlags(kInfo, "/data", false);
  ASSERT_TRUE(rw.has_value());
  EXPECT_EQ(*rw, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOATIME);
  std::optional<unsigned long> ro = RemountFlags(kInfo, "/data", true);
  ASSERT_TRUE(ro.has_value());
  EXPECT_EQ(*ro, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOATIME | MS_RDONLY);
}

TEST(RemountFlagsTest, NotADcfsMountHasNoFlags) {
  EXPECT_EQ(RemountFlags(kMountinfo, "/plain", true), std::nullopt);
  EXPECT_EQ(RemountFlags(kMountinfo, "/nothing", true), std::nullopt);
}

TEST(MountHelperNameTest, BothNamesMountHasIt) {
  EXPECT_TRUE(IsMountHelperName("mount.dcfs"));
  EXPECT_TRUE(IsMountHelperName("mount.fuse.dcfs"));
  EXPECT_FALSE(IsMountHelperName("dcfs"));
  EXPECT_FALSE(IsMountHelperName("fsck.dcfs"));
  EXPECT_FALSE(IsMountHelperName(""));
}

// The flag options are Abseil flags of the binary (a stand-in is defined
// here: the real ones are main.cc's).
TEST(ApplyFlagOptionTest, SetsAKnownFlagAndRefusesABadValue) {
  EXPECT_THAT(ApplyFlagOption("test_interval_sec", "7"),
              absl_testing::IsOk());
  EXPECT_EQ(absl::GetFlag(FLAGS_test_interval_sec), 7);
  EXPECT_THAT(ApplyFlagOption("test_interval_sec", "soon"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("dcfs.test_interval_sec=soon")));
  EXPECT_EQ(absl::GetFlag(FLAGS_test_interval_sec), 7);
}

TEST(ApplyFlagOptionTest, AnUnknownFlagIsAnInternalError) {
  EXPECT_THAT(ApplyFlagOption("no_such_flag", "1"),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("no_such_flag")));
}

TEST(ParseHelperArgsTest, SubtypeFlagTakesAnArgumentAndIsIgnored) {
  EXPECT_THAT(ParseHelperArgs(Strings{"s", "m", "-t", "dcfs"}),
              IsOkAndHolds(Field(&HelperArgs::source, "s")));
}

// Step 15.6b: umount.fuse.dcfs and umount.fuse, the helpers umount(8) runs.
TEST(UmountHelperNameTest, BothNamesLibmountMayLookFor) {
  EXPECT_TRUE(IsUmountHelperName("umount.fuse.dcfs"));
  EXPECT_TRUE(IsUmountHelperName("umount.fuse"));
  EXPECT_FALSE(IsUmountHelperName("umount.dcfs"));
  EXPECT_FALSE(IsUmountHelperName("mount.dcfs"));
  EXPECT_FALSE(IsUmountHelperName("umount"));
  EXPECT_FALSE(IsUmountHelperName(""));
}

TEST(ParseUmountArgsTest, TheTargetAndWhatTheInnerUmountGets) {
  EXPECT_THAT(ParseUmountArgs(Strings{"/data"}),
              IsOkAndHolds(AllOf(Field(&UmountArgs::target, "/data"),
                                 Field(&UmountArgs::lazy, false),
                                 Field(&UmountArgs::other_namespace, false),
                                 Field(&UmountArgs::forwarded, IsEmpty()))));
  // Everything but the target goes on, in order, arguments with their option.
  EXPECT_THAT(
      ParseUmountArgs(Strings{"-n", "/data", "-f", "-t", "fuse.dcfs", "-c",
                              "-Ofoo", "-q"}),
      IsOkAndHolds(
          AllOf(Field(&UmountArgs::target, "/data"),
                Field(&UmountArgs::forwarded,
                      ElementsAre("-n", "-f", "-t", "fuse.dcfs", "-c", "-Ofoo",
                                  "-q")))));
}

TEST(ParseUmountArgsTest, LazyAndNamespaceChangeWhatTheHelperDoes) {
  EXPECT_THAT(ParseUmountArgs(Strings{"-lf", "/data"}),
              IsOkAndHolds(AllOf(Field(&UmountArgs::lazy, true),
                                 Field(&UmountArgs::forwarded,
                                       ElementsAre("-lf")))));
  EXPECT_THAT(ParseUmountArgs(Strings{"--lazy", "/data"}),
              IsOkAndHolds(Field(&UmountArgs::lazy, true)));
  EXPECT_THAT(ParseUmountArgs(Strings{"-N", "/proc/1/ns/mnt", "/data"}),
              IsOkAndHolds(AllOf(
                  Field(&UmountArgs::other_namespace, true),
                  Field(&UmountArgs::target, "/data"),
                  Field(&UmountArgs::forwarded,
                        ElementsAre("-N", "/proc/1/ns/mnt")))));
  EXPECT_THAT(ParseUmountArgs(Strings{"--namespace", "123", "/data"}),
              IsOkAndHolds(AllOf(
                  Field(&UmountArgs::other_namespace, true),
                  Field(&UmountArgs::forwarded,
                        ElementsAre("--namespace", "123")))));
  EXPECT_THAT(ParseUmountArgs(Strings{"--namespace=123", "/data"}),
              IsOkAndHolds(Field(&UmountArgs::other_namespace, true)));
  // Long options with an argument in the next word keep it with them.
  EXPECT_THAT(ParseUmountArgs(Strings{"--types", "fuse.dcfs", "/data"}),
              IsOkAndHolds(AllOf(Field(&UmountArgs::target, "/data"),
                                 Field(&UmountArgs::forwarded,
                                       ElementsAre("--types", "fuse.dcfs")))));
  EXPECT_THAT(ParseUmountArgs(Strings{"--force", "/data"}),
              IsOkAndHolds(Field(&UmountArgs::forwarded,
                                 ElementsAre("--force"))));
}

TEST(ParseUmountArgsTest, VersionNeedsNoTarget) {
  EXPECT_THAT(ParseUmountArgs(Strings{"-V"}),
              IsOkAndHolds(Field(&UmountArgs::version, true)));
}

TEST(ParseUmountArgsTest, MistakesAreUsageErrors) {
  for (const Strings &bad :
       {Strings{}, Strings{"-f"}, Strings{"/data", "/other"}, Strings{"-t"},
        Strings{"/data", "-N"}}) {
    absl::StatusOr<UmountArgs> parsed = ParseUmountArgs(bad);
    ASSERT_FALSE(parsed.ok()) << bad.size();
    EXPECT_EQ(ExitStatusFor(parsed.status()), 1);
  }
}

TEST(TopmostMountEntryTest, DeviceAndTypeOfTheTopmostMount) {
  EXPECT_THAT(TopmostMountEntry(kMountinfo, "/plain"),
              Optional(AllOf(Field(&MountEntry::device, "0:36"),
                             Field(&MountEntry::fstype, "fuse.sshfs"))));
  EXPECT_THAT(TopmostMountEntry(kMountinfo, "/"),
              Optional(Field(&MountEntry::fstype, "ext4")));
  EXPECT_EQ(TopmostMountEntry(kMountinfo, "/nothing"), std::nullopt);
}

TEST(DcfsMountDeviceTest, TheDeviceOfTheTopmostDcfsMountAtThePath) {
  EXPECT_EQ(DcfsMountDevice(kMountinfo, "/data"), "0:35");
  EXPECT_EQ(DcfsMountDevice(kMountinfo, "/with space"), "0:37");
  EXPECT_EQ(DcfsMountDevice(kMountinfo, "/plain"), std::nullopt);
  EXPECT_EQ(DcfsMountDevice(kMountinfo, "/"), std::nullopt);
  EXPECT_EQ(DcfsMountDevice(kMountinfo, "/nothing"), std::nullopt);
  constexpr char kStacked[] =
      "40 26 0:35 / /data rw - fuse.dcfs /dev/vdb rw\n"
      "50 26 0:41 / /data rw - fuse.dcfs /dev/vdc rw\n";
  EXPECT_EQ(DcfsMountDevice(kStacked, "/data"), "0:41");
}

TEST(DaemonLockPathTest, OneFilePerFuseDevice) {
  EXPECT_EQ(DaemonLockPath("0:35"), "/run/dcfs/0_35.lock");
  EXPECT_EQ(DaemonLockPath("0:35", "/tmp/x"), "/tmp/x/0_35.lock");
}

}  // namespace
}  // namespace dcfs
