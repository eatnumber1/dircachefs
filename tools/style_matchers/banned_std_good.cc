// Known-good input of banned_std.query: Abseil's versions, and the std::
// types that are not banned.
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/any_invocable.h"
#include "absl/time/clock.h"

absl::AnyInvocable<void()> good_callback;
absl::flat_hash_map<int, int> good_table;
absl::flat_hash_set<int> good_members;
std::vector<std::string> good_names;

absl::Time GoodNow() { return absl::Now(); }
