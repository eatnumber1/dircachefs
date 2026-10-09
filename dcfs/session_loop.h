#ifndef DCFS_SESSION_LOOP_H_
#define DCFS_SESSION_LOOP_H_

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)
#endif

// dcfs's FUSE session loop (main.cc), and the daemon's interruption source
// (dcfs/interrupts.h; docs/design.md, "Cancellation").
//
// libfuse's own loop (fuse_session_loop) reads the next message from
// /dev/fuse only once the current request's handler has returned, and the
// kernel queues a request's FUSE_INTERRUPT only after dcfs has read the
// request, so a single-threaded dcfs would never see an interrupt of the
// request it serves. This loop is libfuse's, plus Interrupted(): at a
// checkpoint it drains what /dev/fuse already holds without blocking. A
// FUSE_INTERRUPT goes to libfuse at once (fuse_session_process_buf: its
// do_interrupt marks the request it names, which fuse_req_interrupted then
// reports); the first other message ends the drain and is served after the
// current request, before the loop reads again (at most one read ahead per
// checkpoint). The kernel delivers interrupts ahead of forgets and
// requests (fuse_dev_do_read), so the drain finds a request's own. No
// thread, no wakeup while idle: draining happens only inside a request.

#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include "dcfs/interrupts.h"
#include "fuse_lowlevel.h"

namespace dcfs {

class SessionLoop final : public Interrupts {
 public:
  // Not owned; must outlive this.
  explicit SessionLoop(struct fuse_session *se);
  ~SessionLoop() override;

  // fuse_session_loop: serves requests until the session exits or the
  // device is gone; 0, -EPROTO after a refused FUSE_INIT (what libfuse's
  // private se->error says then), or a negative errno from reading the
  // device.
  int Run();

  // Called once, from Run, after the kernel's FUSE_INIT was answered and the
  // session is still going: dcfs is serving. mount.dcfs's wrapper, waiting
  // for that, is told here (dcfs/startup_channel.h).
  void SetOnInit(std::function<void()> on_init) {
    on_init_ = std::move(on_init);
  }

  void Begin(fuse_req *req) override;
  void End() override;
  // Drains /dev/fuse (see the top of this file), then asks libfuse whether
  // the request being served was interrupted.
  bool Interrupted() override;

 private:
  void Drain();

  struct fuse_session *se_;
  std::function<void()> on_init_;
  // The requests being served, innermost last (one at a time in the
  // blocking loop; Begin and End pair like a stack).
  std::vector<fuse_req *> serving_;
  // Messages drained at a checkpoint, to serve after the current request.
  // Each buffer's memory is libfuse's malloc (fuse_session_receive_buf).
  std::deque<struct fuse_buf> queued_;
};

}  // namespace dcfs

#endif  // DCFS_SESSION_LOOP_H_
