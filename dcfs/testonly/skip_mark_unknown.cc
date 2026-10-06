// A fault for trace validation's test-first check (formal/README.md, "Trace
// validation"): phase 1 no longer marks names unknown. Every statement that
// would store a dentry as 'unknown' (cache::MarkUnknown, via PutDentry) is
// skipped as if it had run. Linked, with -Wl,--wrap=sqlite3_step, only into
// //dcfs:dir_cache_fs_fault_test, whose trace validation
// (//dcfs:trace_fault_injection_test) must reject the trace at that phase 1.

#include <cstring>

#include "sqlite3.h"

extern "C" {

int __real_sqlite3_step(sqlite3_stmt *stmt);

int __wrap_sqlite3_step(sqlite3_stmt *stmt) {
  char *sql = sqlite3_expanded_sql(stmt);
  const bool skip = sql != nullptr &&
                    std::strncmp(sql, "INSERT INTO dentries", 20) == 0 &&
                    std::strstr(sql, "'unknown'") != nullptr;
  sqlite3_free(sql);
  if (skip) return SQLITE_DONE;
  return __real_sqlite3_step(stmt);
}

}  // extern "C"
