// The benchmark tree: the layout every bench/ tool agrees on.
//
//   <root>/t/<i/10000, 3 digits>/<(i/100)%100, 2 digits>/f<i%100, 2 digits>
//       the "entries": N small regular files (kFileBytes bytes each), at
//       depth 3 under <root>/t, 100 per directory;
//   <root>/big/f<j, 6 digits>   BIG files in one directory (readdir);
//   <root>/deep/l1/.../l6/leaf  an eight-component path (path walk).
#ifndef DCFS_BENCH_TREE_H_
#define DCFS_BENCH_TREE_H_

#include <cstdint>
#include <string>

namespace dcfs_bench {

inline constexpr int kFileBytes = 100;

// Path of entry `i` relative to a tree root.
std::string EntryPath(uint64_t i);
std::string BigPath(uint64_t j);
inline const char *DeepPath() { return "deep/l1/l2/l3/l4/l5/l6/leaf"; }

// Creates the tree under `root` (which must exist). Returns false and
// prints to stderr on failure.
bool MakeTree(const std::string &root, uint64_t entries, uint64_t big);

}  // namespace dcfs_bench

#endif  // DCFS_BENCH_TREE_H_
