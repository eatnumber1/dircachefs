// SPDX-License-Identifier: MIT
// An object removed while something still refers to it (a process whose
// working directory is removed, an O_PATH descriptor on a file that is then
// unlinked) behaves as it does on the backing filesystem, not with ESTALE:
// stat, open and list it, change it (truncate, chmod, chown, utimes, xattrs,
// fsync, through an open descriptor, an O_PATH descriptor's /proc/self/fd
// magic link or a removed working directory), reopen it through
// /proc/self/fd, and link it back. The kernel keeps such an object's nodeid
// until its last reference goes and meanwhile asks dcfs about it. The
// references are measured on the backing filesystem before dcfs mounts it;
// a scenario's answer is one line of text and dcfs's equals the backing's.
// The comments name the checks of the shell script this replaced.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
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
using ::testing::Not;

// guest/init made the mount point and the directory of the cache database.
constexpr std::string_view kMnt = "/mnt";

std::string Mnt(std::string_view name) { return absl::StrCat(kMnt, "/", name); }

std::string ErrText(const absl::Status &status) {
  return absl::StrCat("ERR ", ErrnoToErrorName(StatusToErrno(status)));
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
    absl::StrAppendFormat(
        &out, ",atime:%d,mtime:%d",
        absl::ToUnixNanos(absl::TimeFromTimespec(stat->st_atim)),
        absl::ToUnixNanos(absl::TimeFromTimespec(stat->st_mtim)));
  }
  return out;
}

std::array<struct timespec, 2> MutateTimes() {
  return {absl::ToTimespec(absl::FromUnixNanos(1'000'000'000'000'000'005)),
          absl::ToTimespec(absl::FromUnixNanos(1'000'000'000'000'000'007))};
}

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
                            : absl::StrCat("stat=", ErrText(st.status()));
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
  std::array<struct timespec, 2> times = MutateTimes();
  std::string out = "fd";
  out += Step("truncate", syscalls::ftruncate(*fd, 3));
  out += Step("chmod", syscalls::fchmod(*fd, 0600));
  out += Step("chown", syscalls::fchownat(*fd, "", 1000, 1000, AT_EMPTY_PATH));
  out += Step("utimens", syscalls::futimens(*fd, times.data()));
  out +=
      Step("setxattr", syscalls::fsetxattr(*fd, "user.dcfs", Bytes("v1"), 0));
  out += StepGetxattr(syscalls::fgetxattr(*fd, "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("removexattr", syscalls::fremovexattr(*fd, "user.dcfs"));
  out += StepGetxattr(syscalls::fgetxattr(*fd, "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("fsync", syscalls::fsync(*fd));
  out += StatText(syscalls::fstat(*fd), /*with_times=*/true);
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
  std::array<struct timespec, 2> times = MutateTimes();
  std::string out = "opath";
  out += Step("truncate", syscalls::truncate(proc, 4));
  out += Step("chmod", syscalls::fchmodat(AT_FDCWD, proc, 0640, 0));
  out += Step("chown", syscalls::fchownat(AT_FDCWD, proc, 1000, 1000, 0));
  out += Step("utimens", syscalls::utimensat(AT_FDCWD, proc, times.data(), 0));
  out +=
      Step("setxattr", syscalls::setxattr(proc, "user.dcfs", Bytes("v2"), 0));
  out += StepGetxattr(syscalls::getxattr(proc, "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("removexattr", syscalls::removexattr(proc, "user.dcfs"));
  absl::StatusOr<FileDescriptor> again = syscalls::open(proc, O_RDWR);
  out += Step("reopen", again.status());
  if (again.ok()) AppendMutateWrite(**again, out);
  // The write after utimensat set mtime to now: no times.
  out += StatText(syscalls::fstat(*fd), /*with_times=*/false);
  return out;
}

absl::StatusOr<std::string> RemovedCwdMutate(const std::string &dir) {
  char buf[64];
  RETURN_IF_ERROR(RemoveCwd(dir));
  std::array<struct timespec, 2> times = MutateTimes();
  std::string out = "cwd";
  out += Step("chmod", syscalls::fchmodat(AT_FDCWD, ".", 0700, 0));
  out += Step("chown", syscalls::fchownat(AT_FDCWD, ".", 1000, 1000, 0));
  out += Step("utimens", syscalls::utimensat(AT_FDCWD, ".", times.data(), 0));
  out += Step("setxattr", syscalls::setxattr(".", "user.dcfs", Bytes("v3"), 0));
  out += StepGetxattr(syscalls::getxattr(".", "user.dcfs", buf, sizeof(buf)),
                      std::string_view(buf, sizeof(buf)));
  out += Step("removexattr", syscalls::removexattr(".", "user.dcfs"));
  out += StatText(syscalls::stat("."), /*with_times=*/true);
  return out;
}

// Holds `path` (a file or a directory) with an O_PATH descriptor, removes it,
// and links it back as <path>.back through /proc/self/fd/<n>.
absl::StatusOr<std::string> RemovedLink(const std::string &path) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_PATH | O_NOFOLLOW));
  ASSIGN_OR_RETURN(struct stat st, syscalls::fstat(*fd));
  RETURN_IF_ERROR(S_ISDIR(st.st_mode) ? Rmdir(path) : syscalls::unlink(path));
  absl::Status link =
      syscalls::linkat(AT_FDCWD, ProcFd(*fd), AT_FDCWD,
                       absl::StrCat(path, ".back"), AT_SYMLINK_FOLLOW);
  return link.ok() ? "link=ok" : absl::StrCat("link=", ErrText(link));
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
  if (!link.ok()) return absl::StrCat("link=", ErrText(link));
  absl::StatusOr<struct stat> st = syscalls::stat(path);
  if (!st.ok()) return absl::StrCat("link=ok stat=", ErrText(st.status()));
  return absl::StrFormat("link=ok nlink=%d", st->st_nlink);
}

// Makes the kernel forget every inode and dentry it can.
absl::Status DropCaches() {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open("/proc/sys/vm/drop_caches", O_WRONLY));
  return syscalls::write(*fd, "2", 1).status();
}

// The suite is one mount: the references are measured on the backing
// filesystem first, then dcfs mounts it and each test does the same through
// the mount.
class RemovedTest : public ::testing::Test {
 protected:
  struct References {
    std::string cwd, file, ls, umut, omut, cmut, lfile, ldir, tlink;
  };

  struct State {
    std::string src;
    References ref;
    std::unique_ptr<DcfsMount> mount;
  };

  static void SetUpTestSuite() {
    state_ = new State;
    state_->src = absl::StrCat(TestTmp(), "/src");
    Mkdir(state_->src);

    Mkdir(Src("ref_cwd"));
    ASSERT_OK_AND_ASSIGN(state_->ref.cwd,
                         InChild([] { return RemovedCwd(Src("ref_cwd")); }));
    Touch(Src("ref_file"), "data\n");
    ASSERT_OK_AND_ASSIGN(state_->ref.file, InChild([] {
                           return OpathUnlinkStat(Src("ref_file"));
                         }));
    Mkdir(Src("ref_ls"));
    ASSERT_OK_AND_ASSIGN(state_->ref.ls, InChild([] {
                           return RemovedCwdListing(Src("ref_ls"));
                         }));
    ASSERT_OK_AND_ASSIGN(state_->ref.umut, InChild([] {
                           return UnlinkedMutate(Src("ref_umut"));
                         }));
    ASSERT_OK_AND_ASSIGN(state_->ref.omut, InChild([] {
                           return OpathUnlinkedMutate(Src("ref_omut"));
                         }));
    Mkdir(Src("ref_cmut"));
    ASSERT_OK_AND_ASSIGN(state_->ref.cmut, InChild([] {
                           return RemovedCwdMutate(Src("ref_cmut"));
                         }));
    // Linking a removed object back: ENOENT for a file, EPERM for a
    // directory (linkat(2): the object has no link count left to raise).
    Touch(Src("ref_lfile"), "data\n");
    ASSERT_OK_AND_ASSIGN(state_->ref.lfile,
                         InChild([] { return RemovedLink(Src("ref_lfile")); }));
    Mkdir(Src("ref_ldir"));
    ASSERT_OK_AND_ASSIGN(state_->ref.ldir,
                         InChild([] { return RemovedLink(Src("ref_ldir")); }));
    Mkdir(Src("ref_tdir"));
    ASSERT_OK_AND_ASSIGN(state_->ref.tlink, InChild([] {
                           return TmpfileLink(Src("ref_tdir"), "t");
                         }));
    syscalls::sync();

    ASSERT_OK_AND_ASSIGN(state_->mount,
                         DcfsMount::Start({.source = state_->src,
                                           .mountpoint = std::string(kMnt),
                                           .cache_db = "/cache/dcfs.db"}));
  }

  static void TearDownTestSuite() {
    if (state_->mount != nullptr) EXPECT_THAT(state_->mount->Unmount(), IsOk());
    delete state_;
  }

  // A path on the backing filesystem.
  static std::string Src(std::string_view name) {
    return absl::StrCat(state_->src, "/", name);
  }

  // A name that is gone for good once the references are gone.
  static void ExpectGone(std::string_view name) {
    EXPECT_THAT(syscalls::stat(Mnt(name)),
                StatusIs(absl::StatusCode::kNotFound))
        << name << ": still exists through dcfs";
    EXPECT_THAT(syscalls::stat(Src(name)),
                StatusIs(absl::StatusCode::kNotFound))
        << name << ": still exists on the backing filesystem";
  }

  static State *absl_nullable state_;
};

RemovedTest::State *absl_nullable RemovedTest::state_ = nullptr;

// removed-cwd
TEST_F(RemovedTest, ARemovedWorkingDirectoryAnswersAsTheBackingDoes) {
  Mkdir(Mnt("cwd"));
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwd(Mnt("cwd")); }));
  ExpectSameAsBacking(state_->ref.cwd, served);
  ExpectGone("cwd");
}

// removed-file-link
TEST_F(RemovedTest, LinkingARemovedFileBackAnswersAsTheBackingDoes) {
  Touch(Mnt("lfile"), "data\n");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedLink(Mnt("lfile")); }));
  ExpectSameAsBacking(state_->ref.lfile, served);
}

// removed-dir-link
TEST_F(RemovedTest, LinkingARemovedDirectoryBackAnswersAsTheBackingDoes) {
  Mkdir(Mnt("ldir"));
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedLink(Mnt("ldir")); }));
  ExpectSameAsBacking(state_->ref.ldir, served);
}

// closed-tmpfile-link
TEST_F(RemovedTest, ALinkedClosedTmpfileAnswersAsTheBackingDoes) {
  Mkdir(Mnt("tdir"));
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return TmpfileLink(Mnt("tdir"), "t"); }));
  ExpectSameAsBacking(state_->ref.tlink, served);
  ASSERT_OK_AND_ASSIGN(struct stat mnt_st, syscalls::stat(Mnt("tdir/t")));
  ASSERT_OK_AND_ASSIGN(struct stat src_st, syscalls::stat(Src("tdir/t")));
  EXPECT_EQ(mnt_st.st_ino, src_st.st_ino);
}

// unlinked-opath-file
TEST_F(RemovedTest, AnUnlinkedOpathFileAnswersAsTheBackingDoes) {
  Touch(Mnt("file"), "data\n");
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return OpathUnlinkStat(Mnt("file")); }));
  ExpectSameAsBacking(state_->ref.file, served);
  ExpectGone("file");
}

// removed-cwd-ls
TEST_F(RemovedTest, ListingARemovedWorkingDirectoryAnswersAsTheBackingDoes) {
  Mkdir(Mnt("ls"));
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwdListing(Mnt("ls")); }));
  ExpectSameAsBacking(state_->ref.ls, served);
  ExpectGone("ls");
}

// removed-listed-cwd: the directory was listed through dcfs first (its row
// and listing are cached), so the cached state cannot answer by accident.
TEST_F(RemovedTest, ARemovedListedWorkingDirectoryAnswersAsTheBackingDoes) {
  Mkdir(Mnt("listed"));
  Touch(Mnt("listed/x"));
  ASSERT_THAT(testonly::ListDirectory(Mnt("listed")), IsOk());
  ASSERT_THAT(syscalls::unlink(Mnt("listed/x")), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwd(Mnt("listed")); }));
  ExpectSameAsBacking(state_->ref.cwd, served);
  ExpectGone("listed");
}

// unlinked-statted-file: the file was stat'ed first (its attributes are
// cached).
TEST_F(RemovedTest, AnUnlinkedStattedFileAnswersAsTheBackingDoes) {
  Touch(Mnt("statted"), "data\n");
  ASSERT_THAT(syscalls::stat(Mnt("statted")), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return OpathUnlinkStat(Mnt("statted")); }));
  ExpectSameAsBacking(state_->ref.file, served);
  ExpectGone("statted");
}

// unlinked-open-mutate
TEST_F(RemovedTest, ChangingAnUnlinkedOpenFileAnswersAsTheBackingDoes) {
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return UnlinkedMutate(Mnt("umut")); }));
  ExpectSameAsBacking(state_->ref.umut, served);
  ExpectGone("umut");
}

// unlinked-open-mutate-statted: with the attributes cached first.
TEST_F(RemovedTest, ChangingAnUnlinkedStattedFileAnswersAsTheBackingDoes) {
  Touch(Mnt("umut2"), "x\n");
  ASSERT_THAT(syscalls::stat(Mnt("umut2")), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return UnlinkedMutate(Mnt("umut2")); }));
  ExpectSameAsBacking(state_->ref.umut, served);
  ExpectGone("umut2");
}

// unlinked-opath-mutate
TEST_F(RemovedTest, ChangingAnUnlinkedOpathFileAnswersAsTheBackingDoes) {
  ASSERT_OK_AND_ASSIGN(std::string served, InChild([] {
                         return OpathUnlinkedMutate(Mnt("omut"));
                       }));
  ExpectSameAsBacking(state_->ref.omut, served);
  ExpectGone("omut");
}

// removed-cwd-mutate
TEST_F(RemovedTest, ChangingARemovedWorkingDirectoryAnswersAsTheBackingDoes) {
  Mkdir(Mnt("cmut"));
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwdMutate(Mnt("cmut")); }));
  ExpectSameAsBacking(state_->ref.cmut, served);
  ExpectGone("cmut");
}

// removed-cwd-mutate-listed: with the directory listed first.
TEST_F(RemovedTest, ChangingARemovedListedWorkingDirectoryAnswersLikewise) {
  Mkdir(Mnt("cmut2"));
  ASSERT_THAT(testonly::ListDirectory(Mnt("cmut2")), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string served,
                       InChild([] { return RemovedCwdMutate(Mnt("cmut2")); }));
  ExpectSameAsBacking(state_->ref.cmut, served);
  ExpectGone("cmut2");
}

// still-serving: after the removals, a change through dcfs still reaches the
// backing filesystem.
TEST_F(RemovedTest, ATreeKeepsServingAfterTheRemovals) {
  Mkdir(Mnt("after"));
  EXPECT_THAT(syscalls::stat(Src("after")), IsOk());
}

// lookup-counts: dcfs counts the kernel's lookups to know when the last
// reference to a removed object is gone, and logs a FORGET of more lookups
// than it counted as an error. Walk the tree (lookups and readdirplus), make
// the kernel forget what it can, then ask for a name the daemon has not seen:
// its answer is served after the forgets, and the log is read after that.
TEST_F(RemovedTest, TheKernelNeverForgetsMoreLookupsThanDcfsCounted) {
  Mkdir(Mnt("tree"));
  Mkdir(Mnt("tree/a"));
  Mkdir(Mnt("tree/a/b"));
  Touch(Mnt("tree/a/f1"));
  Touch(Mnt("tree/a/b/f2"));
  ASSERT_THAT(syscalls::linkat(AT_FDCWD, Mnt("tree/a/f1"), AT_FDCWD,
                               Mnt("tree/a/b/f1link"), 0),
              IsOk());
  ASSERT_THAT(syscalls::symlinkat("f1", AT_FDCWD, Mnt("tree/a/sym")), IsOk());
  ASSERT_THAT(Snapshot(kMnt), IsOk());
  ASSERT_THAT(DropCaches(), IsOk());
  ASSERT_THAT(Snapshot(kMnt), IsOk());
  ASSERT_THAT(DropCaches(), IsOk());
  EXPECT_THAT(syscalls::stat(Mnt("never-seen")),
              StatusIs(absl::StatusCode::kNotFound));
  ASSERT_OK_AND_ASSIGN(std::string log, state_->mount->Log());
  EXPECT_THAT(log, Not(::testing::HasSubstr("lookups of nodeid")));
}

}  // namespace
}  // namespace dcfs
