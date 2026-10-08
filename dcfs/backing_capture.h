#ifndef DCFS_BACKING_CAPTURE_H_
#define DCFS_BACKING_CAPTURE_H_

// Capturing the backing tree for mount.dcfs (phase 15: dcfs.fstype absent, a
// native type or `bind`), with no per-type code. A helper process unshares
// its mount namespace (so nothing it mounts is visible to anyone else, and
// everything goes with it if it dies), runs mount(8) there for SOURCE on a
// staging directory (mount(8) and the type's own helper do the tag
// resolution, flags and the rest), clones the result with open_tree
// (OPEN_TREE_CLONE, not recursive) and sends that descriptor back over a
// socket pair. The helper then exits, taking its namespace and the staging
// mount with it; what is left is the anonymous clone, which only the
// returned descriptors keep alive: it dissolves, and the backing superblock
// is released, when dcfs exits however it exits.

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "dcfs/fd.h"

namespace dcfs {

struct CaptureRequest {
  std::string source;  // as written: a device, UUID=..., host:/export, a path
  // `-t TYPE` for mount(8); absent: it autodetects. Ignored for a bind.
  std::string native_type;
  bool bind = false;  // `mount -o bind SOURCE`
  // The options for the underlying mount, `dcfs.`-prefixed ones removed.
  std::vector<std::string> options;
  bool sloppy = false;  // -s
  bool verbose = false;  // -v
};

struct CapturedTree {
  // The captured filesystem's root directory, opened for reading
  // (O_RDONLY | O_DIRECTORY): what dcfs walks and decodes handles through.
  FileDescriptor root;
  // The open_tree descriptor, kept open so the clone stays in its anonymous
  // namespace for as long as dcfs runs.
  FileDescriptor tree;
};

// The failure of the native mount carries its mount(8) exit status
// (NativeMountError) and its own error text.
absl::StatusOr<CapturedTree> CaptureBacking(const CaptureRequest &request);

// The argv of the mount(8) run on `staging`, for tests of the command line.
std::vector<std::string> NativeMountCommand(const CaptureRequest &request,
                                            const std::string &staging);

}  // namespace dcfs

#endif  // DCFS_BACKING_CAPTURE_H_
