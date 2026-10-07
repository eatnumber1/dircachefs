#ifndef DCFS_INTERRUPTS_H_
#define DCFS_INTERRUPTS_H_

// FUSE_INTERRUPT (docs/design.md, "Cancellation"): where a request being
// served learns that its caller was interrupted. The kernel sends
// FUSE_INTERRUPT for a request whose caller got a signal and keeps waiting
// for the reply (even a SIGKILLed caller waits for one: fs/fuse/dev.c,
// request_wait_answer), so a request dcfs abandons must still be replied:
// EINTR. dcfs serves one request at a time and looks only at checkpoints
// (dcfs/checkpoint.h), each just before a backing syscall; a syscall
// already blocked in the kernel (a disk spinning up, a hung network mount,
// syncfs) cannot be interrupted until the coroutine and io_uring rewrite.
//
// Production: SessionLoop (dcfs/session_loop.h), which drains /dev/fuse at
// a checkpoint and asks libfuse (fuse_req_interrupted). Tests: a fake.

struct fuse_req;

namespace dcfs {

class Interrupts {
 public:
  Interrupts() = default;
  Interrupts(const Interrupts &) = delete;
  Interrupts &operator=(const Interrupts &) = delete;
  virtual ~Interrupts() = default;

  // The FUSE request `req` begins and ends being served (fuse_ops.cc's
  // Serve). They nest only in the forged-request harness, which runs
  // requests inside other requests' syscalls.
  virtual void Begin(fuse_req *req) {}
  virtual void End() {}

  // Whether the request being served was interrupted. Only checkpoints
  // ask.
  virtual bool Interrupted() { return false; }
};

// Never interrupted: what a Context starts with. Stateless.
inline Interrupts &NoInterrupts() {
  static Interrupts none;
  return none;
}

}  // namespace dcfs

#endif  // DCFS_INTERRUPTS_H_
