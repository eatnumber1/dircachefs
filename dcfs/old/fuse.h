#ifndef DCFS_FUSE_H_
#define DCFS_FUSE_H_

#include <string>
#include <span>
#include <concepts>
#include <type_traits>
#include <utility>
#include <sys/stat.h>

#include "fuse/fuse_lowlevel.h"
#include "dcfs/mount.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "absl/status/statusor.h"
#include "absl/log/check.h"
#include "absl/container/flat_hash_set.h"

namespace dcfs {

struct FuseDirEntry {
   static std::vector<char> GetDirEntryBuffer(fuse_req_t req, std::span<FuseDirEntry> entries);

   std::string name;
   struct stat stbuf;
   off_t off;
};

// A FuseRequest is a wrapper around fuse_req_t that RAII owns replying to the
// request.
class FuseRequest {
 public:
  FuseRequest() = default;

  // Transfers responsibility to this FuseRequest for replying.
  FuseRequest(fuse_req_t req);
  ~FuseRequest();

  FuseRequest(FuseRequest &&);
  FuseRequest(const FuseRequest &) = delete;
  FuseRequest &operator=(FuseRequest &&);
  FuseRequest &operator=(const FuseRequest &) = delete;

  fuse_req_t &operator*();
  fuse_req_t &Get();

  absl::Status ReplyAttr(
    const struct stat &attr, absl::Duration attr_timeout);

  absl::Status ReplyOpen(const fuse_file_info &fi);

  // A "successful failure" response, e.g. ENOENT, where we succeeded in
  // performing an operation that correctly produces an error code.
  absl::Status ReplyErrno(int errnum);

  // A true failure response, e.g. db connection lost. We failed to do what the
  // user asked for.
  //
  // Must be called with a non-ok status.
  absl::Status ReplyFailure(const absl::Status &status);

  absl::Status ReplyBuf(std::string_view buf);
  absl::Status ReplyDirs(std::span<FuseDirEntry> entries, size_t maxsize);

  // Implemnetation detail -- helper for the OpFns below
  void ReplyFailureAndLogIfNotOk(const absl::Status &status);
 private:
  std::optional<fuse_req_t> req_;
};

// absl::Status Init(struct fuse_conn_info &conn);
// absl::Status Destroy();
template <typename T>
const struct fuse_lowlevel_ops AsFuseLowLevelOps();

template <typename T>
concept FuseInitOp = requires(T t) {
  {
    t.Init(
        std::declval<std::add_lvalue_reference_t<fuse_conn_info>>())
  } -> std::same_as<absl::Status>;
};
template <typename T>
concept FuseDestroyOp = requires(T t) {
  { t.Destroy() } -> std::same_as<absl::Status>;
};
template <typename T>
concept FuseGetattrOp = requires(T t) {
  {
    t.Getattr(
        std::declval<std::add_lvalue_reference_t<FuseRequest>>(),
        fuse_ino_t{})
  } -> std::same_as<absl::Status>;
};
template <typename T>
concept FuseOpendirOp = requires(T t) {
  {
    t.Opendir(
        std::declval<std::add_lvalue_reference_t<FuseRequest>>(),
        fuse_ino_t{},
        std::declval<std::add_lvalue_reference_t<fuse_file_info>>())
  } -> std::same_as<absl::Status>;
};
template <typename T>
concept FuseReaddirOp = requires(T t) {
  {
    t.Readdir(
        std::declval<std::add_lvalue_reference_t<FuseRequest>>(),
        fuse_ino_t{},
        size_t{},
        off_t{},
        std::declval<std::add_lvalue_reference_t<fuse_file_info>>())
  } -> std::same_as<absl::Status>;
};

// implementation details below

template <FuseInitOp T>
auto GetFuseInitOp() {
  return [](void *userdata, struct fuse_conn_info *conn) {
    CHECK_NE(userdata, nullptr);
    CHECK_NE(conn, nullptr);
    absl::Status s = static_cast<T*>(userdata)->Init(*conn);
    LOG_IF(ERROR, !s.ok()) << s;
  };
}
template <typename Others>
auto GetFuseInitOp() { return nullptr; }

template <FuseDestroyOp T>
auto GetFuseDestroyOp() {
  return [](void *userdata) {
    CHECK_NE(userdata, nullptr);
    absl::Status s = static_cast<T*>(userdata)->Destroy();
    LOG_IF(ERROR, !s.ok()) << s;
  };
}
template <typename>
auto GetFuseDestroyOp() { return nullptr; }

template <FuseGetattrOp T>
auto GetFuseGetattrOp() {
  return [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *) {
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyFailureAndLogIfNotOk(t->Getattr(fr, ino));
  };
}
template <typename>
auto GetFuseGetattrOp() { return nullptr; }

template <FuseOpendirOp T>
auto GetFuseOpendirOp() {
  return [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
    CHECK_NE(fi, nullptr);
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyFailureAndLogIfNotOk(t->Opendir(fr, ino, *fi));
  };
}
template <typename>
auto GetFuseOpendirOp() { return nullptr; }

template <FuseReaddirOp T>
auto GetFuseReaddirOp() {
  return [](fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            fuse_file_info *fi) {
    CHECK_NE(fi, nullptr);
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyFailureAndLogIfNotOk(t->Readdir(fr, ino, size, off, *fi));
  };
}
template <typename>
auto GetFuseReaddirOp() { return nullptr; }

template <typename T>
const struct fuse_lowlevel_ops AsFuseLowLevelOps() {
  struct fuse_lowlevel_ops ops = {
    .init = GetFuseInitOp<T>(),
    .destroy = GetFuseDestroyOp<T>(),
    .getattr = GetFuseGetattrOp<T>(),
    .opendir = GetFuseOpendirOp<T>(),
    .readdir = GetFuseReaddirOp<T>(),
  };
  return ops;
}

}  // namespace dcfs

#endif  // DCFS_FUSE_H_
