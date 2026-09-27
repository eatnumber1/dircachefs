#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/check.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "dcfs/fd.h"
#include "dcfs/fuse.h"
#include "dcfs/sqlite.h"
#include "dcfs/syscalls.h"
#include "dcfs/mount_table.h"
#include "dcfs/attributes.h"
#include "dcfs/dir_cache_fs.h"
#include "fuse_lowlevel.h"
#include "sqlite3.h"
#include "libmount.h"

ABSL_FLAG(std::string, database, ":memory:", "Path to the database file.");
ABSL_FLAG(absl::Duration, kernel_inode_attribute_timeout, absl::ZeroDuration(), "How long the kernel can cache inode attributes.");
ABSL_FLAG(absl::Duration, kernel_directory_entry_timeout, absl::ZeroDuration(), "How long the kernel can cache inode attributes.");
ABSL_FLAG(std::string, cache_of, "", "Path that this dircachefs will cache. Defaults to caching the directory being mounted over.");

namespace dcfs {
namespace {

using InodeID = ::dcfs::FileSystemDatabase::InodeID;

absl::StatusOr<
    std::unique_ptr<libmnt_cache, decltype(&mnt_unref_cache)> absl_nonnull>
    NewMountCache() {
  libmnt_cache *mc = mnt_new_cache();
  if (mc == nullptr) return absl::UnknownError("mnt_new_cache");
  return std::unique_ptr<libmnt_cache, decltype(&mnt_unref_cache)>(
      mc, mnt_unref_cache);
}

absl::StatusOr<int> Main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("[flags] -- [fuse_flags] mountpoint");
  std::vector<char*> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  for (int i = 0; i < args.size(); i++) {
    LOG(INFO) << "argv[" << i << "]=\"" << args[i] << "\"";
  }

  //argc = args.size();
  //args.push_back(nullptr);
  //argv = args.data();

  // TODO safe cast args.size()
  struct fuse_args fuse_args = FUSE_ARGS_INIT(static_cast<int>(args.size()), args.data());
  absl::Cleanup cleanup_fuse_args = [&fuse_args]() {
    fuse_opt_free_args(&fuse_args);
  };

  struct fuse_cmdline_opts fuse_opts;
  if (fuse_parse_cmdline(&fuse_args, &fuse_opts) != 0) return EXIT_FAILURE;
  absl::Cleanup cleanup_fuse_opts = [&fuse_opts]() {
    free(fuse_opts.mountpoint);
  };

  if (fuse_opts.show_help) {
    fuse_cmdline_help();
    fuse_lowlevel_help();
    return EXIT_SUCCESS;
  }

  if (fuse_opts.show_version) {
    std::cerr << "FUSE library version " << fuse_pkgversion() << std::endl;
    fuse_lowlevel_version();
    return EXIT_SUCCESS;
  }

  if (fuse_opts.mountpoint == nullptr) {
    std::cerr << "TODO usage here" << std::endl;
    return EXIT_FAILURE;
  }

  std::string database_uri = absl::GetFlag(FLAGS_database);
  if (database_uri.empty()) {
    std::cerr << "TODO specify the db path" << std::endl;
    return EXIT_FAILURE;
  }

  int db_open_flags =
    SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI | SQLITE_OPEN_CREATE |
    SQLITE_OPEN_READWRITE;

  ASSIGN_OR_RETURN(
      sqlite3::Connection db, sqlite3::Connection::Open(database_uri, db_open_flags));

  ASSIGN_OR_RETURN(auto mt, CreateMountTableFromMtab());
  std::unique_ptr<libmnt_table, decltype(&mnt_unref_table)> mtab =
    std::move(mt);

  ASSIGN_OR_RETURN(auto mc, NewMountCache());
  std::unique_ptr<libmnt_cache, decltype(&mnt_unref_cache)> mnt_cache =
    std::move(mc);

  MountFDCache mnt_fd_cache;

  FileHandle::Builder handle_builder(
      mtab.get(), mnt_cache.get(), &mnt_fd_cache);

  ASSIGN_OR_RETURN(
      auto cacheof_path,
      [&]() -> absl::StatusOr<std::string> {
        std::string cacheof_path = absl::GetFlag(FLAGS_cache_of);
        if (cacheof_path.empty()) cacheof_path = fuse_opts.mountpoint;
        return cacheof_path;
      }());
  ASSIGN_OR_RETURN(
      FileHandle root_handle,
      handle_builder.MakeHandle(AT_FDCWD, cacheof_path));
  ASSIGN_OR_RETURN(
      auto fs_db,
      FileSystemDatabase::Create(&db, &handle_builder, root_handle));
  ASSIGN_OR_RETURN(
      InodeID root_inode, fs_db.InsertRootDirectory(root_handle));

  DirCacheFS dcfs(
      &handle_builder, &fs_db, root_inode,
      {
        .kernel_inode_attribute_timeout =
            absl::GetFlag(FLAGS_kernel_inode_attribute_timeout),
        .kernel_directory_entry_timeout =
            absl::GetFlag(FLAGS_kernel_directory_entry_timeout),
      });

  struct fuse_lowlevel_ops dcfs_ops = AsFuseLowLevelOps<DirCacheFS>();
  struct fuse_session *fuse_session =
      fuse_session_new(&fuse_args, &dcfs_ops, sizeof(dcfs_ops), &dcfs);
  if (fuse_session == nullptr) return EXIT_FAILURE;
  absl::Cleanup cleanup_fuse_session = [fuse_session]() {
    fuse_session_destroy(fuse_session);
  };

  if (fuse_set_signal_handlers(fuse_session) != 0) return EXIT_FAILURE;
  absl::Cleanup cleanup_fuse_signal_handlers = [fuse_session]() {
    fuse_remove_signal_handlers(fuse_session);
  };

  if (fuse_session_mount(fuse_session, fuse_opts.mountpoint) != 0) {
    return EXIT_FAILURE;
  }
  absl::Cleanup cleanup_fuse_mount = [fuse_session]() {
    fuse_session_unmount(fuse_session);
  };

  fuse_daemonize(fuse_opts.foreground);

  if (fuse_opts.singlethread) {
    return fuse_session_loop(fuse_session);
  }

  struct fuse_loop_config *loop_config = fuse_loop_cfg_create();
  CHECK_NE(loop_config, nullptr);
  absl::Cleanup cleanup_loop_config = [loop_config]() {
    fuse_loop_cfg_destroy(loop_config);
  };
  fuse_loop_cfg_set_clone_fd(loop_config, fuse_opts.clone_fd);
  fuse_loop_cfg_set_idle_threads(loop_config, fuse_opts.max_idle_threads);
  fuse_loop_cfg_set_max_threads(loop_config, fuse_opts.max_threads);
  return fuse_session_loop_mt(fuse_session, loop_config);

#if 0
  absl::Status st = dcfs.DoStuff(fuse_opts.mountpoint);
  if (!st.ok()) {
    std::cerr << st << std::endl;
    return EXIT_FAILURE;
  }
#endif
}

}  // namespace
}  // namespace dcfs

int main(int argc, char *argv[]) {
  absl::StatusOr<int> ret = dcfs::Main(argc, argv);
  if (!ret.ok()) {
    std::cerr << ret.status() << std::endl;
    return EXIT_FAILURE;
  }
  return *ret;
}
