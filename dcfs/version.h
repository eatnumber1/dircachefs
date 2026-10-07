#ifndef DCFS_VERSION_H_
#define DCFS_VERSION_H_

namespace dcfs {

// Printed by `dcfs --version`. A fixed string for now; the wrapper phase
// (Phase 15) will stamp it from git.
inline constexpr char kVersion[] = "0.1.0-dev";

}  // namespace dcfs

#endif  // DCFS_VERSION_H_
