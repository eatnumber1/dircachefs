// Known-good input of dcfs_qualified_inside_dcfs.query: unqualified names
// inside the namespace, the qualifier outside it, the fully qualified
// spelling of a macro body (which expands anywhere), and `dcfs::` in a
// comment (dcfs::Helper), a string and a namespace declaration.
#define CALL_HELPER() ::dcfs::Helper()

namespace dcfs {

int Helper();

namespace syscalls {
int open();
}  // namespace syscalls

int Unqualified() { return Helper() + syscalls::open(); }

int ThroughMacro() { return CALL_HELPER(); }

const char *Named() { return "dcfs::Helper"; }

}  // namespace dcfs

namespace dcfs::declared {
int Value();
}  // namespace dcfs::declared

int Outside() { return dcfs::Helper(); }
