#ifndef DFS_FUSE_H_
#define DFS_FUSE_H_

#include <string>
#include <concepts>
#include <type_traits>
#include <utility>

#include "fuse/fuse_lowlevel.h"
#include "dfs/mount.h"
#include "dfs/fd.h"
#include "dfs/status.h"
#include "absl/status/statusor.h"
#include "absl/log/check.h"
#include "absl/container/flat_hash_set.h"

namespace dfs {

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
        fuse_req_t{},
        fuse_ino_t{},
        std::declval<std::add_pointer_t<fuse_file_info>>())
  } -> std::same_as<absl::Status>;
};

// implementation details below

using InitOpFn = void(*)(void *, struct fuse_conn_info *);
template <FuseInitOp T>
InitOpFn GetFuseInitOp() {
  return [](void *userdata, struct fuse_conn_info *conn) {
    CHECK_NE(userdata, nullptr);
    CHECK_NE(conn, nullptr);
    absl::Status s = static_cast<T*>(userdata)->Init(*conn);
    LOG_IF(ERROR, !s.ok()) << s;
  };
}
template <typename Others>
InitOpFn GetFuseInitOp() { return nullptr; }

using DestroyOpFn = void(*)(void *);
template <FuseDestroyOp T>
DestroyOpFn GetFuseDestroyOp() {
  return [](void *userdata) {
    CHECK_NE(userdata, nullptr);
    absl::Status s = static_cast<T*>(userdata)->Destroy();
    LOG_IF(ERROR, !s.ok()) << s;
  };
}
template <typename>
DestroyOpFn GetFuseDestroyOp() { return nullptr; }

using GetattrOpFn = void(*)(fuse_req_t, fuse_ino_t, struct fuse_file_info *);
template <FuseGetattrOp T>
GetattrOpFn GetFuseGetattrOp() {
  return [](fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    absl::Status s = t->Getattr(req, ino, fi);
    LOG_IF(ERROR, !s.ok()) << s;
  };
}
template <typename>
GetattrOpFn GetFuseGetattrOp() { return nullptr; }

template <typename T>
const struct fuse_lowlevel_ops AsFuseLowLevelOps() {
  struct fuse_lowlevel_ops ops = {
    .init = GetFuseInitOp<T>(),
    .destroy = GetFuseDestroyOp<T>(),
    .getattr = GetFuseGetattrOp<T>(),
  };
  return ops;
}

}  // namespace dfs

#endif  // DFS_FUSE_H_
