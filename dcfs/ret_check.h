#ifndef DCFS_RET_CHECK_H_
#define DCFS_RET_CHECK_H_

#include "absl/status/status.h"

#define RET_CHECK(expr) \
  ({ \
   if (!(expr)) { \
     return absl::InternalError("assertion failed: " #expr); \
   } \
  })

#define RET_CHECK_EQ(lhs, rhs) RET_CHECK((lhs) == (rhs))
#define RET_CHECK_NE(lhs, rhs) RET_CHECK((lhs) != (rhs))
#define RET_CHECK_GT(lhs, rhs) RET_CHECK((lhs) > (rhs))
#define RET_CHECK_LT(lhs, rhs) RET_CHECK((lhs) < (rhs))
#define RET_CHECK_GE(lhs, rhs) RET_CHECK((lhs) >= (rhs))
#define RET_CHECK_LE(lhs, rhs) RET_CHECK((lhs) <= (rhs))

#endif  // DCFS_RET_CHECK_H_
