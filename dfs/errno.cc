#include "dfs/status.h"

#include "absl/strings/str_format.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "absl/strings/numbers.h"
#include "absl/container/flat_hash_map.h"

namespace dfs {

std::string ErrnoToErrorName(int error_number) {
  if (error_number == 0) return "OK";
  const char *n = strerrorname_np(error_number);
  if (n != nullptr) return n;
  // This syntax is parsed by ErrorNameToErrno.
  return absl::StrFormat("UNKNOWN (%d)", error_number);
}

absl::StatusOr<int> ErrorNameToErrno(std::string_view error_name) {
  const static auto *kNamesToErrors = new absl::flat_hash_map<std::string, int>{
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
#undef E
  };

  if (std::string_view en = error_name;
      absl::ConsumePrefix(&en, "UNKNOWN (") && absl::ConsumeSuffix(&en, ")")) {
    int err;
    if (absl::SimpleAtoi(en, &err)) return err;
  }

  auto it = kNamesToErrors->find(error_name);
  if (it == kNamesToErrors->end()) {
    // TODO statusbuilder
    return absl::NotFoundError(absl::StrCat("No such errno for ", error_name));
  }
  return it->second;
}

}  // namespace dfs
