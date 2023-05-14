#include "dfs/io_uring.h"

#include <utility>

namespace dfs {

IoUring::IoUring(io_uring ring)
    : ring_(std::move(ring)) {}

IoUring::IoUring(IoUring &&o)
    : IoUring() {
  *this = std::move(o);
}

IoUring &IoUring::operator=(IoUring &&o) {
  ring_ = o.ring_;
  o.ring_ = std::nullopt;
  return *this;
}

IoUring::~IoUring() {
  if (ring_ != std::nullopt) {
    // TODO internally this can fail, but it's ignored. Fix
    io_uring_queue_exit(&(*ring_));
  }
}

absl::StatusOr<IoUring> IoUring::Create(Options opts) {
  if (opts.submission_queue_entries == 0) {
    return absl::InvalidArgumentError(
        "opts.submission_queue_entries must not be zero");
  }

  // TODO allow seting sq_thread_cpu and sq_thread_idle
  // TODO add support for copying IoUring via IORING_SETUP_ATTACH_WQ
  // TODO add multithreading support via multiple rings
  // https://github.com/axboe/liburing/issues/571#issuecomment-1106480309
  io_uring_params params{};

  if (opts.completion_queue_entries != 0) {
    params.cq_entries = opts.completion_queue_entries;
    params.flags = IORING_SETUP_CQSIZE;
  }

  io_uring ring;
  if (int err = io_uring_queue_init_params(opts.submission_queue_entries, &ring, &params);
      err != 0) {
    return absl::ErrnoToStatus(-err, "io_uring_queue_init_params");
  }
  return IoUring(std::move(ring));
}

}  // namespace dfs
