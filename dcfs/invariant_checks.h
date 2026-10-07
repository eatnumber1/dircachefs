#ifndef DCFS_INVARIANT_CHECKS_H_
#define DCFS_INVARIANT_CHECKS_H_

// The runtime invariant checks (docs/design.md, "Runtime invariant checks";
// step 26.2): hooks at the points where the review checklist's invariants
// must hold, which the testonly checking build turns into checks that abort
// the daemon naming the invariant and the request.
//
// Production code calls them through Context::checks, which is
// NoInvariantChecks() (every method does nothing) in every production
// binary: main.cc installs MainInvariantChecks(), which
// invariant_checks_main.cc defines as the no-op, and which the testonly
// checking build (//dcfs:main_static_checked) links from
// dcfs/testonly/main_invariant_checker.cc instead (link-time selection, as
// for MainProtocolEvents()). The forged-request harness
// (dcfs/dir_cache_fs_test.cc) installs the checker itself. What production
// pays is one call through a pointer to an empty function per hook: per
// backing syscall, per request and per FORGET entry.
//
// The hooks only observe: an implementation must not change the state it is
// shown (it may read the cache database).

#include <cstdint>
#include <string_view>

#include "absl/types/source_location.h"

namespace dcfs {

struct Context;
class DirCacheFS;
namespace events {
struct Request;
}  // namespace events

class InvariantChecks {
 public:
  InvariantChecks() = default;
  InvariantChecks(const InvariantChecks &) = delete;
  InvariantChecks &operator=(const InvariantChecks &) = delete;
  virtual ~InvariantChecks() = default;

  // A syscall that can reach the backing filesystem, or a call into code
  // without a Context that makes such syscalls, named `what`, is about to
  // be made. backing.cc calls it before each one it makes from a function
  // holding a Context, and DirCacheFS before each call of a backing::
  // helper that takes only a descriptor (backing::ReadFile, StatFd, ...).
  // Code without a Context cannot open a transaction (the cache database
  // is reachable only through Context::db), so checking here covers every
  // backing syscall: none can run inside a transaction. `site` is the call
  // site (the step 26.6 fault sweep fails each site's syscalls in turn).
  virtual void BackingCall(Context &ctx, std::string_view what,
                           absl::SourceLocation site) {}

  // fuse_ops.cc: a request was dispatched (after the periodic sync point,
  // before its handler), and its reply has been sent. Requests nest when
  // one runs inside another's backing syscall (the forged-request harness
  // does that today, coroutines will).
  virtual void RequestBegin(Context &ctx, const DirCacheFS &fs,
                            const events::Request &request) {}
  virtual void RequestEnd(Context &ctx, const DirCacheFS &fs,
                          const events::Request &request) {}

  // fuse_ops.cc, inside a FORGET's or BATCH_FORGET's frame: the kernel is
  // about to drop `nlookup` of its lookups of nodeid `ino` (before
  // DirCacheFS counts them down).
  virtual void Forgetting(Context &ctx, const DirCacheFS &fs, uint64_t ino,
                          uint64_t nlookup) {}

  // backing::StartRun has recovered the dirty set and started the run
  // (Startup's probe of the recovered rows has not run yet).
  virtual void RunStarting(Context &ctx) {}

  // backing::Startup is done: StartRun, InitRoot, StartupPurge and the
  // probe of the recovered rows (ProbeRecoveredRows), so the state is the
  // one the first request will see.
  virtual void RunStarted(Context &ctx) {}

  // fuse_ops.cc: DESTROY's DirCacheFS::Destroy has returned (the kernel
  // holds no nodeid any more).
  virtual void Destroyed(Context &ctx, const DirCacheFS &fs) {}
};

// The implementation every production Context uses: checks nothing.
// Stateless, so one object serves every Context (as NoProtocolEvents()).
inline InvariantChecks &NoInvariantChecks() {
  static InvariantChecks none;
  return none;
}

// The implementation main.cc installs: NoInvariantChecks() in production
// (invariant_checks_main.cc), the checker in the testonly checking build
// (dcfs/testonly/main_invariant_checker.cc). A binary links exactly one.
InvariantChecks &MainInvariantChecks();

}  // namespace dcfs

#endif  // DCFS_INVARIANT_CHECKS_H_
