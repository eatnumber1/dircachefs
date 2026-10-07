#include "dcfs/session_loop.h"

#include <poll.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dcfs/interrupts.h"
#include "dcfs/syscalls.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"

namespace dcfs {
namespace {

// Whether `buf` (read into memory) is a FUSE_INTERRUPT.
bool IsInterrupt(const struct fuse_buf &buf) {
  if ((buf.flags & FUSE_BUF_IS_FD) != 0 ||
      buf.size < sizeof(struct fuse_in_header)) {
    return false;
  }
  struct fuse_in_header hdr {};
  std::memcpy(&hdr, buf.mem, sizeof(hdr));
  return hdr.opcode == FUSE_INTERRUPT;
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
    fuse_session_process_buf(se_, &fbuf);
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
    if (IsInterrupt(buf)) {
      // libfuse's do_interrupt: marks the request it names (or keeps it
      // for one not read yet), and replies nothing.
      fuse_session_process_buf(se_, &buf);
      std::free(buf.mem);
    } else if ((buf.flags & FUSE_BUF_IS_FD) != 0) {
      // A spliced message lives in libfuse's per-thread pipe, which the
      // next read reuses: it cannot wait. Never the case: dcfs does not
      // ask for FUSE_CAP_SPLICE_READ. Served now, nested.
      LOG(WARNING) << "checkpoint: serving a spliced message at once";
      fuse_session_process_buf(se_, &buf);
      std::free(buf.mem);
    } else {
      queued_.push_back(buf);
    }
  }
}

}  // namespace dcfs
