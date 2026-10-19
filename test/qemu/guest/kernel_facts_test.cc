// SPDX-License-Identifier: MIT
// What the kernel and dcfs do for O_DIRECT and inotify, and the kernel's rules
// for setuid, setgid, file capabilities and the user namespace that dcfs
// relies on, pinned.
//
// Each test states a fact about the kernel that dcfs relies on, or that its
// users meet, and fails when the fact stops being true, so that a kernel
// update which changes one is noticed here and not in the field. Kernel
// citations are file:line in Linux v7.3-rc7 (the guest runs Alpine's 6.18;
// the tests are the check that it agrees).
//
// Run in the guest as a qemu_cc_test (ext4, xfs and btrfs:
// test/qemu/BUILD.bazel "kernel_facts_test"). The environment below mounts dcfs
// on /mnt over a directory of the test's disk. Native() is a directory dcfs
// never touches: the "native" half of a differential test runs the same
// operations there, with dcfs out of the loop (docs/design.md, "Behave like the
// backing").
//
// Nothing here depends on timing. inotify queues an event in the syscall that
// causes it (fs/notify/inotify/inotify_fsnotify.c:126 fsnotify_add_event,
// called from the fsnotify call in the caller's context), so a queue read
// after the syscalls returned holds every event, in order. Page cache
// residency is read with mincore(2) on a mapping, which looks at the page
// cache without faulting a page in (mm/mincore.c:162 mincore_pte_range).

#include <fcntl.h>
#include <linux/capability.h>
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sched.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/fixed_array.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/types/optional_ref.h"
#include "dcfs/fd.h"
#include "dcfs/proc_status.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/capabilities.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test/qemu/guest/guest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::Contains;
using ::testing::ElementsAre;

constexpr std::string_view kMnt = "/mnt";

// The backing directory (on the test's disk, so on the file system under test)
// and, inside it, the directory dcfs never touches.
const std::string &Src() {
  static const absl::NoDestructor<std::string> src(
      absl::StrCat(TestTmp(), "/src"));
  return *src;
}
const std::string &Native() {
  static const absl::NoDestructor<std::string> native(
      absl::StrCat(Src(), "/native"));
  return *native;
}
constexpr size_t kPage = 4096;

std::string Path(std::string_view dir, std::string_view name) {
  return absl::StrCat(dir, "/", name);
}

// "-1 EINVAL"-style text for a syscall result.
template <typename T>
std::string Outcome(const absl::StatusOr<T> &result) {
  if (result.ok()) return absl::StrCat(*result);
  int err = StatusToErrno(result.status());
  if (err == EINVAL) return "-1 EINVAL";
  return absl::StrCat("-1 errno ", err);
}

// A page-aligned buffer holding a recognisable pattern, for O_DIRECT.
using AlignedBuffer = std::unique_ptr<char[], decltype(&std::free)>;

AlignedBuffer MakeAlignedBuffer(size_t size) {
  AlignedBuffer buf(static_cast<char *>(std::aligned_alloc(kPage, size)),
                    &std::free);
  for (size_t i = 0; i < size; ++i) buf[i] = static_cast<char>('a' + (i % 26));
  return buf;
}

// The number of pages of `path`'s first `length` bytes that are in its page
// cache. mincore(2) on a file mapping reports page cache residency and a
// mapping makes no page resident until it is touched, so nothing here changes
// the answer.
absl::StatusOr<size_t> ResidentPages(std::string_view path, size_t length) {
  ASSIGN_OR_RETURN(FileDescriptor fd, syscalls::open(path, O_RDONLY));
  ASSIGN_OR_RETURN(void *map, syscalls::mmap(nullptr, length, PROT_READ,
                                             MAP_SHARED, *fd, 0));
  absl::Cleanup unmap = [&] { syscalls::munmap(map, length).IgnoreError(); };
  absl::FixedArray<unsigned char> vec(length / kPage);
  RETURN_IF_ERROR(syscalls::mincore(map, length, vec.data()));
  size_t resident = 0;
  for (unsigned char page : vec) resident += page & 1;
  return resident;
}

// Creates a `size`-byte sparse file at `path` and opens it with `flags` (a
// sparse file has no data pages anywhere, so nothing is cached before the
// test's own I/O). An empty file is not truncated: ftruncate(2) raises
// IN_MODIFY even when it changes nothing (fs/open.c:68 do_truncate calls
// notify_change, whose fsnotify_change at fs/attr.c:561 turns ATTR_SIZE into
// IN_MODIFY: include/linux/fsnotify.h:484).
absl::StatusOr<FileDescriptor> CreateSparse(std::string_view path, int flags,
                                            size_t size) {
  ASSIGN_OR_RETURN(
      FileDescriptor fd,
      syscalls::open(path, O_CREAT | O_EXCL | O_RDWR | flags, 0600));
  if (size > 0) {
    RETURN_IF_ERROR(syscalls::ftruncate(*fd, static_cast<off_t>(size)));
  }
  return fd;
}

// ---------------------------------------------------------------------------
// O_DIRECT on a passthrough open
//
// The kernel opens a backing file of its own for every FUSE open, with the
// caller's open flags:
//   fs/fuse/passthrough.c:170   backing_file_open(file, file->f_flags, ...)
//   fs/backing-file.c:34-57     alloc_empty_backing_file(flags, ...) at :42,
//                               then vfs_open on the backing path
// so an O_DIRECT open of the FUSE file is an O_DIRECT open of the backing
// file. Every read and write then goes through vfs_iter_read/vfs_iter_write
// on it (fs/backing-file.c:173, 227), which builds its kiocb from that file's
// f_flags (init_sync_kiocb, include/linux/fs.h:2185) and so is direct. FUSE
// does not see the I/O at all: fuse_file_write_iter sends a passthrough file
// to fuse_passthrough_write_iter (fs/fuse/file.c:1862-1866).
// ---------------------------------------------------------------------------

// The steps of a direct I/O session on `dir`/`name`, as text, so that the same
// steps on the backing without dcfs and through the mount compare equal.
absl::StatusOr<std::vector<std::string>> DirectIoSession(
    std::string_view dir, std::string_view name) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(Path(dir, name),
                                  O_CREAT | O_EXCL | O_RDWR | O_DIRECT, 0600));
  AlignedBuffer out = MakeAlignedBuffer(2 * kPage);
  AlignedBuffer in = MakeAlignedBuffer(2 * kPage);
  std::memset(in.get(), 0, 2 * kPage);
  std::vector<std::string> steps;
  steps.push_back(
      absl::StrCat("pwrite 8192 at 0: ",
                   Outcome(syscalls::pwrite(*fd, out.get(), 2 * kPage, 0))));
  steps.push_back(
      absl::StrCat("pread 8192 at 0: ",
                   Outcome(syscalls::pread(*fd, in.get(), 2 * kPage, 0))));
  steps.push_back(absl::StrCat(
      "read back equals written: ",
      std::memcmp(in.get(), out.get(), 2 * kPage) == 0 ? "yes" : "no"));
  steps.push_back(
      absl::StrCat("pwrite 4096 at 1: ",
                   Outcome(syscalls::pwrite(*fd, out.get(), kPage, 1))));
  steps.push_back(absl::StrCat(
      "pwrite 100 at 0: ", Outcome(syscalls::pwrite(*fd, out.get(), 100, 0))));
  steps.push_back(
      absl::StrCat("pwrite 4096 from an unaligned buffer at 0: ",
                   Outcome(syscalls::pwrite(*fd, out.get() + 1, kPage, 0))));
  steps.push_back(absl::StrCat(
      "pread 100 at 0: ", Outcome(syscalls::pread(*fd, in.get(), 100, 0))));
  steps.push_back(absl::StrCat(
      "pread 4096 at 1: ", Outcome(syscalls::pread(*fd, in.get(), kPage, 1))));
  return steps;
}

TEST(ODirectPassthroughTest, BehavesLikeTheBacking) {
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> native,
                       DirectIoSession(Native(), "dio_match"));
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> mounted,
                       DirectIoSession(kMnt, "dio_match"));
  ASSERT_OK_AND_ASSIGN(struct statfs fs, syscalls::statfs(Src()));
  // The backing's own answers, so that agreement between the two cannot be
  // two failures alike. Aligned I/O works on all three file systems. An
  // offset, a length or a buffer that is not aligned is EINVAL on ext4 and
  // xfs (both use iomap: fs/iomap/direct-io.c:436 iomap_dio_bio_iter,
  // "(pos | length) & (alignment - 1)" -> -EINVAL; ext4's own
  // ext4_should_use_dio, fs/ext4/file.c:55, accepts every alignment for a
  // plain file because ext4_dio_alignment, fs/ext4/inode.c:6253, says 1), but
  // not on btrfs, which sees the unaligned request first and does it as a
  // buffered one (fs/btrfs/direct-io.c:940-943 check_direct_IO -> goto
  // buffered), so there it succeeds.
  if (static_cast<uint32_t>(fs.f_type) == BTRFS_SUPER_MAGIC) {
    EXPECT_THAT(native,
                ElementsAre("pwrite 8192 at 0: 8192", "pread 8192 at 0: 8192",
                            "read back equals written: yes",
                            "pwrite 4096 at 1: 4096", "pwrite 100 at 0: 100",
                            "pwrite 4096 from an unaligned buffer at 0: 4096",
                            "pread 100 at 0: 100", "pread 4096 at 1: 4096"));
  } else {
    EXPECT_THAT(
        native,
        ElementsAre("pwrite 8192 at 0: 8192", "pread 8192 at 0: 8192",
                    "read back equals written: yes",
                    "pwrite 4096 at 1: -1 EINVAL", "pwrite 100 at 0: -1 EINVAL",
                    "pwrite 4096 from an unaligned buffer at 0: -1 EINVAL",
                    "pread 100 at 0: -1 EINVAL", "pread 4096 at 1: -1 EINVAL"));
  }
  EXPECT_EQ(mounted, native);
}

TEST(ODirectPassthroughTest, WritesAndReadsBypassTheBackingPageCache) {
  // A file that was never read buffered has no page of it in the page cache.
  // Written with O_DIRECT through the mount, it has none after the write and
  // none after a direct read either: the kernel did the I/O on the backing
  // file with O_DIRECT (see above), not through the cache.
  constexpr size_t kSize = 16 * kPage;
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       CreateSparse(Path(kMnt, "dio_bypass"), O_DIRECT, kSize));
  AlignedBuffer buf = MakeAlignedBuffer(kSize);
  EXPECT_THAT(syscalls::pwrite(*fd, buf.get(), kSize, 0), IsOkAndHolds(kSize));
  EXPECT_THAT(ResidentPages(Path(Src(), "dio_bypass"), kSize),
              IsOkAndHolds(0u));
  EXPECT_THAT(syscalls::pread(*fd, buf.get(), kSize, 0), IsOkAndHolds(kSize));
  EXPECT_THAT(ResidentPages(Path(Src(), "dio_bypass"), kSize),
              IsOkAndHolds(0u));
}

TEST(ODirectPassthroughTest, TheProbeSeesABufferedWrite) {
  // The control for the test above: the same sequence without O_DIRECT leaves
  // every written page in the backing file's page cache, so a zero there is
  // the absence of pages and not a probe that cannot see them. (A passthrough
  // buffered write is a buffered write on the backing file:
  // fs/backing-file.c:227 vfs_iter_write.)
  constexpr size_t kSize = 16 * kPage;
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       CreateSparse(Path(kMnt, "dio_control"), 0, kSize));
  absl::FixedArray<char> data(kSize, 'x');
  EXPECT_THAT(syscalls::pwrite(*fd, data.data(), kSize, 0),
              IsOkAndHolds(kSize));
  EXPECT_THAT(ResidentPages(Path(Src(), "dio_control"), kSize),
              IsOkAndHolds(kSize / kPage));
}

// ---------------------------------------------------------------------------
// fcntl(F_SETFL) on a passthrough file
//
// F_SETFL changes the FUSE file's f_flags only: O_DIRECT is in SETFL_MASK
// (fs/fcntl.c:37) and setfl() assigns `filp->f_flags` (fs/fcntl.c:83) of the
// file the caller holds. The per-open backing file keeps the flags it was
// opened with (fs/fuse/passthrough.c:170), and what the backing file's I/O
// takes from the FUSE file's kiocb is BACKING_IOCB_MASK, which leaves out
// IOCB_DIRECT (fs/backing-file.c:94: IOCB_NOWAIT | IOCB_HIPRI | IOCB_DSYNC |
// IOCB_SYNC | IOCB_APPEND). So O_DIRECT cleared with F_SETFL does not make the
// backing I/O buffered. On the backing without dcfs, F_SETFL changes the file
// the I/O uses, and the I/O follows.
//
// This pins a kernel quirk, not a goal: dcfs would want the backing to follow
// the flag (docs/plan/notes/kernel-patches.md). The test fails the day
// upstream propagates the change; the pin then flips to "the mount equals
// native" and the comment with it.
// ---------------------------------------------------------------------------

// Opens `dir`/`name` with O_DIRECT, clears the flag with F_SETFL, writes 16
// aligned pages, and returns what the file's flags say and how many of the
// pages are in the page cache of the file on the backing (`backing_dir`/`name`,
// the same file when there is no dcfs).
absl::StatusOr<std::vector<std::string>> ClearODirect(
    std::string_view dir, std::string_view backing_dir, std::string_view name) {
  constexpr size_t kSize = 16 * kPage;
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   CreateSparse(Path(dir, name), O_DIRECT, kSize));
  std::vector<std::string> steps;
  ASSIGN_OR_RETURN(int flags, syscalls::fcntl(*fd, F_GETFL));
  steps.push_back(absl::StrCat("opened with O_DIRECT: ",
                               (flags & O_DIRECT) != 0 ? "yes" : "no"));
  steps.push_back(
      absl::StrCat("F_SETFL without O_DIRECT: ",
                   Outcome(syscalls::fcntl(*fd, F_SETFL, flags & ~O_DIRECT))));
  ASSIGN_OR_RETURN(int after, syscalls::fcntl(*fd, F_GETFL));
  steps.push_back(absl::StrCat("F_GETFL has O_DIRECT: ",
                               (after & O_DIRECT) != 0 ? "yes" : "no"));
  AlignedBuffer buf = MakeAlignedBuffer(kSize);
  steps.push_back(
      absl::StrCat("pwrite 16 pages: ",
                   Outcome(syscalls::pwrite(*fd, buf.get(), kSize, 0))));
  ASSIGN_OR_RETURN(size_t resident,
                   ResidentPages(Path(backing_dir, name), kSize));
  steps.push_back(
      absl::StrCat("pages of it in the backing's page cache: ", resident));
  return steps;
}

TEST(ODirectSetflTest, ClearingItLeavesTheBackingFileDirect) {
  // Without dcfs the write that follows the F_SETFL is buffered: all 16 pages
  // are in the cache. Through the mount the FUSE file says it is buffered and
  // the backing file is still direct: none are.
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> native,
                       ClearODirect(Native(), Native(), "setfl_clear"));
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> mounted,
                       ClearODirect(kMnt, Src(), "setfl_clear"));
  EXPECT_THAT(
      native,
      ElementsAre("opened with O_DIRECT: yes", "F_SETFL without O_DIRECT: 0",
                  "F_GETFL has O_DIRECT: no", "pwrite 16 pages: 65536",
                  "pages of it in the backing's page cache: 16"));
  EXPECT_THAT(
      mounted,
      ElementsAre("opened with O_DIRECT: yes", "F_SETFL without O_DIRECT: 0",
                  "F_GETFL has O_DIRECT: no", "pwrite 16 pages: 65536",
                  "pages of it in the backing's page cache: 0"));
}

// ---------------------------------------------------------------------------
// inotify
// ---------------------------------------------------------------------------

constexpr uint32_t kDirEvents =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO;

// One inotify instance. Events are read after the syscalls that cause them
// have returned: the kernel queues them in those syscalls.
class Notifier {
 public:
  struct Event {
    uint32_t mask;
    uint32_t cookie;
    std::string name;
  };

  static absl::StatusOr<Notifier> Create() {
    ASSIGN_OR_RETURN(FileDescriptor fd, syscalls::inotify_init1(IN_NONBLOCK));
    return Notifier(std::move(fd));
  }

  absl::Status Watch(std::string_view path, uint32_t mask) {
    return syscalls::inotify_add_watch(*fd_, path, mask).status();
  }

  // Everything queued so far, in order.
  absl::StatusOr<std::vector<Event>> Drain() {
    std::vector<Event> events;
    alignas(inotify_event) char buf[4096];
    for (;;) {
      absl::StatusOr<size_t> n = syscalls::read(*fd_, buf, sizeof(buf));
      if (!n.ok()) {
        if (StatusToErrno(n.status()) == EAGAIN) return events;
        return n.status();
      }
      for (size_t at = 0; at < *n;) {
        const auto *e = reinterpret_cast<const inotify_event *>(buf + at);
        events.push_back({.mask = e->mask,
                          .cookie = e->cookie,
                          .name = e->len != 0 ? e->name : ""});
        at += sizeof(inotify_event) + e->len;
      }
    }
  }

  // Drain(), as text: "create f", "create dir d", "modify f", ...
  absl::StatusOr<std::vector<std::string>> DrainText() {
    ASSIGN_OR_RETURN(std::vector<Event> events, Drain());
    return Text(events);
  }

  static std::vector<std::string> Text(const std::vector<Event> &events) {
    std::vector<std::string> text;
    text.reserve(events.size());
    for (const Event &e : events) text.push_back(Describe(e));
    return text;
  }

 private:
  explicit Notifier(FileDescriptor fd) : fd_(std::move(fd)) {}

  static std::string Describe(const Event &e) {
    struct Kind {
      uint32_t bit;
      const char *text;
    };
    static constexpr Kind kKinds[] = {
        {.bit = IN_CREATE, .text = "create"},
        {.bit = IN_DELETE, .text = "delete"},
        {.bit = IN_MODIFY, .text = "modify"},
        {.bit = IN_ATTRIB, .text = "attrib"},
        {.bit = IN_MOVED_FROM, .text = "moved_from"},
        {.bit = IN_MOVED_TO, .text = "moved_to"},
    };
    std::string what = absl::StrFormat("event 0x%x", e.mask);
    for (const Kind &kind : kKinds) {
      if ((e.mask & kind.bit) != 0) {
        what = kind.text;
        break;
      }
    }
    if ((e.mask & IN_ISDIR) != 0) what += " dir";
    return e.name.empty() ? what : absl::StrCat(what, " ", e.name);
  }

  FileDescriptor fd_;
};

// The operations whose events the first inotify test compares, in `dir`.
absl::Status ChangeTree(std::string_view dir) {
  std::string f = Path(dir, "f");
  std::string g = Path(dir, "g");
  std::string d = Path(dir, "d");
  {
    ASSIGN_OR_RETURN(FileDescriptor fd,
                     syscalls::open(f, O_CREAT | O_WRONLY, 0644));
    RETURN_IF_ERROR(syscalls::write(*fd, "hello", 5).status());
  }
  RETURN_IF_ERROR(syscalls::chmod(f, 0600));
  RETURN_IF_ERROR(syscalls::mkdir(d, 0755));
  RETURN_IF_ERROR(syscalls::rename(f, g));
  RETURN_IF_ERROR(syscalls::unlink(g));
  return syscalls::rmdir(d);
}

TEST(InotifyTest, SeesChangesMadeThroughTheMount) {
  // Every one of these goes through a vfs_* function that calls fsnotify
  // itself, above the file system, so a FUSE file system gets the events with
  // no code of its own: create (fs/namei.c:4602 fsnotify_create in the
  // atomic_open path FUSE takes; :4228 vfs_create), write (fs/read_write.c:691
  // vfs_write, fsnotify_modify), chmod (fs/attr.c:561 notify_change,
  // fsnotify_change), mkdir (fs/namei.c:5447 fsnotify_mkdir), rename
  // (:6263 and :6266 fsnotify_move, one per direction), unlink (:5690
  // fsnotify_unlink) and rmdir (:5557 d_delete_notify). The same operations
  // in /src/native, with dcfs out of the loop, give the events a user of the
  // backing gets, and the mount gives the same ones.
  ASSERT_OK_AND_ASSIGN(Notifier native, Notifier::Create());
  ASSERT_THAT(native.Watch(Native(), kDirEvents), IsOk());
  ASSERT_OK_AND_ASSIGN(Notifier mounted, Notifier::Create());
  ASSERT_THAT(mounted.Watch(kMnt, kDirEvents), IsOk());
  ASSERT_THAT(ChangeTree(Native()), IsOk());
  ASSERT_THAT(ChangeTree(kMnt), IsOk());

  ASSERT_OK_AND_ASSIGN(std::vector<Notifier::Event> native_events,
                       native.Drain());
  ASSERT_OK_AND_ASSIGN(std::vector<Notifier::Event> mounted_events,
                       mounted.Drain());
  EXPECT_THAT(
      Notifier::Text(native_events),
      ElementsAre("create f", "modify f", "attrib f", "create dir d",
                  "moved_from f", "moved_to g", "delete g", "delete dir d"));
  EXPECT_EQ(Notifier::Text(mounted_events), Notifier::Text(native_events));

  // The rename's two events carry one cookie, which pairs them.
  ASSERT_EQ(mounted_events.size(), 8u);
  EXPECT_NE(mounted_events[4].cookie, 0u);
  EXPECT_EQ(mounted_events[4].cookie, mounted_events[5].cookie);
}

TEST(InotifyTest, APassthroughWriteRaisesModifyOnTheMountAndOnTheBacking) {
  // One write(2) on a passthrough file raises IN_MODIFY twice, on two inodes:
  // vfs_write on the FUSE file (fs/read_write.c:691, fsnotify_modify(file))
  // and, inside the kernel's passthrough write, vfs_iter_write on the backing
  // file (fs/read_write.c:984, fsnotify_modify(file); called from
  // fs/backing-file.c:227). The backing file is not silenced for it: only the
  // permission events are off (fs/file_table.c:208 sets FMODE_NONOTIFY_PERM on
  // every file, backing ones included, and nothing there turns modify off).
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor created,
                         CreateSparse(Path(kMnt, "watched"), 0, 0));
  }
  ASSERT_OK_AND_ASSIGN(Notifier mount_watch, Notifier::Create());
  ASSERT_THAT(mount_watch.Watch(Path(kMnt, "watched"), IN_MODIFY), IsOk());
  ASSERT_OK_AND_ASSIGN(Notifier backing_watch, Notifier::Create());
  ASSERT_THAT(backing_watch.Watch(Path(Src(), "watched"), IN_MODIFY), IsOk());

  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       syscalls::open(Path(kMnt, "watched"), O_WRONLY));
  ASSERT_THAT(syscalls::pwrite(*fd, "hello", 5, 0), IsOkAndHolds(5u));

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> on_mount,
                       mount_watch.DrainText());
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> on_backing,
                       backing_watch.DrainText());
  EXPECT_THAT(on_mount, ElementsAre("modify"));
  EXPECT_THAT(on_backing, ElementsAre("modify"));
}

// Reads every entry of the directory `path` (and drops them): a listing the
// mount has to answer, as a user's would be.
absl::Status ListDirectory(std::string_view path) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_RDONLY | O_DIRECTORY));
  absl::FixedArray<char> entries(size_t{32} * 1024);
  for (;;) {
    ASSIGN_OR_RETURN(ssize_t n,
                     syscalls::getdents64(*fd, entries.data(), entries.size()));
    if (n == 0) return absl::OkStatus();
  }
}

TEST(InotifyTest, ABackingChangeRaisesNoEventOnTheMount) {
  // FUSE sends no event for a change that did not come through the mount:
  // fs/fuse has no fsnotify call (grep fsnotify fs/fuse finds one comment, at
  // fs/fuse/dir.c:2274), and the VFS raises events only in the syscalls that
  // make the change, which on the backing are the backing's. So an inotify
  // watch on a dcfs mount is blind to a change made directly on the backing
  // tree: README.md "Limitations", exclusive access to the backing tree.
  //
  // Ordering makes the check independent of timing: change the backing, have
  // the mount look at what changed (a stat and a listing, so that the absence
  // of an event is not the absence of any look), make a marker change through
  // the mount, then read the mount's queue. A queue holds events in the order
  // the syscalls queued them, and a syscall's events are queued before it
  // returns, so an event of the backing change would be ahead of the marker's,
  // and the queue would not be the marker alone. The backing's own watch is
  // the control: it saw the changes. The checking build of dcfs (the small
  // tier's) does not object to the change.
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor created,
                         CreateSparse(Path(kMnt, "oob"), 0, 0));
  }
  ASSERT_OK_AND_ASSIGN(Notifier mount_watch, Notifier::Create());
  ASSERT_THAT(mount_watch.Watch(kMnt, kDirEvents), IsOk());
  ASSERT_THAT(mount_watch.Watch(Path(kMnt, "oob"), kDirEvents), IsOk());
  ASSERT_OK_AND_ASSIGN(Notifier backing_watch, Notifier::Create());
  ASSERT_THAT(backing_watch.Watch(Src(), kDirEvents), IsOk());
  ASSERT_THAT(backing_watch.Watch(Path(Src(), "oob"), kDirEvents), IsOk());

  std::string oob = Path(Src(), "oob");
  std::string created = Path(Src(), "oob_created");
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor fd, syscalls::open(oob, O_WRONLY));
    ASSERT_THAT(syscalls::write(*fd, "x", 1), IsOkAndHolds(1u));
  }
  ASSERT_THAT(syscalls::chmod(oob, 0600), IsOk());
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor fd, CreateSparse(created, 0, 0));
  }
  ASSERT_THAT(syscalls::unlink(created), IsOk());

  ASSERT_THAT(syscalls::stat(Path(kMnt, "oob")), IsOk());
  ASSERT_THAT(ListDirectory(kMnt), IsOk());

  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor marker,
                         CreateSparse(Path(kMnt, "oob_marker"), 0, 0));
  }

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> on_mount,
                       mount_watch.DrainText());
  EXPECT_THAT(on_mount, ElementsAre("create oob_marker"));
  // The control: the backing's watch saw the backing change (the file's own
  // watch on `oob` and the directory's both name it) and the marker, whose
  // creation passed through dcfs to the backing.
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> backing,
                       backing_watch.DrainText());
  EXPECT_THAT(backing, Contains("modify oob"));
  EXPECT_THAT(backing, Contains("attrib oob"));
  EXPECT_THAT(backing, Contains("create oob_created"));
  EXPECT_THAT(backing, Contains("delete oob_created"));
  EXPECT_THAT(backing, Contains("create oob_marker"));
}

// ---------------------------------------------------------------------------
// Setuid, setgid and file capabilities, natively
//
// dcfs clears setuid and setgid itself where the backing filesystem's own
// call would (docs/design.md, "Caller credentials"), so it relies on the
// kernel's rules for them. Each case runs on Native(), dcfs out of the loop.
//   fs/attr.c:63-77        setattr_should_drop_suidgid: setuid always, setgid
//                          by setattr_should_drop_sgid (:33-45: with group
//                          execute, or for a caller neither in the group nor
//                          capable_wrt_inode_uidgid), and only for a caller
//                          without capable(CAP_FSETID)
//   fs/inode.c:2339-2356   dentry_needs_remove_privs: that, and
//                          ATTR_KILL_PRIV (security.capability) for anyone
//   fs/inode.c:2547        file_modified, which a write, a fallocate and a
//                          copy_file_range's remap call (mm/filemap.c:4468,
//                          fs/ext4/extents.c:4986, fs/remap_range.c:362; the
//                          splice fallback, fs/read_write.c:1598-1606, writes)
//   fs/open.c:57           do_truncate asks dentry_needs_remove_privs too
//   fs/open.c:816-818      chown_common: ATTR_KILL_SUID always, setgid by
//                          setattr_should_drop_sgid, and ATTR_KILL_PRIV
//   fs/attr.c:499-512      notify_change makes the kills an ATTR_MODE, which
//                          setattr_prepare allows the owner only (:205)
//   fs/posix_acl.c:516-559 __posix_acl_chmod_masq: a chmod rewrites the
//                          access ACL from the owner, group and other bits
// ---------------------------------------------------------------------------

// The users credentials.sh names: alice in group 2000, bob outside it.
Identity Alice() { return {.uid = 1000, .gid = 1000, .groups = {1000, 2000}}; }
Identity Bob() { return {.uid = 1001, .gid = 1001, .groups = {1001}}; }

// "ok", or the errno's name.
std::string Result(const absl::Status &status) {
  if (status.ok()) return "ok";
  return ErrnoToErrorName(StatusToErrno(status));
}

// `path`'s mode in octal, setuid and setgid included ("6666").
std::string Mode(std::string_view path) {
  absl::StatusOr<struct stat> st = syscalls::stat(path);
  if (!st.ok()) return Result(st.status());
  return absl::StrFormat("%o", st->st_mode & 07777);
}

// A file at `path` with contents, owned by `uid`:`gid`, of mode `mode`.
absl::Status MakeFile(std::string_view path, uid_t uid, gid_t gid,
                      mode_t mode) {
  RETURN_IF_ERROR(WriteFile(path, "test-data\n"));
  RETURN_IF_ERROR(syscalls::fchownat(AT_FDCWD, path, uid, gid, 0));
  return syscalls::chmod(path, mode);
}

// `fn`'s Result, run in a child as `as`, or as root with every capability.
std::string ResultAs(absl::FunctionRef<absl::Status()> fn,
                     absl::optional_ref<const Identity> as) {
  absl::StatusOr<std::string> result = InChild(
      [&]() -> absl::StatusOr<std::string> { return Result(fn()); }, as);
  return result.ok() ? *result : Result(result.status());
}

// `fn`'s Result, run in a child that is root without `cap` in its effective
// and permitted sets.
std::string ResultWithout(int cap, absl::FunctionRef<absl::Status()> fn) {
  return ResultAs(
      [&]() -> absl::Status {
        ASSIGN_OR_RETURN(syscalls::CapabilitySets sets, syscalls::capget());
        sets.effective = testonly::Without(sets.effective, cap);
        sets.permitted = testonly::Without(sets.permitted, cap);
        RETURN_IF_ERROR(syscalls::capset(sets));
        return fn();
      },
      std::nullopt);
}

// A version 2 file capability granting CAP_NET_RAW (struct vfs_cap_data,
// include/uapi/linux/capability.h:63-69).
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

// "cap" if `path` carries FileCapability(), else the errno of reading it.
std::string Capability(std::string_view path) {
  vfs_cap_data want = FileCapability();
  vfs_cap_data got{};
  absl::StatusOr<size_t> n =
      syscalls::getxattr(path, "security.capability", &got, sizeof(got));
  if (!n.ok()) return Result(n.status());
  if (*n == sizeof(want) && std::memcmp(&got, &want, *n) == 0) return "cap";
  return "another capability";
}

enum class Change { kWrite, kTruncate, kFallocate, kCopyFileRange };

// `change` made to `path` by the calling process: a write of two bytes, a
// truncate to 4, a fallocate of 8 KiB, or a copy_file_range of 4 bytes from
// Native()/src.
absl::Status Make(Change change, std::string_view path) {
  if (change == Change::kTruncate) return syscalls::truncate(path, 4);
  ASSIGN_OR_RETURN(FileDescriptor fd, syscalls::open(path, O_WRONLY));
  if (change == Change::kWrite) {
    return syscalls::pwrite(*fd, "x\n", 2, 0).status();
  }
  if (change == Change::kFallocate) return syscalls::fallocate(*fd, 0, 0, 8192);
  ASSIGN_OR_RETURN(FileDescriptor src,
                   syscalls::open(Path(Native(), "src"), O_RDONLY));
  return syscalls::copy_file_range(*src, 0, *fd, 0, 4, 0).status();
}

// A write, a truncate, a fallocate and a copy_file_range clear by one rule:
// setuid and, with group execute or outside the group, setgid, for a caller
// without CAP_FSETID (bob, outside group 2000; alice, in it); nothing for one
// with it (root). dcfs's SuidgidClearedByWrite is this rule, and its backing
// calls keep CAP_FSETID because the backing's own would apply it again.
TEST(SetidFactsTest, AChangeClearsSetidBitsByTheCallersCapFsetid) {
  ASSERT_THAT(MakeFile(Path(Native(), "src"), 0, 0, 0644), IsOk());
  struct Caller {
    std::string name;
    std::optional<Identity> as;
    std::string from_6666;  // what a change leaves of 6666
    std::string from_6676;  // and of 6676, with group execute
  };
  Caller callers[] = {
      {.name = "bob", .as = Bob(), .from_6666 = "666", .from_6676 = "676"},
      {.name = "alice", .as = Alice(), .from_6666 = "2666", .from_6676 = "676"},
      {.name = "root",
       .as = std::nullopt,
       .from_6666 = "6666",
       .from_6676 = "6676"}};
  std::pair<std::string_view, Change> changes[] = {
      {"write", Change::kWrite},
      {"truncate", Change::kTruncate},
      {"fallocate", Change::kFallocate},
      {"copy", Change::kCopyFileRange}};
  for (const Caller &caller : callers) {
    for (auto [name, change] : changes) {
      for (mode_t mode : {06666, 06676}) {
        std::string path =
            Path(Native(),
                 absl::StrFormat("setid-%s-%s-%o", caller.name, name, mode));
        ASSERT_THAT(MakeFile(path, 0, 2000, mode), IsOk());
        EXPECT_EQ(ResultAs([&] { return Make(change, path); }, caller.as), "ok")
            << path;
        EXPECT_EQ(Mode(path),
                  mode == 06666 ? caller.from_6666 : caller.from_6676)
            << path;
      }
    }
  }
}

// security.capability goes on a write by anyone, root included, whose
// CAP_FSETID keeps setuid (dentry_needs_remove_privs's ATTR_KILL_PRIV).
TEST(SetidFactsTest, AWriteRemovesAFileCapabilityWhoeverWrites) {
  std::pair<std::string, std::optional<Identity>> writers[] = {
      {"bob", Bob()}, {"root", std::nullopt}};
  for (const auto &[name, as] : writers) {
    std::string path = Path(Native(), absl::StrCat("capwrite-", name));
    ASSERT_THAT(MakeFile(path, 0, 2000, 04666), IsOk());
    ASSERT_THAT(SetFileCapability(path), IsOk());
    EXPECT_EQ(ResultAs([&] { return Make(Change::kWrite, path); }, as), "ok");
    EXPECT_EQ(Capability(path), "ENODATA") << name;
    EXPECT_EQ(Mode(path), as.has_value() ? "666" : "4666") << name;
  }
}

// chown(path, -1, -1) clears setuid whoever makes it, root with CAP_FSETID
// included, and setgid by setattr_should_drop_sgid: with group execute, or for
// a caller outside the group without CAP_FSETID.
TEST(SetidFactsTest, ARootChownOfNothingClearsSetuidAndSetgidByTheGroupRule) {
  auto chown_nothing = [](const std::string &path) {
    return [path] {
      return syscalls::fchownat(AT_FDCWD, path, static_cast<uid_t>(-1),
                                static_cast<gid_t>(-1), 0);
    };
  };
  std::string suid_sgid = Path(Native(), "chown-root-6666");
  std::string sgid_gx = Path(Native(), "chown-root-2676");
  std::string sgid_nofsetid = Path(Native(), "chown-nofsetid-2666");
  ASSERT_THAT(MakeFile(suid_sgid, 0, 2000, 06666), IsOk());
  ASSERT_THAT(MakeFile(sgid_gx, 0, 2000, 02676), IsOk());
  ASSERT_THAT(MakeFile(sgid_nofsetid, 0, 2000, 02666), IsOk());
  EXPECT_EQ(ResultAs(chown_nothing(suid_sgid), std::nullopt), "ok");
  EXPECT_EQ(ResultAs(chown_nothing(sgid_gx), std::nullopt), "ok");
  EXPECT_EQ(ResultWithout(CAP_FSETID, chown_nothing(sgid_nofsetid)), "ok");
  EXPECT_EQ(Mode(suid_sgid), "2666");
  EXPECT_EQ(Mode(sgid_gx), "676");
  EXPECT_EQ(Mode(sgid_nofsetid), "666");
}

// A non-owner's chown(path, -1, -1) of a setuid file, or of a setgid one
// outside its group, fails with EPERM and changes nothing: the setid removal
// is a mode change, which only the owner may make.
TEST(SetidFactsTest, ANonOwnersChownOfNothingOfASetidFileFails) {
  for (mode_t mode : {04666, 02666}) {
    std::string path = Path(Native(), absl::StrFormat("chown-bob-%o", mode));
    ASSERT_THAT(MakeFile(path, 0, 2000, mode), IsOk());
    EXPECT_EQ(ResultAs(
                  [&] {
                    return syscalls::fchownat(AT_FDCWD, path,
                                              static_cast<uid_t>(-1),
                                              static_cast<gid_t>(-1), 0);
                  },
                  Bob()),
              "EPERM")
        << path;
    EXPECT_EQ(Mode(path), absl::StrFormat("%o", mode)) << path;
  }
}

// A chmod that changes setuid and setgid alone leaves the access ACL as it
// was, byte for byte: dcfs's KILL_PRIV chmod does not mark it unknown.
TEST(SetidFactsTest, AChmodOfTheSetidBitsAloneLeavesTheAccessAclAsItWas) {
  std::string path = Path(Native(), "acl");
  ASSERT_THAT(MakeFile(path, 0, 2000, 0664), IsOk());
  // user::rw- user:1000:r-- group::r-- mask::rw- other::r--
  // (include/uapi/linux/posix_acl_xattr.h, posix_acl.h).
  struct {
    posix_acl_xattr_header header;
    posix_acl_xattr_entry entries[5];
  } acl = {.header = {.a_version = POSIX_ACL_XATTR_VERSION},
           .entries = {{.e_tag = ACL_USER_OBJ,
                        .e_perm = ACL_READ | ACL_WRITE,
                        .e_id = static_cast<uint32_t>(ACL_UNDEFINED_ID)},
                       {.e_tag = ACL_USER, .e_perm = ACL_READ, .e_id = 1000},
                       {.e_tag = ACL_GROUP_OBJ,
                        .e_perm = ACL_READ,
                        .e_id = static_cast<uint32_t>(ACL_UNDEFINED_ID)},
                       {.e_tag = ACL_MASK,
                        .e_perm = ACL_READ | ACL_WRITE,
                        .e_id = static_cast<uint32_t>(ACL_UNDEFINED_ID)},
                       {.e_tag = ACL_OTHER,
                        .e_perm = ACL_READ,
                        .e_id = static_cast<uint32_t>(ACL_UNDEFINED_ID)}}};
  ASSERT_THAT(syscalls::setxattr(
                  path, "system.posix_acl_access",
                  std::span<const uint8_t>(
                      reinterpret_cast<const uint8_t *>(&acl), sizeof(acl)),
                  0),
              IsOk());
  auto read_acl = [&]() -> absl::StatusOr<std::string> {
    std::string value(256, '\0');
    ASSIGN_OR_RETURN(size_t n,
                     syscalls::getxattr(path, "system.posix_acl_access",
                                        value.data(), value.size()));
    value.resize(n);
    return value;
  };
  ASSERT_OK_AND_ASSIGN(std::string before, read_acl());
  ASSERT_OK_AND_ASSIGN(struct stat st, syscalls::stat(path));
  ASSERT_THAT(syscalls::chmod(path, (st.st_mode & 07777) | S_ISUID | S_ISGID),
              IsOk());
  EXPECT_THAT(read_acl(), IsOkAndHolds(before));
  EXPECT_EQ(Mode(path), "6664");
}

// ---------------------------------------------------------------------------
// The user namespace and /proc
//
// mount.dcfs refuses a child user namespace by /proc/self/ns/user's inode
// (dcfs/proc_status.h's InInitUserNamespace, USER_NS_INIT_INO,
// include/uapi/linux/nsfs.h:50); NS_GET_PARENT cannot tell, since it fails
// with EPERM in every user namespace (fs/nsfs.c:235-238 calls ns_get_owner,
// kernel/user_namespace.c:1377-1393, which refuses any owner outside the
// caller's namespace, the init namespace's absent one included). dcfs reads a
// caller's groups from /proc/<tid>/task/<tid>/status, mode 0444 with no ptrace
// check (fs/proc/base.c:3650), and vouches for its CAP_FSETID only through
// /proc/<tid>/task/<tid>/ns/user, which proc_ns_get_link opens only with
// ptrace read access (fs/proc/namespaces.c:63-64; cap_ptrace_access_check,
// security/commoncap.c:164).
// ---------------------------------------------------------------------------

// `fn`'s Result, run in a child that has moved into a user namespace of its
// own (no ids mapped: none of these needs one).
std::string ResultInChildUserNamespace(absl::FunctionRef<absl::Status()> fn) {
  return ResultAs(
      [&]() -> absl::Status {
        RETURN_IF_ERROR(syscalls::unshare(CLONE_NEWUSER));
        return fn();
      },
      std::nullopt);
}

absl::Status NsGetParentOfMine() {
  ASSIGN_OR_RETURN(FileDescriptor user,
                   syscalls::open("/proc/self/ns/user", O_RDONLY));
  return syscalls::ioctl(*user, NS_GET_PARENT).status();
}

TEST(UserNamespaceFactsTest, NsGetParentFailsWithEpermInEveryUserNamespace) {
  EXPECT_EQ(Result(NsGetParentOfMine()), "EPERM");
  EXPECT_EQ(ResultInChildUserNamespace([] { return NsGetParentOfMine(); }),
            "EPERM");
}

TEST(UserNamespaceFactsTest, OnlyTheInitUserNamespaceHasUserNsInitIno) {
  EXPECT_THAT(InInitUserNamespace(), IsOkAndHolds(true));
  EXPECT_EQ(ResultInChildUserNamespace([]() -> absl::Status {
              ASSIGN_OR_RETURN(bool init, InInitUserNamespace());
              if (!init) return absl::OkStatus();
              return InternalErrorBuilder()
                     << "A child user namespace has USER_NS_INIT_INO";
            }),
            "ok");
}

// bob reads root's status file, but follows neither root's ns/user link nor
// that of this process, root with every capability, from a root without
// CAP_SYS_PTRACE.
TEST(ProcFactsTest, AStatusFileIsAnyonesButANamespaceLinkNeedsPtrace) {
  std::string status = absl::StrCat("/proc/", syscalls::getpid(), "/task/",
                                    syscalls::getpid(), "/status");
  std::string ns = absl::StrCat("/proc/", syscalls::getpid(), "/task/",
                                syscalls::getpid(), "/ns/user");
  EXPECT_EQ(
      ResultAs([&] { return testonly::ReadFileToString(status).status(); },
               Bob()),
      "ok");
  EXPECT_EQ(ResultAs([&] { return syscalls::stat(ns).status(); }, Bob()),
            "EACCES");
  EXPECT_EQ(ResultWithout(CAP_SYS_PTRACE,
                          [&] { return syscalls::stat(ns).status(); }),
            "EACCES");
}

// The mount, and the check that the guest never ran short of memory: the
// residency checks read the backing's page cache, which reclaim would empty
// (the cc-test form of lib.sh's require_no_reclaim; /proc/vmstat counts pages
// scanned by kswapd and by direct reclaim).
class FactsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override { ASSERT_THAT(Prepare(), IsOk()); }

  void TearDown() override {
    // SetUp may have failed before it mounted.
    if (mount_ != nullptr) EXPECT_THAT(mount_->Unmount(), IsOk());
    ASSERT_OK_AND_ASSIGN(uint64_t reclaim_after, PagesScanned());
    EXPECT_EQ(reclaim_after, reclaim_before_)
        << "the kernel reclaimed memory during the run: cached pages were "
           "evicted; raise this test's mem=";
  }

 private:
  absl::Status Prepare() {
    ASSIGN_OR_RETURN(reclaim_before_, PagesScanned());
    RETURN_IF_ERROR(Mkdir(Src()));
    RETURN_IF_ERROR(Mkdir(Native()));
    ASSIGN_OR_RETURN(mount_, DcfsMount::Start({.source = Src(),
                                               .mountpoint = std::string(kMnt),
                                               .cache_db = "/cache/dcfs.db"}));
    return absl::OkStatus();
  }

  static absl::StatusOr<uint64_t> PagesScanned() {
    ASSIGN_OR_RETURN(std::string vmstat,
                     testonly::ReadFileToString("/proc/vmstat"));
    uint64_t scanned = 0;
    for (std::string_view line : absl::StrSplit(vmstat, '\n')) {
      std::vector<std::string_view> kv = absl::StrSplit(line, ' ');
      uint64_t n;
      if (kv.size() == 2 &&
          (kv[0] == "pgscan_kswapd" || kv[0] == "pgscan_direct") &&
          absl::SimpleAtoi(kv[1], &n)) {
        scanned += n;
      }
    }
    return scanned;
  }

  std::unique_ptr<DcfsMount> mount_;
  uint64_t reclaim_before_ = 0;
};

::testing::Environment *const kEnvironment =
    ::testing::AddGlobalTestEnvironment(new FactsEnvironment);

}  // namespace
}  // namespace dcfs
