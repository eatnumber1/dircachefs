// Known-bad input of banned_std.query.
#include <chrono>
#include <functional>
#include <unordered_map>
#include <unordered_set>

std::function<void()> bad_callback;  // HIT
std::unordered_map<int, int> bad_table;  // HIT
std::unordered_set<int> bad_members;  // HIT
std::chrono::seconds bad_wait(1);  // HIT

auto BadNow() { return std::chrono::steady_clock::now(); }  // HIT
