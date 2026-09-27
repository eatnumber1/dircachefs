#ifndef DCFS_MEMORY_H_
#define DCFS_MEMORY_H_

namespace dcfs {

// Deletes memory allocated using ::operator new (size).
struct RawMemoryDeleter {
  void operator()(void *ptr) const { ::operator delete (ptr); }
};

}  // namespace dcfs

#endif  // DCFS_MEMORY_H_
