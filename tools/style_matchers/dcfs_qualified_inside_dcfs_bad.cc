// Known-bad input of dcfs_qualified_inside_dcfs.query.
namespace dcfs {

int Helper();

namespace syscalls {
int open();
}  // namespace syscalls

int Qualified() {
  return dcfs::Helper();  // HIT
}

int QualifiedSubNamespace() {
  return dcfs::syscalls::open();  // HIT
}

}  // namespace dcfs
