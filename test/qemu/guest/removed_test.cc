// SPDX-License-Identifier: MIT
// dcfs step 6.3 (audit F10), ported from guest/removed.sh (step 25.34): an
// object that is removed while something still refers to it -- a process whose
// working directory is removed, an O_PATH descriptor on a file that is then
// unlinked -- must behave as it does on the backing filesystem, not fail with
// ESTALE. Step 23.2: that includes changing it (truncate, chmod, chown,
// utimes, xattrs, fsync, through an open descriptor, an O_PATH descriptor's
// /proc/self/fd magic link, or a removed working directory) and reopening an
// unlinked file through /proc/self/fd. Step 23.9: linking a removed object
// back.
//
// The kernel keeps such an object's FUSE nodeid until its last reference goes
// (then it sends FORGET), and keeps asking dcfs about it meanwhile (GETATTR
// for a stat, OPENDIR for an open of "."). The reference results are measured
// on the backing filesystem itself, before dcfs mounts it, so that dcfs's
// exclusive access is never violated. A scenario's answer is one line of text
// ("<step>=ok" or "<step>=<errno name>"); the test is that dcfs's equals the
// backing filesystem's.
//
// The checks keep the shell's names (removed-cwd is RemovedCwd, and so on).
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/strip.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test/qemu/guest/guest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

std::string ErrText(const absl::Status &status) {
  return "ERR " + ErrnoToErrorName(StatusToErrno(status));
}

// " <step>=ok" or " <step>=<errno name>" for a call that returned `status`.
std::string Step(std::string_view name, const absl::Status &status) {
  if (status.ok()) return absl::StrCat(" ", name, "=ok");
  return absl::StrCat(" ", name, "=", ErrnoToErrorName(StatusToErrno(status)));
}

std::string StepGetxattr(const absl::StatusOr<size_t> &n,
                         std::string_view value) {
  if (!n.ok()) return Step("getxattr", n.status());
  return absl::StrCat(" getxattr=", value.substr(0, *n));
}

// The attributes the mutate scenarios compare: what a local filesystem and
// dcfs must agree on (no ctime: it is the moment of the change).
std::string StatText(const absl::StatusOr<struct stat> &stat, bool with_times) {
  if (!stat.ok()) return Step("stat", stat.status());
  std::string out = absl::StrFormat(
      " stat=size:%d,mode:%o,uid:%u,gid:%u,nlink:%u", stat->st_size,
      stat->st_mode, stat->st_uid, stat->st_gid, stat->st_nlink);
  if (with_times) {
    absl::StrAppendFormat(&out, ",atime:%d.%09d,mtime:%d.%09d",
                          stat->st_atim.tv_sec, stat->st_atim.tv_nsec,
                          stat->st_mtim.tv_sec, stat->st_mtim.tv_nsec);
  }
  return out;
}

const struct timespec kMutateTimes[2] = {
    {.tv_sec = 1000000000, .tv_nsec = 5},
    {.tv_sec = 1000000000, .tv_nsec = 7},
};

std::span<const uint8_t> Bytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}

std::string ProcFd(int fd) { return absl::StrCat("/proc/self/fd/", fd); }

absl::Status Rmdir(const std::string &path) {
  return syscalls::unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

// chdir into `dir` and remove it: the process's working directory is removed.
absl::Status RemoveCwd(const std::string &dir) {
  RETURN_IF_ERROR(syscalls::chdir(dir));
  return Rmdir(dir);
}

std::string ReadBack(int fd) {
  char buf[64];
  absl::StatusOr<size_t> n = syscalls::pread(fd, buf, sizeof(buf), 0);
  if (!n.ok()) return absl::StrCat(" read=", ErrText(n.status()));
  return absl::StrCat(" read=", std::string_view(buf, *n));
}

// open(2)s `path` O_PATH, unlinks it, then fstat(2)s the descriptor.
absl::StatusOr<std::string> OpathUnlinkStat(const std::string &path) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_PATH | O_NOFOLLOW));
  RETURN_IF_ERROR(syscalls::unlink(path));
  absl::StatusOr<struct stat> st = syscalls::fstat(*fd);
  if (!st.ok()) return ErrText(st.status());
  return absl::StrFormat("nlink=%d size=%d", st->st_nlink, st->st_size);
}

// A process whose working directory was removed: stat("."), open(".") and one
// getdents64 on it.
absl::StatusOr<std::string> RemovedCwd(const std::string &dir) {
  RETURN_IF_ERROR(RemoveCwd(dir));
  absl::StatusOr<struct stat> st = syscalls::stat(".");
  std::string out = st.ok() ? absl::StrFormat("stat=nlink:%d", st->st_nlink)
                            : "stat=" + ErrText(st.status());
  absl::StatusOr<FileDescriptor> fd =
      syscalls::open(".", O_RDONLY | O_DIRECTORY);
  if (!fd.ok()) {
    return absl::StrCat(out, " open=", ErrText(fd.status()), " getdents=-");
  }
  char buf[4096];
  absl::StatusOr<ssize_t> n = syscalls::getdents64(**fd, buf, sizeof(buf));
  return absl::StrCat(
      out, " open=ok getdents=", n.ok() ? "ok" : ErrText(n.status()));
}

// What listing a directory that was removed under the process answers.
absl::StatusOr<std::string> RemovedCwdListing(const std::string &dir) {
  RETURN_IF_ERROR(RemoveCwd(dir));
  absl::StatusOr<std::vector<std::string>> names = testonly::ListDirectory(".");
  return names.ok() ? absl::StrJoin(*names, ",") : ErrText(names.status());
}

absl::StatusOr<std::string> UnlinkedMutate(const std::string &path) {
  char buf[64];
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644));
  RETURN_IF_ERROR(syscalls::write(*fd, "hello", 5).status());
  RETURN_IF_ERROR(syscalls::unlink(path));
  std::string out = "fd";
  out += Step("truncate", syscalls::ftruncate(*fd, 3));
  out += Step("chmod", syscalls::fchmod(*fd, 0600));
  out += Step("chown", syscalls::fchownat(*fd, "", 1000, 1000, AT_EMPTY_PATH));
  out += Step("utimens", syscalls::futimens(*fd, kMutateTimes));
  out +=
      Step("setxattr", syscalls::fsetxattr(*fd, "user.dcfs", Bytes("v1"), 0));
  out += StepGetxattr(syscalls::fgetxattr(*fd, "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("removexattr", syscalls::fremovexattr(*fd, "user.dcfs"));
  out += StepGetxattr(syscalls::fgetxattr(*fd, "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("fsync", syscalls::fsync(*fd));
  out += StatText(syscalls::fstat(*fd), true);
  absl::StatusOr<FileDescriptor> again = syscalls::open(ProcFd(*fd), O_RDONLY);
  out += Step("reopen", again.status());
  if (again.ok()) out += ReadBack(**again);
  return out;
}

void AppendMutateWrite(int fd, std::string &out) {
  out += Step("write", syscalls::pwrite(fd, "J", 1, 0).status());
  out += ReadBack(fd);
  out += Step("fsync", syscalls::fsync(fd));
}

absl::StatusOr<std::string> OpathUnlinkedMutate(const std::string &path) {
  char buf[64];
  {
    ASSIGN_OR_RETURN(FileDescriptor file,
                     syscalls::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644));
    RETURN_IF_ERROR(syscalls::write(*file, "hello", 5).status());
  }
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_PATH | O_NOFOLLOW));
  RETURN_IF_ERROR(syscalls::unlink(path));
  std::string proc = ProcFd(*fd);
  std::string out = "opath";
  out += Step("truncate", syscalls::truncate(proc, 4));
  out += Step("chmod", syscalls::fchmodat(AT_FDCWD, proc, 0640, 0));
  out += Step("chown", syscalls::fchownat(AT_FDCWD, proc, 1000, 1000, 0));
  out += Step("utimens", syscalls::utimensat(AT_FDCWD, proc, kMutateTimes, 0));
  out +=
      Step("setxattr", syscalls::setxattr(proc, "user.dcfs", Bytes("v2"), 0));
  out += StepGetxattr(syscalls::getxattr(proc, "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("removexattr", syscalls::removexattr(proc, "user.dcfs"));
  absl::StatusOr<FileDescriptor> again = syscalls::open(proc, O_RDWR);
  out += Step("reopen", again.status());
  if (again.ok()) AppendMutateWrite(**again, out);
  // The write after utimensat set mtime to now: no times.
  out += StatText(syscalls::fstat(*fd), false);
  return out;
}

absl::StatusOr<std::string> RemovedCwdMutate(const std::string &dir) {
  char buf[64];
  RETURN_IF_ERROR(RemoveCwd(dir));
  std::string out = "cwd";
  out += Step("chmod", syscalls::fchmodat(AT_FDCWD, ".", 0700, 0));
  out += Step("chown", syscalls::fchownat(AT_FDCWD, ".", 1000, 1000, 0));
  out += Step("utimens", syscalls::utimensat(AT_FDCWD, ".", kMutateTimes, 0));
  out += Step("setxattr", syscalls::setxattr(".", "user.dcfs", Bytes("v3"), 0));
  out += StepGetxattr(syscalls::getxattr(".", "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("removexattr", syscalls::removexattr(".", "user.dcfs"));
  out += StatText(syscalls::stat("."), true);
  return out;
}

// Holds `path` (a file or a directory) with an O_PATH descriptor, removes it,
// and links it back as <path>.back through /proc/self/fd/<n>.
absl::StatusOr<std::string> RemovedLink(const std::string &path) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_PATH | O_NOFOLLOW));
  ASSIGN_OR_RETURN(struct stat st, syscalls::fstat(*fd));
  RETURN_IF_ERROR(S_ISDIR(st.st_mode) ? Rmdir(path) : syscalls::unlink(path));
  absl::Status link = syscalls::linkat(AT_FDCWD, ProcFd(*fd), AT_FDCWD,
                                       path + ".back", AT_SYMLINK_FOLLOW);
  return link.ok() ? "link=ok" : "link=" + ErrText(link);
}

// Opens an O_TMPFILE file in `dir`, holds it with an O_PATH descriptor, closes
// the file, and links it as <dir>/<name> through the descriptor's magic link.
absl::StatusOr<std::string> TmpfileLink(const std::string &dir,
                                        const std::string &name) {
  ASSIGN_OR_RETURN(FileDescriptor file,
                   syscalls::open(dir, O_TMPFILE | O_RDWR, 0644));
  ASSIGN_OR_RETURN(FileDescriptor opath, syscalls::open(ProcFd(*file), O_PATH));
  RETURN_IF_ERROR(file.Close());
  std::string path = absl::StrCat(dir, "/", name);
  absl::Status link = syscalls::linkat(AT_FDCWD, ProcFd(*opath), AT_FDCWD, path,
                                       AT_SYMLINK_FOLLOW);
  if (!link.ok()) return "link=" + ErrText(link);
  absl::StatusOr<struct stat> st = syscalls::stat(path);
  if (!st.ok()) return "link=ok stat=" + ErrText(st.status());
  return absl::StrFormat("link=ok nlink=%d", st->st_nlink);
}

void Mkdir(const std::string &path) {
  ASSERT_THAT(syscalls::mkdir(path, 0755), IsOk());
}

void Touch(const std::string &path, const std::string &data = "") {
  EXPECT_THAT(WriteFile(path, data), IsOk());
}

// The suite is one mount: the references are measured on the backing
// filesystem first, then dcfs mounts it and each check does the same through
// the mount.
class RemovedTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    src_ = absl::StrCat(absl::StripSuffix(::testing::TempDir(), "/"), "/src");
    mnt_ = "/mnt";  // guest/init made it, and /cache
    Mkdir(src_);

    ref_ = new References;
    Mkdir(src_ + "/ref_cwd");
    ASSERT_OK_AND_ASSIGN(ref_->cwd,
                         InChild([] { return RemovedCwd(src_ + "/ref_cwd"); }));
    Touch(src_ + "/ref_file", "data\n");
    ASSERT_OK_AND_ASSIGN(ref_->file, InChild([] {
                           return OpathUnlinkStat(src_ + "/ref_file");
                         }));
    Mkdir(src_ + "/ref_ls");
    ASSERT_OK_AND_ASSIGN(
        ref_->ls, InChild([] { return RemovedCwdListing(src_ + "/ref_ls"); }));
    ASSERT_OK_AND_ASSIGN(
        ref_->umut, InChild([] { return UnlinkedMutate(src_ + "/ref_umut"); }));
    ASSERT_OK_AND_ASSIGN(ref_->omut, InChild([] {
                           return OpathUnlinkedMutate(src_ + "/ref_omut");
                         }));
    Mkdir(src_ + "/ref_cmut");
    ASSERT_OK_AND_ASSIGN(ref_->cmut, InChild([] {
                           return RemovedCwdMutate(src_ + "/ref_cmut");
                         }));
    // Step 23.9: linking a removed object back (ENOENT for a file, EPERM for
    // a directory, on every Linux filesystem).
    Touch(src_ + "/ref_lfile", "data\n");
    ASSERT_OK_AND_ASSIGN(
        ref_->lfile, InChild([] { return RemovedLink(src_ + "/ref_lfile"); }));
    Mkdir(src_ + "/ref_ldir");
    ASSERT_OK_AND_ASSIGN(
        ref_->ldir, InChild([] { return RemovedLink(src_ + "/ref_ldir"); }));
    Mkdir(src_ + "/ref_tdir");
    ASSERT_OK_AND_ASSIGN(ref_->tlink, InChild([] {
                           return TmpfileLink(src_ + "/ref_tdir", "t");
                         }));
    syscalls::sync();

    ASSERT_OK_AND_ASSIGN(mount_,
                         DcfsMount::Start({.source = src_,
                                           .mountpoint = mnt_,
                                           .cache_db = "/cache/dcfs.db"}));
  }

  static void TearDownTestSuite() {
    if (mount_ != nullptr) EXPECT_THAT(mount_->Unmount(), IsOk());
    mount_.reset();
    delete ref_;
  }

  struct References {
    std::string cwd, file, ls, umut, omut, cmut, lfile, ldir, tlink;
  };

  static std::string src_;
  static std::string mnt_;
  static References *ref_;
  static std::unique_ptr<DcfsMount> mount_;
};

std::string RemovedTest::src_;
std::string RemovedTest::mnt_;
RemovedTest::References *RemovedTest::ref_;
std::unique_ptr<DcfsMount> RemovedTest::mount_;

// A name that is gone for good once the references are gone.
void ExpectGone(const std::string &src, const std::string &mnt,
                const std::string &name) {
  EXPECT_THAT(syscalls::stat(mnt + "/" + name),
              StatusIs(absl::StatusCode::kNotFound))
      << "gone-" << name << ": still exists through dcfs";
  EXPECT_THAT(syscalls::stat(src + "/" + name),
              StatusIs(absl::StatusCode::kNotFound))
      << "gone-" << name << ": still exists on the backing filesystem";
}

TEST_F(RemovedTest, Mount) { ASSERT_NE(mount_, nullptr); }

TEST_F(RemovedTest, RemovedCwd) {
  Mkdir(mnt_ + "/cwd");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwd(mnt_ + "/cwd"); }));
  ExpectSameAsBacking("removed-cwd", ref_->cwd, served);
  ExpectGone(src_, mnt_, "cwd");
}

TEST_F(RemovedTest, RemovedFileLink) {
  Touch(mnt_ + "/lfile", "data\n");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedLink(mnt_ + "/lfile"); }));
  ExpectSameAsBacking("removed-file-link", ref_->lfile, served);
}

TEST_F(RemovedTest, RemovedDirLink) {
  Mkdir(mnt_ + "/ldir");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedLink(mnt_ + "/ldir"); }));
  ExpectSameAsBacking("removed-dir-link", ref_->ldir, served);
}

TEST_F(RemovedTest, ClosedTmpfileLink) {
  Mkdir(mnt_ + "/tdir");
  ASSERT_OK_AND_ASSIGN(std::string served, InChild([] {
                         return TmpfileLink(mnt_ + "/tdir", "t");
                       }));
  ExpectSameAsBacking("closed-tmpfile-link", ref_->tlink, served);
  ASSERT_OK_AND_ASSIGN(struct stat mnt_st, syscalls::stat(mnt_ + "/tdir/t"));
  ASSERT_OK_AND_ASSIGN(struct stat src_st, syscalls::stat(src_ + "/tdir/t"));
  EXPECT_EQ(mnt_st.st_ino, src_st.st_ino);
}

TEST_F(RemovedTest, UnlinkedOpathFile) {
  Touch(mnt_ + "/file", "data\n");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return OpathUnlinkStat(mnt_ + "/file"); }));
  ExpectSameAsBacking("unlinked-opath-file", ref_->file, served);
  ExpectGone(src_, mnt_, "file");
}

TEST_F(RemovedTest, RemovedCwdLs) {
  Mkdir(mnt_ + "/ls");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwdListing(mnt_ + "/ls"); }));
  ExpectSameAsBacking("removed-cwd-ls", ref_->ls, served);
  ExpectGone(src_, mnt_, "ls");
}

// A removed directory that was listed through dcfs first (its row and listing
// are cached), and a file that was stat'ed (its attributes are cached), so the
// cached state cannot answer by accident.
TEST_F(RemovedTest, RemovedListedCwd) {
  Mkdir(mnt_ + "/listed");
  Touch(mnt_ + "/listed/x");
  ASSERT_THAT(testonly::ListDirectory(mnt_ + "/listed"), IsOk());
  ASSERT_THAT(syscalls::unlink(mnt_ + "/listed/x"), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwd(mnt_ + "/listed"); }));
  ExpectSameAsBacking("removed-listed-cwd", ref_->cwd, served);
  ExpectGone(src_, mnt_, "listed");
}

TEST_F(RemovedTest, UnlinkedStattedFile) {
  Touch(mnt_ + "/statted", "data\n");
  ASSERT_THAT(syscalls::stat(mnt_ + "/statted"), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served, InChild([] {
                         return OpathUnlinkStat(mnt_ + "/statted");
                       }));
  ExpectSameAsBacking("unlinked-statted-file", ref_->file, served);
  ExpectGone(src_, mnt_, "statted");
}

// Step 23.2: changing them. Each also with its attributes cached first, so a
// cached value cannot answer by accident.
TEST_F(RemovedTest, UnlinkedOpenMutate) {
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return UnlinkedMutate(mnt_ + "/umut"); }));
  ExpectSameAsBacking("unlinked-open-mutate", ref_->umut, served);
  ExpectGone(src_, mnt_, "umut");
}

TEST_F(RemovedTest, UnlinkedOpenMutateStatted) {
  Touch(mnt_ + "/umut2", "x\n");
  ASSERT_THAT(syscalls::stat(mnt_ + "/umut2"), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return UnlinkedMutate(mnt_ + "/umut2"); }));
  ExpectSameAsBacking("unlinked-open-mutate-statted", ref_->umut, served);
  ExpectGone(src_, mnt_, "umut2");
}

TEST_F(RemovedTest, UnlinkedOpathMutate) {
  ASSERT_OK_AND_ASSIGN(std::string served, InChild([] {
                         return OpathUnlinkedMutate(mnt_ + "/omut");
                       }));
  ExpectSameAsBacking("unlinked-opath-mutate", ref_->omut, served);
  ExpectGone(src_, mnt_, "omut");
}

TEST_F(RemovedTest, RemovedCwdMutate) {
  Mkdir(mnt_ + "/cmut");
  ASSERT_OK_AND_ASSIGN(std::string served, InChild([] {
                         return RemovedCwdMutate(mnt_ + "/cmut");
                       }));
  ExpectSameAsBacking("removed-cwd-mutate", ref_->cmut, served);
  ExpectGone(src_, mnt_, "cmut");
}

TEST_F(RemovedTest, RemovedCwdMutateListed) {
  Mkdir(mnt_ + "/cmut2");
  ASSERT_THAT(testonly::ListDirectory(mnt_ + "/cmut2"), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served, InChild([] {
                         return RemovedCwdMutate(mnt_ + "/cmut2");
                       }));
  ExpectSameAsBacking("removed-cwd-mutate-listed", ref_->cmut, served);
  ExpectGone(src_, mnt_, "cmut2");
}

// dcfs keeps serving the rest of the tree once the removed objects are gone.
TEST_F(RemovedTest, StillServing) {
  Mkdir(mnt_ + "/after");
  EXPECT_THAT(syscalls::stat(src_ + "/after"), IsOk())
      << "still-serving: mkdir after the removals did not reach the backing "
         "filesystem";
}

// dcfs counts the kernel's lookups to know when the last reference to a
// removed object is gone; a FORGET of more lookups than it counted is logged
// as an error. Walk the tree (lookups and readdirplus), then make the kernel
// forget everything it can.
TEST_F(RemovedTest, LookupCounts) {
  std::string tree = mnt_ + "/tree";
  Mkdir(tree);
  Mkdir(tree + "/a");
  Mkdir(tree + "/a/b");
  Touch(tree + "/a/f1");
  Touch(tree + "/a/b/f2");
  ASSERT_THAT(syscalls::linkat(AT_FDCWD, tree + "/a/f1", AT_FDCWD,
                               tree + "/a/b/f1link", 0),
              IsOk());
  ASSERT_THAT(syscalls::symlinkat("f1", AT_FDCWD, tree + "/a/sym"), IsOk());
  ASSERT_THAT(Snapshot(mnt_), IsOk());
  ASSERT_THAT(WriteFile("/proc/sys/vm/drop_caches", "2"), IsOk());
  ASSERT_THAT(Snapshot(mnt_), IsOk());
  ASSERT_THAT(WriteFile("/proc/sys/vm/drop_caches", "2"), IsOk());
  absl::StatusOr<std::string> log = mount_->Log();
  ASSERT_THAT(log, IsOk());
  EXPECT_FALSE(absl::StrContains(*log, "lookups of nodeid"))
      << "lookup-counts: " << *log;
}

}  // namespace
}  // namespace dcfs
