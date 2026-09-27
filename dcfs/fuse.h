#ifndef DCFS_FUSE_H_
#define DCFS_FUSE_H_

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)

#include <concepts>
#include <span>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <string_view>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/time/time.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/attributes.h"
#include "fuse_lowlevel.h"

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

  absl::Status ReplyEntry(
      fuse_ino_t ino, uint64_t generation, struct stat attr,
      absl::Duration attr_timeout, absl::Duration entry_timeout);

  // Implemnetation details -- helpers for the OpFns below
  void ReplyFailureAndLogIfNotOk(const absl::Status &status);
  void ReplyAlwaysAndLogIfNotOk(const absl::Status &status);
 private:
  std::optional<fuse_req_t> req_;
};

template <typename T>
const fuse_lowlevel_ops AsFuseLowLevelOps();

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
        fuse_ino_t{},
        std::declval<fuse_file_info*>())
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
concept FuseReleasedirOp = requires(T t) {
  {
    t.Releasedir(
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
template <typename T>
concept FuseReaddirplusOp = requires(T t) {
  {
    t.Readdirplus(
        std::declval<std::add_lvalue_reference_t<FuseRequest>>(),
        fuse_ino_t{},
        size_t{},
        off_t{},
        std::declval<std::add_lvalue_reference_t<fuse_file_info>>())
  } -> std::same_as<absl::Status>;
};
template <typename T>
concept FuseLookupOp = requires(T t) {
  {
    t.Lookup(
        std::declval<std::add_lvalue_reference_t<FuseRequest>>(),
        fuse_ino_t{},
        std::string_view{})
  } -> std::same_as<absl::Status>;
};

struct LogFuseFileInfo {
 public:
  explicit LogFuseFileInfo(fuse_file_info *fi);
  explicit LogFuseFileInfo(fuse_file_info &fi);

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const LogFuseFileInfo &lfi);

 private:
  fuse_file_info *absl_nullable fi_ = nullptr;
};

// implementation details below

template <FuseInitOp T>
auto GetFuseInitOp() {
  return [](void *userdata, fuse_conn_info *conn) {
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
  return [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyFailureAndLogIfNotOk(t->Getattr(fr, ino, fi));
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

template <FuseReaddirplusOp T>
auto GetFuseReaddirplusOp() {
  return [](fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            fuse_file_info *fi) {
    CHECK_NE(fi, nullptr);
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyFailureAndLogIfNotOk(t->Readdirplus(fr, ino, size, off, *fi));
  };
}
template <typename>
auto GetFuseReaddirplusOp() { return nullptr; }

template <FuseReleasedirOp T>
auto GetFuseReleasedirOp() {
  return [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
    CHECK_NE(fi, nullptr);
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyAlwaysAndLogIfNotOk(t->Releasedir(fr, ino, *fi));
  };
}
template <typename>
auto GetFuseReleasedirOp() { return nullptr; }

template <FuseLookupOp T>
auto GetFuseLookupOp() {
  return [](fuse_req_t req, fuse_ino_t parent, const char *name) {
    auto *t = static_cast<T*>(fuse_req_userdata(req));
    CHECK_NE(t, nullptr);
    FuseRequest fr(req);
    fr.ReplyFailureAndLogIfNotOk(t->Lookup(fr, parent, name));
  };
}
template <typename>
auto GetFuseLookupOp() { return nullptr; }

template <typename T>
const fuse_lowlevel_ops AsFuseLowLevelOps() {
  fuse_lowlevel_ops ops = {
    .init = GetFuseInitOp<T>(),
    .destroy = GetFuseDestroyOp<T>(),
    .lookup = GetFuseLookupOp<T>(),
    .getattr = GetFuseGetattrOp<T>(),
    .opendir = GetFuseOpendirOp<T>(),
    .readdir = GetFuseReaddirOp<T>(),
    .releasedir = GetFuseReleasedirOp<T>(),
    .readdirplus = GetFuseReaddirplusOp<T>(),
  };
  return ops;
}

template <typename Sink>
void AbslStringify(Sink &sink, const LogFuseFileInfo &lfi) {
  if (lfi.fi_ == nullptr) {
    absl::Format(&sink, "nullptr");
    return;
  }
  fuse_file_info &fi = *lfi.fi_;
  absl::Format(&sink, "fuse_file_info {\n");
  if (fi.flags) {
    absl::Format(&sink, "\t.flags = %v\n", LogOpenFlags(fi.flags));
  }
  if (fi.writepage) {
    absl::Format(
        &sink, "\t.writepage = %v\n", static_cast<bool>(fi.writepage));
  }
  if (fi.direct_io) {
    absl::Format(
        &sink, "\t.direct_io = %v\n", static_cast<bool>(fi.direct_io));
  }
  if (fi.keep_cache) {
    absl::Format(
        &sink, "\t.keep_cache = %v\n", static_cast<bool>(fi.keep_cache));
  }
  if (fi.flush) {
    absl::Format(&sink, "\t.flush = %v\n", static_cast<bool>(fi.flush));
  }
  if (fi.nonseekable) {
    absl::Format(
        &sink, "\t.nonseekable = %v\n", static_cast<bool>(fi.nonseekable));
  }
  if (fi.flock_release) {
    absl::Format(
        &sink, "\t.flock_release = %v\n", static_cast<bool>(fi.flock_release));
  }
  if (fi.cache_readdir) {
    absl::Format(
        &sink, "\t.cache_readdir = %v\n", static_cast<bool>(fi.cache_readdir));
  }
  if (fi.noflush) {
    absl::Format(&sink, "\t.noflush = %v\n", static_cast<bool>(fi.noflush));
  }
  if (fi.parallel_direct_writes) {
    absl::Format(
        &sink, "\t.parallel_direct_writes = %v\n",
        static_cast<bool>(fi.parallel_direct_writes));
  }
  if (fi.fh) {
    absl::Format(&sink, "\t.fh = %p\n", reinterpret_cast<void *>(fi.fh));
  }
  if (fi.lock_owner) {
    absl::Format(&sink, "\t.lock_owner = %d\n", fi.lock_owner);
  }
  if (fi.poll_events) {
    absl::Format(&sink, "\t.poll_events = %d\n", fi.poll_events);
  }
  absl::Format(&sink, "}");
}

}  // namespace dcfs

#endif  // DCFS_FUSE_H_
