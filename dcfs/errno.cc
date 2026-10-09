#include "dcfs/status.h"

#include <cstring>

#include "absl/base/no_destructor.h"
#include "absl/status/status_builder.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "absl/strings/numbers.h"
#include "absl/container/flat_hash_map.h"

namespace dcfs {

std::string ErrnoToErrorName(int error_number) {
  if (error_number == 0) return "OK";
  const char *n = strerrorname_np(error_number);
  if (n != nullptr) return n;
  // This syntax is parsed by ErrorNameToErrno.
  return absl::StrFormat("UNKNOWN (%d)", error_number);
}

namespace {

// The reverse (name -> errno) side of the errno<->name mapping.
const absl::flat_hash_map<std::string, int> &NameToErrnoTable() {
  static const absl::NoDestructor<absl::flat_hash_map<std::string, int>>
      kNamesToErrors(absl::flat_hash_map<std::string, int>{
      {"OK", 0},
#define E(n) {#n, n}
      E(EINVAL),
      E(ENAMETOOLONG),
      E(E2BIG),
      E(EDESTADDRREQ),
      E(EDOM),
      E(EFAULT),
      E(EILSEQ),
      E(ENOPROTOOPT),
      E(ENOSTR),
      E(ENOTSOCK),
      E(ENOTTY),
      E(EPROTOTYPE),
      E(ESPIPE),
      E(ETIMEDOUT),
      E(ETIME),
      E(ENODEV),
      E(ENOENT),
      E(ELOOP),
#ifdef ENOMEDIUM
      E(ENOMEDIUM),
#endif
      E(ENXIO),
      E(ESRCH),
      E(EEXIST),
      E(EADDRNOTAVAIL),
      E(EALREADY),
#ifdef ENOTUNIQ
      E(ENOTUNIQ),
#endif
      E(EPERM),
      E(EACCES),
#ifdef ENOKEY
      E(ENOKEY),
#endif
      E(EROFS),
      E(ENOTEMPTY),
      E(EISDIR),
      E(ENOTDIR),
      E(EADDRINUSE),
      E(EBADF),
#ifdef EBADFD
      E(EBADFD),
#endif
      E(EBUSY),
      E(ECHILD),
      E(EISCONN),
#ifdef EISNAM
      E(EISNAM),
#endif
#ifdef ENOTBLK
      E(ENOTBLK),
#endif
      E(ENOTCONN),
      E(EPIPE),
#ifdef ESHUTDOWN
      E(ESHUTDOWN),
#endif
      E(ETXTBSY),
#ifdef EUNATCH
      E(EUNATCH),
#endif
      E(ENOSPC),
#ifdef EDQUOT
      E(EDQUOT),
#endif
      E(EMFILE),
      E(EMLINK),
      E(ENFILE),
      E(ENOBUFS),
      E(ENODATA),
      E(ENOMEM),
      E(ENOSR),
#ifdef EUSERS
      E(EUSERS),
#endif
#ifdef ECHRNG
      E(ECHRNG),
#endif
      E(EFBIG),
      E(EOVERFLOW),
      E(ERANGE),
#ifdef ENOPKG
      E(ENOPKG),
#endif
      E(ENOSYS),
      E(ENOTSUP),
      // On Linux, ENOTSUP and EOPNOTSUPP are the same numeric value, and
      // strerrorname_np() picks EOPNOTSUPP as the canonical name for it, so
      // both names must round-trip back to that value.
      E(EOPNOTSUPP),
      E(EAFNOSUPPORT),
#ifdef EPFNOSUPPORT
      E(EPFNOSUPPORT),
#endif
      E(EPROTONOSUPPORT),
#ifdef ESOCKTNOSUPPORT
      E(ESOCKTNOSUPPORT),
#endif
      E(EXDEV),
      E(EAGAIN),
#ifdef ECOMM
      E(ECOMM),
#endif
      E(ECONNREFUSED),
      E(ECONNABORTED),
      E(ECONNRESET),
      E(EINTR),
#ifdef EHOSTDOWN
      E(EHOSTDOWN),
#endif
      E(EHOSTUNREACH),
      E(ENETDOWN),
      E(ENETRESET),
      E(ENETUNREACH),
      E(ENOLCK),
      E(ENOLINK),
#ifdef ENONET
      E(ENONET),
#endif
      E(EDEADLK),
#ifdef ESTALE
      E(ESTALE),
#endif
      E(ECANCELED),
      // The entries below were found missing by dcfs/status_test.cc's
      // exhaustive round trip over every errno value the C library names
      // (1..134): every one of these is a name strerrorname_np() produces
      // as the canonical name for some errno on this platform, so
      // ErrorNameToErrno() must be able to parse it back.
      E(EIO),
      E(ENOEXEC),
      E(EIDRM),
      E(EMSGSIZE),
      E(EPROTO),
      E(EMULTIHOP),
      E(EBADMSG),
      E(ENOMSG),
      E(EINPROGRESS),
      E(EREMOTE),
      E(ERESTART),
      E(ESTRPIPE),
      E(ETOOMANYREFS),
      E(EOWNERDEAD),
      E(ENOTRECOVERABLE),
#ifdef EADV
      E(EADV),
#endif
#ifdef EBADE
      E(EBADE),
#endif
#ifdef EBADR
      E(EBADR),
#endif
#ifdef EBADRQC
      E(EBADRQC),
#endif
#ifdef EBADSLT
      E(EBADSLT),
#endif
#ifdef EBFONT
      E(EBFONT),
#endif
#ifdef EDOTDOT
      E(EDOTDOT),
#endif
#ifdef EHWPOISON
      E(EHWPOISON),
#endif
#ifdef EKEYEXPIRED
      E(EKEYEXPIRED),
#endif
#ifdef EKEYREJECTED
      E(EKEYREJECTED),
#endif
#ifdef EKEYREVOKED
      E(EKEYREVOKED),
#endif
#ifdef EL2HLT
      E(EL2HLT),
#endif
#ifdef EL2NSYNC
      E(EL2NSYNC),
#endif
#ifdef EL3HLT
      E(EL3HLT),
#endif
#ifdef EL3RST
      E(EL3RST),
#endif
#ifdef ELIBACC
      E(ELIBACC),
#endif
#ifdef ELIBBAD
      E(ELIBBAD),
#endif
#ifdef ELIBEXEC
      E(ELIBEXEC),
#endif
#ifdef ELIBMAX
      E(ELIBMAX),
#endif
#ifdef ELIBSCN
      E(ELIBSCN),
#endif
#ifdef ELNRNG
      E(ELNRNG),
#endif
#ifdef EMEDIUMTYPE
      E(EMEDIUMTYPE),
#endif
#ifdef ENAVAIL
      E(ENAVAIL),
#endif
#ifdef ENOANO
      E(ENOANO),
#endif
#ifdef ENOCSI
      E(ENOCSI),
#endif
#ifdef ENOTNAM
      E(ENOTNAM),
#endif
#ifdef EREMCHG
      E(EREMCHG),
#endif
#ifdef EREMOTEIO
      E(EREMOTEIO),
#endif
#ifdef ERFKILL
      E(ERFKILL),
#endif
#ifdef ESRMNT
      E(ESRMNT),
#endif
#ifdef EUCLEAN
      E(EUCLEAN),
#endif
#ifdef EXFULL
      E(EXFULL),
#endif
#undef E
  });
  return *kNamesToErrors;
}

}  // namespace

absl::StatusOr<int> ErrorNameToErrno(std::string_view error_name) {
  if (std::string_view en = error_name;
      absl::ConsumePrefix(&en, "UNKNOWN (") && absl::ConsumeSuffix(&en, ")")) {
    int err;
    if (absl::SimpleAtoi(en, &err)) return err;
  }

  const absl::flat_hash_map<std::string, int> &names_to_errors =
      NameToErrnoTable();
  auto it = names_to_errors.find(error_name);
  if (it == names_to_errors.end()) {
    return NotFoundErrorBuilder() << "No such errno for " << error_name;
  }
  return it->second;
}

}  // namespace dcfs
