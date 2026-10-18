// SPDX-License-Identifier: MIT
#ifndef DCFS_INTERRUPTION_POINT_H_
#define DCFS_INTERRUPTION_POINT_H_

// An interruption point (docs/design.md, "Cancellation", lists where they
// are; formal/dcfs.tla's Interrupt): OK, or, if the request being served
// was interrupted (Context::interrupts), ProtocolEvents::Interrupted and
// EINTR naming the point, which the request replies. Only just before a
// backing syscall, where stopping leaves the cache sound; never between a
// mutation's syscall and its phase 3.

#include <cstddef>
#include <string_view>

#include "absl/status/status.h"
#include "dcfs/context.h"

namespace dcfs {

// Where a request may stop for an interrupt, named for the log and the
// EINTR status: a string literal, never empty (both checked when it is
// compiled).
class InterruptionPoint {
 public:
  template <size_t N>
  explicit consteval InterruptionPoint(const char (&where)[N])
      : where_(where, N - 1) {
    static_assert(N > 1, "an interruption point needs a name");
  }

  constexpr std::string_view where() const { return where_; }

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const InterruptionPoint &point) {
    sink.Append(point.where_);
  }

 private:
  std::string_view where_;
};

absl::Status CheckInterrupted(Context &ctx, InterruptionPoint point);

}  // namespace dcfs

#endif  // DCFS_INTERRUPTION_POINT_H_
