// SPDX-License-Identifier: MIT
// Setuid, setgid and file capabilities through dcfs where the answer turns on
// capabilities rather than on ids: the daemon's own (a daemon without
// CAP_SYS_PTRACE, one in a child user namespace) and a caller's in its user
// namespace. Each case runs natively, with dcfs out of the loop, and through
// the mount; where they differ, the case pins the difference that
// docs/design.md, "Caller credentials", lists as a price. The cases
// credentials.sh's tables cannot express live here.

#include <fcntl.h>
#include <linux/capability.h>
#include <linux/prctl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test/qemu/guest/guest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::HasSubstr;

// guest/init made the mount point and the directory of the cache database.
constexpr std::string_view kMnt = "/mnt";

// The backing directory (on the test's disk) and, inside it, the directory
// dcfs never touches.
const std::string &Src() {
  static const absl::NoDestructor<std::string> kSrc(
      absl::StrCat(TestTmp(), "/src"));
  return *kSrc;
}
const std::string &Native() {
  static const absl::NoDestructor<std::string> kNative(
      absl::StrCat(Src(), "/native"));
  return *kNative;
}

std::string Path(std::string_view dir, std::string_view name) {
  return absl::StrCat(dir, "/", name);
}

// The users credentials.sh names: alice in group 2000, bob outside it.
Identity Alice() { return {.uid = 1000, .gid = 1000, .groups = {1000, 2000}}; }
Identity Bob() { return {.uid = 1001, .gid = 1001, .groups = {1001}}; }

// "ok", or the errno's name.
std::string Outcome(const absl::Status &status) {
  if (status.ok()) return "ok";
  return ErrnoToErrorName(StatusToErrno(status));
}

// A version 2 file capability granting CAP_NET_RAW (struct vfs_cap_data,
// include/uapi/linux/capability.h:63-69 (Linux v7.3-rc7)).
vfs_cap_data FileCapability() {
  vfs_cap_data capability{};
  capability.magic_etc = VFS_CAP_REVISION_2;
  capability.data[0].permitted = 1U << CAP_NET_RAW;
  return capability;
}

absl::Status SetFileCapability(std::string_view path) {
  vfs_cap_data capability = FileCapability();
  return syscalls::setxattr(
      path, "security.capability",
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&capability),
                               sizeof(capability)),
      0);
}

// `path`'s mode in octal, setuid and setgid included, and whether it carries
// FileCapability(): "4666 cap", or the errno of reading it ("4666 ENODATA").
std::string ModeAndCapability(std::string_view path) {
  absl::StatusOr<struct stat> st = syscalls::stat(path);
  if (!st.ok()) return Outcome(st.status());
  vfs_cap_data want = FileCapability();
  vfs_cap_data got{};
  absl::StatusOr<size_t> n =
      syscalls::getxattr(path, "security.capability", &got, sizeof(got));
  std::string capability;
  if (!n.ok()) {
    capability = Outcome(n.status());
  } else {
    capability = *n == sizeof(want) && std::memcmp(&got, &want, *n) == 0
                     ? "cap"
                     : "another capability";
  }
  return absl::StrFormat("%o %s", st->st_mode & 07777, capability);
}

// A file at `path` with contents, owned by `uid`:`gid`, of mode `mode`.
absl::Status MakeFile(std::string_view path, uid_t uid, gid_t gid,
                      mode_t mode) {
  RETURN_IF_ERROR(WriteFile(path, "test-data\n"));
  RETURN_IF_ERROR(syscalls::fchownat(AT_FDCWD, path, uid, gid, 0));
  return syscalls::chmod(path, mode);
}

// The caller appends a line to `path`: a write.
absl::Status Append(std::string_view path) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_WRONLY | O_APPEND));
  return syscalls::write(*fd, "x\n", 2).status();
}

// chown(path, -1, -1).
absl::Status ChownNothing(std::string_view path) {
  return syscalls::fchownat(AT_FDCWD, path, static_cast<uid_t>(-1),
                            static_cast<gid_t>(-1), 0);
}

// What DcfsMount::Start of `spec` says: its error, or "mounted" (and then it
// is unmounted).
absl::StatusOr<std::string> StartOutcome(MountSpec spec) {
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount =
      DcfsMount::Start(std::move(spec));
  if (!mount.ok()) return absl::StrCat(mount.status());
  RETURN_IF_ERROR((*mount)->Unmount());
  return std::string("mounted");
}

// A mount of an empty directory of its own, for a daemon the test starts
// beside the environment's.
absl::StatusOr<MountSpec> SpareMount(std::string_view name) {
  MountSpec spec{.source = absl::StrCat(TestTmp(), "/", name, "_src"),
                 .mountpoint = absl::StrCat(TestTmp(), "/", name, "_mnt"),
                 .cache_db = absl::StrCat("/cache/", name, ".db")};
  RETURN_IF_ERROR(Mkdir(spec.source));
  RETURN_IF_ERROR(Mkdir(spec.mountpoint));
  return spec;
}

// Writes `text` to the procfs file `path` in one write(2), as a user
// namespace's map must be written (user_namespaces(7)).
absl::Status WriteProc(std::string_view path, std::string_view text) {
  ASSIGN_OR_RETURN(FileDescriptor fd, syscalls::open(path, O_WRONLY));
  return syscalls::write(*fd, text.data(), text.size()).status();
}

// Reads a pid from `socket`, maps that process's user namespace (uid 0 alone,
// and the gids of `gid_map`), and tells it so with one byte.
absl::Status MapUserNamespace(int socket, std::string_view gid_map) {
  pid_t pid = 0;
  ASSIGN_OR_RETURN(size_t n, syscalls::read(socket, &pid, sizeof(pid)));
  if (n != sizeof(pid)) {
    return InternalErrorBuilder()
           << "The child sent no pid (" << n << " bytes)";
  }
  RETURN_IF_ERROR(WriteProc(absl::StrCat("/proc/", pid, "/uid_map"), "0 0 1"));
  RETURN_IF_ERROR(WriteProc(absl::StrCat("/proc/", pid, "/gid_map"), gid_map));
  return syscalls::write(socket, "m", 1).status();
}

// What `fn` returns, run in a child that is the root of a user namespace of
// its own, mapping uid 0 and the gids of `gid_map` to themselves, with no
// supplementary groups. It holds every capability in that namespace and none
// outside it. A map of more than the creator's own ids takes a writer with
// CAP_SETUID and CAP_SETGID over the parent namespace (user_namespaces(7)),
// so a thread of this process writes the maps once the child has unshared.
absl::StatusOr<std::string> InUserNamespace(
    std::string_view gid_map,
    absl::FunctionRef<absl::StatusOr<std::string>()> fn) {
  ASSIGN_OR_RETURN(auto ends, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  auto &[here, there] = ends;
  std::future<absl::Status> mapped =
      std::async(std::launch::async, MapUserNamespace, *here, gid_map);
  absl::StatusOr<std::string> result =
      InChild([&]() -> absl::StatusOr<std::string> {
        RETURN_IF_ERROR(syscalls::unshare(CLONE_NEWUSER));
        pid_t pid = syscalls::getpid();
        RETURN_IF_ERROR(syscalls::write(*there, &pid, sizeof(pid)).status());
        char ack = 0;
        ASSIGN_OR_RETURN(size_t n, syscalls::read(*there, &ack, 1));
        if (n != 1) {
          return InternalErrorBuilder() << "The user namespace was not mapped";
        }
        RETURN_IF_ERROR(syscalls::setgroups({}));
        return fn();
      });
  {
    // The mapper's read ends, if the child sent nothing, once no end of
    // `there` is left: the child's went with it.
    FileDescriptor closing = std::move(there);
  }
  RETURN_IF_ERROR(mapped.get());
  return result;
}

// bob's chown(path, -1, -1) of root's setuid file with security.capability,
// in a group bob is not in. Natively it fails with EPERM before the
// capability is touched: setuid's removal is a mode change (notify_change,
// fs/attr.c:499-504), which setattr_prepare refuses a non-owner (fs/attr.c:205)
// before it removes the capability (fs/attr.c:229). Through the mount
// fuse_setattr drops that mode change first (fs/fuse/dir.c:2340-2342), so
// setattr_prepare removes the capability with a REMOVEXATTR, and the SETATTR
// that follows fails with EPERM (all Linux v7.3-rc7): a price (README,
// "Setuid, setgid and file capabilities").
TEST(CallerCapabilitiesTest, ANonOwnersChownStripsTheCapabilityThenFails) {
  for (const std::string &path :
       {Path(Native(), "capchown"), Path(kMnt, "capchown")}) {
    ASSERT_THAT(MakeFile(path, 0, 2000, 04666), IsOk());
    ASSERT_THAT(SetFileCapability(path), IsOk());
  }
  auto chown_as_bob = [](std::string path) {
    absl::StatusOr<std::string> chowned = InChild(
        [&]() -> absl::StatusOr<std::string> {
          return Outcome(ChownNothing(path));
        },
        Bob());
    return absl::StrCat(chowned.ok() ? *chowned : Outcome(chowned.status()),
                        " ", ModeAndCapability(path));
  };
  EXPECT_EQ(chown_as_bob(Path(Native(), "capchown")), "EPERM 4666 cap");
  EXPECT_EQ(chown_as_bob(Path(kMnt, "capchown")), "EPERM 4666 ENODATA");
  EXPECT_EQ(ModeAndCapability(Path(Src(), "capchown")), "4666 ENODATA");
}

// A daemon without CAP_SYS_PTRACE (19), as in Docker's default capability
// set. A caller's groups come from its /proc status file, which anyone may
// read, so alice still reaches a directory through group 2000. Its
// /proc/<pid>/ns links need ptrace access (proc_ns_get_link,
// fs/proc/namespaces.c:63), which a reader without CAP_SYS_PTRACE lacks for a
// process holding a capability the reader does not (cap_ptrace_access_check,
// security/commoncap.c:164 (Linux v7.3-rc7)):
// root's write of a setuid file with security.capability then clears setuid
// where the native filesystem keeps it, the safe side.
TEST(CallerCapabilitiesTest, ADaemonWithoutCapSysPtraceReadsGroupsButNotCaps) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, SpareMount("noptrace"));
  std::string grp = Path(spec.source, "grp");
  ASSERT_THAT(Mkdir(grp), IsOk());
  ASSERT_THAT(syscalls::fchownat(AT_FDCWD, grp, 0, 2000, 0), IsOk());
  ASSERT_THAT(syscalls::chmod(grp, 0770), IsOk());
  std::string native = Path(Native(), "noptrace-capwrite");
  ASSERT_THAT(MakeFile(native, 0, 2000, 04666), IsOk());
  ASSERT_THAT(SetFileCapability(native), IsOk());
  ASSERT_THAT(Append(native), IsOk());
  // The daemon is this child's child, which the dropped bounding set denies
  // CAP_SYS_PTRACE; this child, the writer, keeps it.
  absl::StatusOr<std::string> served =
      InChild([&]() -> absl::StatusOr<std::string> {
        RETURN_IF_ERROR(syscalls::prctl<PR_CAPBSET_DROP>(CAP_SYS_PTRACE));
        ASSIGN_OR_RETURN(std::unique_ptr<DcfsMount> mount,
                         DcfsMount::Start(spec));
        std::string mkdir = Outcome(
            InChild(
                [&]() -> absl::StatusOr<std::string> {
                  RETURN_IF_ERROR(Mkdir(Path(spec.mountpoint, "grp/noptrace")));
                  return "";
                },
                Alice())
                .status());
        std::string capwrite = Path(spec.mountpoint, "noptrace-capwrite");
        RETURN_IF_ERROR(MakeFile(capwrite, 0, 2000, 04666));
        RETURN_IF_ERROR(SetFileCapability(capwrite));
        std::string write = Outcome(Append(capwrite));
        RETURN_IF_ERROR(mount->Unmount());
        return absl::StrCat("mkdir ", mkdir, ", write ", write);
      });
  EXPECT_THAT(served, IsOkAndHolds("mkdir ok, write ok"));
  ASSERT_OK_AND_ASSIGN(struct stat made,
                       syscalls::stat(Path(spec.source, "grp/noptrace")));
  EXPECT_EQ(absl::StrCat(made.st_uid, ":", made.st_gid), "1000:1000");
  EXPECT_EQ(ModeAndCapability(native), "4666 ENODATA");
  EXPECT_EQ(ModeAndCapability(Path(spec.source, "noptrace-capwrite")),
            "666 ENODATA");
}

// The root of a child user namespace passes the root check, but its
// capabilities, and those its callers' /proc entries show, count only in that
// namespace: mount.dcfs refuses to start (dcfs/main.cc's
// RequireInitUserNamespace).
TEST(CallerCapabilitiesTest, MountDcfsRefusesAChildUserNamespace) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, SpareMount("userns"));
  EXPECT_THAT(InUserNamespace("0 0 1", [&] { return StartOutcome(spec); }),
              IsOkAndHolds(HasSubstr("must run in the init user namespace")));
}

// A daemon without CAP_FSETID in its permitted set could not keep it for a
// truncate, fallocate, copy_file_range or fallback write (backing.cc's
// AsCallerKeepingCapFsetid) and would fail each with EPERM: mount.dcfs
// refuses to start instead.
TEST(CallerCapabilitiesTest, MountDcfsRefusesADaemonWithoutCapFsetid) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, SpareMount("nofsetid"));
  EXPECT_THAT(InChild([&]() -> absl::StatusOr<std::string> {
                RETURN_IF_ERROR(syscalls::prctl<PR_CAPBSET_DROP>(CAP_FSETID));
                return StartOutcome(spec);
              }),
              IsOkAndHolds(HasSubstr("needs CAP_FSETID")));
}

// The root of a user namespace that maps a file's owner (0) and group (2000),
// outside that group, holds CAP_FSETID there but not in the init namespace.
// Natively its truncate and write keep setgid (in_group_or_capable's
// capable_wrt_inode_uidgid, fs/attr.c:42 and fs/inode.c:3040 (Linux
// v7.3-rc7)) and lose setuid; through dcfs, which learns only that it lacks
// CAP_FSETID in the init namespace, they lose both: a price on the safe side
// (docs/design.md, "Caller credentials", the truncate's FATTR_KILL_SUIDGID).
TEST(CallerCapabilitiesTest, ANamespacedCapFsetidHolderLosesSetgidThroughDcfs) {
  for (std::string_view dir : {std::string_view(Native()), kMnt}) {
    ASSERT_THAT(MakeFile(Path(dir, "userns-truncate"), 0, 2000, 02666), IsOk());
    ASSERT_THAT(MakeFile(Path(dir, "userns-write"), 0, 2000, 06666), IsOk());
  }
  EXPECT_THAT(
      InUserNamespace(
          "0 0 1\n2000 2000 1\n",
          [&]() -> absl::StatusOr<std::string> {
            std::string done;
            for (std::string_view dir : {std::string_view(Native()), kMnt}) {
              absl::StrAppend(
                  &done,
                  Outcome(syscalls::truncate(Path(dir, "userns-truncate"), 4)),
                  " ", Outcome(Append(Path(dir, "userns-write"))), " ");
            }
            return done;
          }),
      IsOkAndHolds("ok ok ok ok "));
  for (std::string_view name : {"userns-truncate", "userns-write"}) {
    EXPECT_EQ(ModeAndCapability(Path(Native(), name)), "2666 ENODATA") << name;
    EXPECT_EQ(ModeAndCapability(Path(kMnt, name)), "666 ENODATA") << name;
    EXPECT_EQ(ModeAndCapability(Path(Src(), name)), "666 ENODATA") << name;
  }
}

class CallerCapabilitiesEnvironment : public ::testing::Environment {
 public:
  void SetUp() override { ASSERT_THAT(Prepare(), IsOk()); }

  void TearDown() override {
    // SetUp may have failed before it mounted.
    if (mount_ != nullptr) EXPECT_THAT(mount_->Unmount(), IsOk());
  }

 private:
  absl::Status Prepare() {
    RETURN_IF_ERROR(Mkdir(Src()));
    RETURN_IF_ERROR(Mkdir(Native()));
    ASSIGN_OR_RETURN(mount_, DcfsMount::Start({.source = Src(),
                                               .mountpoint = std::string(kMnt),
                                               .cache_db = "/cache/dcfs.db"}));
    return absl::OkStatus();
  }

  std::unique_ptr<DcfsMount> mount_;
};

::testing::Environment *const kEnvironment =
    ::testing::AddGlobalTestEnvironment(new CallerCapabilitiesEnvironment);

}  // namespace
}  // namespace dcfs
