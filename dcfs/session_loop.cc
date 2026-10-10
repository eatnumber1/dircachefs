#include "dcfs/session_loop.h"

#include <poll.h>
#include <sys/poll.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dcfs/interrupts.h"
#include "dcfs/syscalls.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"

namespace dcfs {
namespace {

// The opcode of `buf` (read into memory), or 0.
uint32_t OpcodeOf(const struct fuse_buf &buf) {
  if ((buf.flags & FUSE_BUF_IS_FD) != 0 ||
      buf.size < sizeof(struct fuse_in_header)) {
    return 0;
  }
  struct fuse_in_header hdr {};
  std::memcpy(&hdr, buf.mem, sizeof(hdr));
  return hdr.opcode;
}

}  // namespace

SessionLoop::SessionLoop(struct fuse_session *se) : se_(se) {}

SessionLoop::~SessionLoop() {
  for (struct fuse_buf &buf : queued_) std::free(buf.mem);
}

int SessionLoop::Run() {
  int res = 0;
  struct fuse_buf fbuf = {};
  while (fuse_session_exited(se_) == 0) {
    if (!queued_.empty()) {
      struct fuse_buf next = queued_.front();
      queued_.pop_front();
      fuse_session_process_buf(se_, &next);
      std::free(next.mem);
      continue;
    }
    res = fuse_session_receive_buf(se_, &fbuf);
    if (res == -EINTR) continue;
    if (res <= 0) break;
    const uint32_t opcode = OpcodeOf(fbuf);
    fuse_session_process_buf(se_, &fbuf);
    // A refused FUSE_INIT ends the session (libfuse's do_init sets its
    // private se->error to -EPROTO, which fuse_session_loop returns): the
    // only way an INIT ends it.
    if (opcode == FUSE_INIT && fuse_session_exited(se_) != 0) {
      std::free(fbuf.mem);
      return -EPROTO;
    }
    if (opcode == FUSE_INIT && on_init_) {
      std::exchange(on_init_, nullptr)();
    }
  }
  std::free(fbuf.mem);
  return res > 0 ? 0 : res;
}

void SessionLoop::Begin(fuse_req *req) { serving_.push_back(req); }

void SessionLoop::End() {
  if (!serving_.empty()) serving_.pop_back();
}

bool SessionLoop::Interrupted() {
  if (serving_.empty()) return false;
  Drain();
  return fuse_req_interrupted(serving_.back()) != 0;
}

void SessionLoop::Drain() {
  // The kernel hands out interrupts before forgets and requests
  // (fuse_dev_do_read), so the drain stops at the first other message: at
  // most one request is read ahead per checkpoint, and the rest stay
  // queued in the kernel, where an interrupt or a killed caller can still
  // take them back.
  const int fd = fuse_session_fd(se_);
  while (fuse_session_exited(se_) == 0) {
    absl::StatusOr<short> ready = syscalls::poll(fd, POLLIN, 0);
    if (!ready.ok()) {
      LOG(WARNING) << "checkpoint: " << ready.status();
      return;
    }
    if ((*ready & POLLIN) == 0) return;
    struct fuse_buf buf = {};
    const int res = fuse_session_receive_buf(se_, &buf);
    if (res <= 0) {  // -EAGAIN (nothing after all), -EINTR, the device gone
      std::free(buf.mem);
      return;
    }
    if (OpcodeOf(buf) == FUSE_INTERRUPT) {
      // libfuse's do_interrupt: marks the request it names (or keeps it
      // for one not read yet), and replies nothing.
      fuse_session_process_buf(se_, &buf);
      std::free(buf.mem);
      continue;
    }
    // Served after the current request (Run). Always in memory:
    // DirCacheFS::Init turns spliced reads off.
    queued_.push_back(buf);
    return;
  }
}

}  // namespace dcfs
