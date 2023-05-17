#define FUSE_USE_VERSION 312

#include <sys/stat.h>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <cstdint>
#include <unistd.h>

#include "absl/time/time.h"
#include "absl/log/initialize.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/cleanup/cleanup.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/log.h"
#include "dfs/io_uring.h"
#include "dfs/syscalls.h"
#include "dfs/fuse.h"
#include "dfs/sqlite.h"
#include "fuse/fuse_lowlevel.h"
#include "sqlite/sqlite3.h"

ABSL_FLAG(
    uint32_t, io_uring_submission_queue_entries, 128,
    "The minimum number of entries to fit in the io_uring submission queue.");
ABSL_FLAG(
    uint32_t, io_uring_completion_queue_entries, 0,
    "The minimum number of entries to fit in the io_uring completion queue. "
    "Automatically determined if 0.");
ABSL_FLAG(std::string, database, "", "Path to the database file.");
ABSL_FLAG(bool, db_readonly, false, "Open the database as read-only.");
// TODO turn into a mkfs command
ABSL_FLAG(bool, db_create, false, "Create the database if it doesn't already exist.");
ABSL_FLAG(absl::Duration, kernel_entry_timeout, absl::ZeroDuration(), "How long the kernel can cache fs entries (files and directories).");

namespace dfs {

class Diskyphus {
 public:
  struct Options {
    IoUring::Options io_uring_opts;
    absl::Duration kernel_entry_timeout = absl::ZeroDuration();
  };

  Diskyphus(Sqlite3 db, Options opts) : db_(std::move(db)), opts_(std::move(opts)) {}

  absl::Status Init(struct fuse_conn_info &conn) {
    LOG(INFO)
      << "Fuse connection using kernel protocol version " << conn.proto_major
      << "." << conn.proto_minor;
    LOG(INFO) << "Maximum write buffer size is " << conn.max_write;
    // TODO allow setting this  when `-o max_read=<n>` is used.
    LOG(INFO) << "Maximum read buffer size is " << conn.max_read;
    LOG(INFO) << "Maximum readahead is " << conn.max_readahead;
    LOG(INFO) << "Maximum background requests is " << conn.max_background;
    LOG(INFO) << "Congestion threshold is " << conn.congestion_threshold;
    // TODO set FUSE_CAP_EXPORT_SUPPORT
    return absl::OkStatus();
  }
  static_assert(FuseInitOp<Diskyphus>);

  absl::Status Destroy() {
    LOG(INFO) << "Destroy()";
    return absl::OkStatus();
  }
  static_assert(FuseDestroyOp<Diskyphus>);

  absl::Status Getattr(FuseRequest &req, fuse_ino_t ino) {
    LOG(INFO) << "Getattr() ino:" << ino;
    struct stat attrs = {
      .st_ino = ino,
    };
    RETURN_IF_ERROR(db_.Exec(
        R"(
          SELECT inode
          FROM dfs_entries
          WHERE inode = 0
        )",
        [&attrs](const std::vector<std::string_view> &,
                 std::vector<std::string_view> colvals) {
          CHECK_EQ(colvals.size(), 1);
          LOG(INFO) << "got inode " << colvals[0];
          return absl::OkStatus();
        }));
    attrs.st_mode = S_IFDIR | 0755;
    attrs.st_nlink = 2;
    return req.ReplyAttr(attrs, opts_.kernel_entry_timeout);
  }
  static_assert(FuseGetattrOp<Diskyphus>);

  absl::Status Opendir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
    LOG(INFO) << "Opendir() ino:" << ino;
    return req.ReplyOpen(fi);
  }
  static_assert(FuseGetattrOp<Diskyphus>);

  absl::Status Readdir(FuseRequest &req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info &fi) {
    LOG(INFO) << "Readdir() ino:" << ino << ", off:" << off << ", size:" << size;
    // TODO change to use fuse_reply_data

    if (off == 5) {
      return req.ReplyBuf({nullptr, 0});
    }

    std::vector<FuseDirEntry> dirs {
      {
        .name = ".",
        .stbuf = {
          .st_ino = ino,
          .st_mode = S_IFDIR,
        },
        .off = 3,  // TODO
      },
      {
        .name = "..",
        .stbuf = {
          .st_ino = 4,
          .st_mode = S_IFDIR,
        },
        .off = 4,  // TODO
      },
      {
        .name = "hello",
        .stbuf = {
          .st_ino = 5,
          .st_mode = S_IFMT,
        },
        .off = 5,  // TODO
      },
    };

    return req.ReplyDirs(dirs, /*maxsize=*/size);
  }
  static_assert(FuseReaddirOp<Diskyphus>);

#if 0
  absl::Status DoStuff(std::string mountpoint) {
    // prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
    //
    // struct rlimit rlimit;
    // rlimit.rlim_cur = RLIM_INFINITY;
    // rlimit.rlim_max = RLIM_INFINITY;
    // rc = setrlimit(RLIMIT_MEMLOCK, &rlimit);
    // mlock all

    absl::StatusOr<IoUring> uring = IoUring::Create(opts_.io_uring_opts);
    if (!uring.ok()) return std::move(uring).status();

#if 0
    LOG(INFO) << "Mounting to " << mountpoint;
    absl::StatusOr<FuseMount> mount = FuseMount::Create(
        std::move(mountpoint),
        {
            .fsname = "dfs",
            .mount_source = "mydisks",
            .mount_options = {"default_permissions"},
        });
    if (!mount.ok()) return std::move(mount).status();
#endif

    LOG(INFO) << "Success";

    return absl::OkStatus();
  }
#endif

 private:
  Sqlite3 db_;
  const Options opts_;
};

absl::StatusOr<int> Main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("[flags] -- [fuse_flags [--]] mountpoint");
  std::vector<char*> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

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

  int db_open_flags = SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI;
  if (absl::GetFlag(FLAGS_db_readonly)) {
    db_open_flags |= SQLITE_OPEN_READONLY;
  } else {
    db_open_flags |= SQLITE_OPEN_READWRITE;
  }
  if (absl::GetFlag(FLAGS_db_create)) {
    db_open_flags |= SQLITE_OPEN_CREATE;
  }

  ASSIGN_OR_RETURN(
      Sqlite3 db, Sqlite3::Open(database_uri.c_str(), db_open_flags));

  dfs::Diskyphus dfs(
      std::move(db),
      {
        .io_uring_opts {
          .submission_queue_entries =
              absl::GetFlag(FLAGS_io_uring_submission_queue_entries),
          .completion_queue_entries =
              absl::GetFlag(FLAGS_io_uring_completion_queue_entries),
        },
        .kernel_entry_timeout = absl::GetFlag(FLAGS_kernel_entry_timeout),
      });

  struct fuse_lowlevel_ops dfs_ops = dfs::AsFuseLowLevelOps<dfs::Diskyphus>();
  struct fuse_session *fuse_session =
      fuse_session_new(&fuse_args, &dfs_ops, sizeof(dfs_ops), &dfs);
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
  absl::Status st = dfs.DoStuff(fuse_opts.mountpoint);
  if (!st.ok()) {
    std::cerr << st << std::endl;
    return EXIT_FAILURE;
  }
#endif
}

}  // namespace dfs

int main(int argc, char *argv[]) {
  absl::StatusOr<int> ret = dfs::Main(argc, argv);
  if (!ret.ok()) {
    std::cerr << ret.status() << std::endl;
    return EXIT_FAILURE;
  }
  return *ret;
}
