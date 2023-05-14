#ifndef DFS_IO_URING_H_
#define DFS_IO_URING_H_

#include <cstdint>
#include <optional>

#include "liburing.h"
#include "absl/status/statusor.h"

namespace dfs {

class IoUring {
 public:
   struct Options {
     uint32_t submission_queue_entries = 128;
     // Automatically determined if 0.
     uint32_t completion_queue_entries = 0;
   };

   ~IoUring();

   // Moveable, but not copyable. struct io_uring is not copyable.
   IoUring(IoUring &&);
   IoUring(const IoUring &) = delete;
   IoUring &operator=(IoUring &&);
   IoUring &operator=(const IoUring &) = delete;

   static absl::StatusOr<IoUring> Create(Options opts);

 private:
   IoUring() = default;
   explicit IoUring(io_uring ring);

   std::optional<io_uring> ring_;
};

}

#endif  // DFS_IO_URING_H_
