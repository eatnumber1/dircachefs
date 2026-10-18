// SPDX-License-Identifier: MIT
// What the kernel and dcfs do for O_DIRECT and inotify, pinned.
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
#include <linux/magic.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/fixed_array.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
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
