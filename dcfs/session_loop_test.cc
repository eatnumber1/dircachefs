// Step 15.2: SessionLoop's FUSE_INIT callback, which tells the waiting
// mount.dcfs wrapper that dcfs is serving. A libfuse session with custom io
// (as fuse_request_channel_test.cc) whose read() hands out forged requests:
// the callback runs once, after the INIT reply was written, and never when
// the INIT is refused.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include "dcfs/session_loop.h"

#include <fcntl.h>
#include <sys/uio.h>

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls_backing.h"
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
  script->events.push_back("write");
  return total;
}

void FakeInit(void *userdata, struct fuse_conn_info *conn) {
  auto *script = static_cast<Script *>(userdata);
  script->events.push_back("init");
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

// Runs a SessionLoop over `script`; returns Run()'s result.
int RunLoop(Script &script, bool set_callback = true) {
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
  int rc;
  {
    SessionLoop loop(se);
    if (set_callback) {
      loop.SetOnInit([&script] { script.events.push_back("on_init"); });
    }
    rc = loop.Run();
  }
  fuse_session_destroy(se);
  return rc;
}

TEST(SessionLoopOnInitTest, RunsOnceAfterTheInitReplyWasWritten) {
  Script script;
  script.requests = {InitRequest()};
  EXPECT_EQ(RunLoop(script), 0);
  EXPECT_THAT(script.events, ElementsAre("init", "write", "on_init"));
}

TEST(SessionLoopOnInitTest, NeverRunsForARefusedInit) {
  Script script;
  script.refuse_init = true;
  script.requests = {InitRequest()};
  EXPECT_EQ(RunLoop(script), -EPROTO);
  EXPECT_THAT(script.events, ElementsAre("init", "write"));
}

TEST(SessionLoopOnInitTest, RunsAtMostOnce) {
  Script script;
  script.requests = {InitRequest(), InitRequest()};
  RunLoop(script);
  int calls = 0;
  for (const std::string &event : script.events) calls += event == "on_init";
  EXPECT_EQ(calls, 1);
}

TEST(SessionLoopOnInitTest, NoCallbackIsFine) {
  Script script;
  script.requests = {InitRequest()};
  EXPECT_EQ(RunLoop(script, /*set_callback=*/false), 0);
}

}  // namespace
}  // namespace dcfs
