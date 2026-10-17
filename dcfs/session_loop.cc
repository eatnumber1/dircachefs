// SPDX-License-Identifier: MIT
#include "dcfs/session_loop.h"

#include <poll.h>
#include <sys/poll.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/backing_door.h"
#include "dcfs/interrupts.h"
#include "dcfs/status.h"
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
  struct fuse_in_header hdr{};
  std::memcpy(&hdr, buf.mem, sizeof(hdr));
  return hdr.opcode;
}

}  // namespace

SessionLoop::SessionLoop(struct fuse_session *se) : se_(se) {}

SessionLoop::~SessionLoop() {
  for (struct fuse_buf &buf : queued_) std::free(buf.mem);
}

absl::Status SessionLoop::Run() {
  struct fuse_buf fbuf = {};
  absl::Cleanup free_fbuf = [&fbuf] { std::free(fbuf.mem); };
  while (true) {
    // The request just served may have poisoned the backing door: then no
    // other is served, even if the session has exited meanwhile.
    RETURN_IF_ERROR(BackingDoor());
    if (fuse_session_exited(se_) != 0) return absl::OkStatus();
    ASSIGN_OR_RETURN(bool device_open, ServeNext(fbuf));
    if (!device_open) return absl::OkStatus();
  }
}

absl::StatusOr<bool> SessionLoop::ServeNext(struct fuse_buf &fbuf) {
  if (!queued_.empty()) {
    struct fuse_buf next = queued_.front();
    queued_.pop_front();
    fuse_session_process_buf(se_, &next);
    std::free(next.mem);
    return true;
  }
  int res = fuse_session_receive_buf(se_, &fbuf);
  if (res == -EINTR) return true;
  if (res == 0) return false;
  if (res < 0) return ErrnoToStatus(-res, "fuse_session_receive_buf");
  const uint32_t opcode = OpcodeOf(fbuf);
  fuse_session_process_buf(se_, &fbuf);
  if (opcode != FUSE_INIT) return true;
  // A refused FUSE_INIT ends the session (libfuse's do_init sets its private
  // se->error to -EPROTO, which fuse_session_loop returns): the only way an
  // INIT ends it.
  if (fuse_session_exited(se_) != 0) {
    return ProducedErrnoToStatus(EPROTO, "The kernel's FUSE_INIT was refused");
  }
  if (on_init_) std::exchange(on_init_, nullptr)();
  return true;
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
  // most one request is read ahead per interruption point, and the rest stay
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
