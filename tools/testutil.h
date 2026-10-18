/* SPDX-License-Identifier: MIT */
#ifndef TOOLS_TESTUTIL_H_
#define TOOLS_TESTUTIL_H_

#ifdef __cplusplus
extern "C" {
#endif

int watch_pid(const char *pid_str, int *gone);
int cmd_waitexit(char *const pids[], int count);

#ifdef __cplusplus
}
#endif

#endif  // TOOLS_TESTUTIL_H_
