#ifndef DCFS_BACKING_CAPTURE_H_
#define DCFS_BACKING_CAPTURE_H_

// Opening the backing tree for mount.dcfs (phase 15). OpenBacking owns the
// three forms of dcfs.fstype: `none` opens SOURCE in the caller's namespace;
// a native type, or none, and `bind` capture the tree with no per-type code.
//
// The capture: a helper process unshares its mount namespace (so nothing it
// mounts is visible to anyone else, and everything goes with it if it dies),
// mounts a tmpfs there for a staging directory, runs mount(8) for SOURCE on
// it (mount(8) and the type's own helper do the tag resolution, flags and
// the rest), refuses a filesystem that went read-only by itself (11.5: only
// its own mountinfo lists the staging mount), clones the result with
// open_tree (OPEN_TREE_CLONE, not recursive) and sends that descriptor back
// over a socket pair. The helper then exits, taking its namespace and the
// staging mount with it; what is left is the anonymous clone, which only the
// returned descriptors keep alive: it dissolves, and the backing superblock
// is released, when dcfs exits however it exits.

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/mount_dcfs.h"

namespace dcfs {

struct CaptureRequest {
  std::string source;  // as written: a device, UUID=..., host:/export, a path
  // `-t TYPE` for mount(8); empty: it autodetects. Ignored for a bind.
  std::string native_type;
  bool bind = false;  // `mount -o bind SOURCE`
  // The options for the underlying mount, `dcfs.`-prefixed ones removed.
  std::vector<std::string> options;
  bool sloppy = false;   // -s
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

struct OpenedBacking {
  // The backing tree's root directory (a real descriptor, not O_PATH: it is
  // registered as the source filesystem's mount fd, and open_by_handle_at
  // resolves its mount descriptor as a regular file).
  FileDescriptor root;
  // What keeps a captured clone alive; invalid for `none`.
  FileDescriptor tree;
};

// Opens SOURCE as `options.backing` says and applies the start checks that
// belong to the form: nothing mounted below SOURCE (until 15.4's stubs) for
// `none` and `bind`, and no filesystem that went read-only by itself.
// `args.source` must be absolute for `none` and `bind` (the daemon has no
// working directory to resolve against).
absl::StatusOr<OpenedBacking> OpenBacking(const HelperArgs &args,
                                          const HelperOptions &options);

}  // namespace dcfs

#endif  // DCFS_BACKING_CAPTURE_H_
