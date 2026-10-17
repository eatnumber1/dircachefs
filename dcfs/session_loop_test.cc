// SPDX-License-Identifier: MIT
// Step 15.2: SessionLoop's FUSE_INIT callback, which tells the waiting
// mount.dcfs wrapper that dcfs is serving. A libfuse session with custom io
// (as fuse_request_channel_test.cc) whose read() hands out forged requests:
// the callback runs once, after the INIT reply was written, and never when
// the INIT is refused.

#include <sys/types.h>
#include <sys/uio.h>

#include "absl/status/statusor.h"
#include "fuse_opt.h"
#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <fcntl.h>

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/backing_door.h"
#include "dcfs/fd.h"
#include "dcfs/session_loop.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::ElementsAre;

// What the fake channel and the ops record, in order.
struct Script {
  std::deque<std::string> requests;  // handed out by read(), then EOF
  std::vector<std::string> events;
  bool refuse_init = false;
};

ssize_t FakeRead(int, void *buf, size_t size, void *userdata) {
  auto *script = static_cast<Script *>(userdata);
  // The end of the script: the device is gone, as after an unmount.
  if (script->requests.empty()) {
    errno = ENODEV;
    return -1;
  }
  std::string request = std::move(script->requests.front());
  script->requests.pop_front();
  EXPECT_LE(request.size(), size);
  std::memcpy(buf, request.data(), request.size());
  return static_cast<ssize_t>(request.size());
}

ssize_t FakeWritev(int, struct iovec *iov, int count, void *userdata) {
  auto *script = static_cast<Script *>(userdata);
  ssize_t total = 0;
  for (int i = 0; i < count; ++i) total += static_cast<ssize_t>(iov[i].iov_len);
  script->events.emplace_back("write");
  return total;
}

void FakeInit(void *userdata, struct fuse_conn_info *conn) {
  auto *script = static_cast<Script *>(userdata);
  script->events.emplace_back("init");
  // The way fuse_ops.cc's Init refuses: a wanted flag libfuse did not offer.
  if (script->refuse_init) {
    conn->want_ext |= std::bit_floor(~conn->capable_ext);
  }
}

template <typename T>
void Append(std::string &out, const T &value) {
  out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

std::string InitRequest() {
  struct fuse_in_header hdr = {};
  struct fuse_init_in in = {};
  in.major = FUSE_KERNEL_VERSION;
  in.minor = FUSE_KERNEL_MINOR_VERSION;
  hdr.opcode = FUSE_INIT;
  hdr.unique = 1;
  hdr.len = static_cast<uint32_t>(sizeof(hdr) + sizeof(in));
  std::string out;
  Append(out, hdr);
  Append(out, in);
  return out;
}

// What has happened when Run starts.
struct BeforeRun {
  // fuse_session_exit, as a SIGTERM's handler calls it.
  bool exited = false;
  // The backing door poisoned, as a request's failed credential restore does.
  bool poisoned = false;
};

// Runs a SessionLoop over `script`; returns Run()'s result.
absl::Status RunLoop(Script &script, bool set_callback = true,
                     BeforeRun before = {}) {
  struct fuse_lowlevel_ops ops = {};
  ops.init = FakeInit;
  char arg0[] = "session_loop_test";
  char *argv[] = {arg0};
  struct fuse_args args = FUSE_ARGS_INIT(1, argv);
  struct fuse_session *se = fuse_session_new(&args, &ops, sizeof(ops), &script);
  EXPECT_NE(se, nullptr);
  struct fuse_custom_io io = {};
  io.read = FakeRead;
  io.writev = FakeWritev;
  absl::StatusOr<FileDescriptor> dummy =
      syscalls::openat(AT_FDCWD, "/dev/null", O_RDWR);
  EXPECT_THAT(dummy, absl_testing::IsOk());
  EXPECT_EQ(
      fuse_session_custom_io(se, &io, sizeof(io), std::move(*dummy).Release()),
      0);
  if (before.exited) fuse_session_exit(se);
  if (before.poisoned) PoisonBacking();
  absl::Status served;
  {
    SessionLoop loop(se);
    if (set_callback) {
      loop.SetOnInit([&script] { script.events.emplace_back("on_init"); });
    }
    served = loop.Run();
  }
  fuse_session_destroy(se);
  return served;
}

TEST(SessionLoopOnInitTest, RunsOnceAfterTheInitReplyWasWritten) {
  Script script;
  script.requests = {InitRequest()};
  EXPECT_THAT(RunLoop(script), absl_testing::IsOk());
  EXPECT_THAT(script.events, ElementsAre("init", "write", "on_init"));
}

TEST(SessionLoopOnInitTest, NeverRunsForARefusedInit) {
  Script script;
  script.refuse_init = true;
  script.requests = {InitRequest()};
  EXPECT_THAT(GetErrnoFromStatus(RunLoop(script)),
              absl_testing::IsOkAndHolds(EPROTO));
  EXPECT_THAT(script.events, ElementsAre("init", "write"));
}

TEST(SessionLoopOnInitTest, RunsAtMostOnce) {
  Script script;
  script.requests = {InitRequest(), InitRequest()};
  RunLoop(script).IgnoreError();
  int calls = 0;
  for (const std::string &event : script.events) calls += event == "on_init";
  EXPECT_EQ(calls, 1);
}

TEST(SessionLoopOnInitTest, NoCallbackIsFine) {
  Script script;
  script.requests = {InitRequest()};
  EXPECT_THAT(RunLoop(script, /*set_callback=*/false), absl_testing::IsOk());
}

// The control for the next test: an exited session ends the loop, OK.
TEST(SessionLoopPoisonTest, AnExitedSessionEndsTheLoop) {
  Script script;
  script.requests = {InitRequest()};
  EXPECT_THAT(RunLoop(script, /*set_callback=*/true, {.exited = true}),
              absl_testing::IsOk());
  EXPECT_THAT(script.events, ::testing::IsEmpty());
}

// Runs the loop over an exited session with the backing door poisoned, and
// exits 0 if the loop returned the door's refusal and served nothing.
[[noreturn]] void RunPoisonedOverAnExitedSession() {
  Script script;
  script.requests = {InitRequest()};
  const absl::Status served = RunLoop(script, /*set_callback=*/true,
                                      {.exited = true, .poisoned = true});
  syscalls::_exit(served == BackingDoor() && script.events.empty() ? 0 : 1);
}

// A poisoned backing door ends the loop with its refusal, even when the
// session has exited meanwhile (a SIGTERM during the request that poisoned
// it): the daemon must not report a clean end. In a child: the door stays
// poisoned. (dir_cache_fs_poisoned_test poisons it through a request.)
TEST(SessionLoopPoisonTest, APoisonedDoorEndsTheLoopWithItsRefusal) {
  ASSERT_OK_AND_ASSIGN(pid_t child, syscalls::fork());
  if (child == 0) RunPoisonedOverAnExitedSession();
  int wait_status = -1;
  ASSERT_THAT(syscalls::waitpid(child, &wait_status, 0),
              absl_testing::IsOkAndHolds(child));
  EXPECT_EQ(wait_status, 0);
}

}  // namespace
}  // namespace dcfs
