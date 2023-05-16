#include <cstdlib>
#include <iostream>
#include <utility>
#include <cstdint>
#include <unistd.h>

#include "absl/log/initialize.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/cleanup/cleanup.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/log.h"
#include "dfs/sqlite.h"
#include "dfs/status.h"
#include "sqlite/sqlite3.h"

namespace dfs {

absl::StatusOr<int> Main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("[flags] [--] db");
  std::vector<char*> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  if (args.size() != 2) {
    std::cerr << "TODO usage here" << std::endl;
    return EXIT_FAILURE;
  }

  std::string database_uri(args[1]);

  // TODO SQLITE_CONFIG_SINGLETHREAD
  ASSIGN_OR_RETURN(
      Sqlite3 db,
      Sqlite3::Open(
        database_uri.c_str(),
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));

  // TODO move to separate file and syntax check+lint?
  RETURN_IF_ERROR(db.Exec(R"(
    CREATE TABLE dfs_entries (
      inode INTEGER PRIMARY KEY,
      name TEXT NOT NULL
    )
  )"));

  return EXIT_SUCCESS;
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
