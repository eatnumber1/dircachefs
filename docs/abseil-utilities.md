# Abseil utilities for dcfs coders and reviewers

A catalogue of what the pinned Abseil offers, so that nobody hand-rolls what a
header already gives. It is a reference, not a rule book: the rules are in
`docs/style.md` and `AGENTS.md`, and the "Rule" column below says which of
them already demands a swap. Abseil's Tips of the Week
(https://abseil.io/tips/) are design guidance behind it (`docs/style.md`,
"Precedence, and the Abseil Tips of the Week").

- **Pinned version: abseil-cpp 20260817.0** (`MODULE.bazel`, `bazel_dep(name
  = "abseil-cpp", ...)`, repo name `absl`). Everything below was read from
  the headers of that release under `external/abseil-cpp+/absl/` in the Bazel
  output base. A path such as `strings/str_cat.h` means
  `absl/strings/str_cat.h`. The Bazel target is `@absl//absl/<dir>:<name>`;
  read the name from that directory's `BUILD.bazel` (`@absl//absl/strings`
  holds most string headers, `str_format` and `cord` are separate targets,
  `algorithm:container`, `status:status_builder`, `log:check`).
- C++ standard: `-std=c++20` (`MODULE.bazel`, `.bazelrc`). In that mode many
  Abseil names are aliases of the standard ones (`absl::string_view`,
  `absl::optional`, `absl::variant`, `absl::any`, `absl::bit_cast`,
  `absl::popcount`, `absl::make_unique`): the aliases are deprecated, so use
  the `std::` spelling (`docs/style.md` 1.2).
- **Refreshing this document when the pin moves.** (1) `ls $(bazel info
  output_base)/external/abseil-cpp+/absl/` and diff the directory and header
  lists against section 2. (2) For each header that changed, reread its
  top-of-file comment and public declarations and fix the entry. (3) Check
  the names the swaps table relies on still exist (the grep list under "Names
  to re-verify", end of section 1). (4) Rerun the commands under "Candidates
  for a sweep" and update the counts and the tree commit. (5) Update the
  version above.
- Counts of dcfs use were taken at `89b917c` (2026-10-10). "prod" means
  `dcfs/` (not `*_test.cc`, not `dcfs/testonly/`), `tools/*.cc`, `bench/` and
  `test/qemu/testdata/*.cc`; "tests" means `*_test.cc` and `dcfs/testonly/`.

## 1. The swaps table

Left: the hand-written pattern or `std::` call. Right: the Abseil utility and
its header. "Rule": the `docs/style.md` section that already demands the swap,
`adv.` where it is only advice (a Tip of the Week or this document), `no`
where the rule goes the other way (the swap is banned or the `std::` form is
the rule), `-` for none.

### Strings and formatting

| Instead of | Use | Header | Rule |
|---|---|---|---|
| `a + "/" + b`, `s += x; s += y;` chains | `absl::StrCat(a, "/", b)` (accepts strings, ints, bools, floats, `AbslStringify` types) | `strings/str_cat.h` | adv. |
| appending in a loop (`s += ...`, `s.append`) | `absl::StrAppend(&s, a, b, ...)` | `strings/str_cat.h` | adv. |
| `std::to_string(n)` as a piece of a larger string | pass `n` straight to `StrCat`; `absl::Hex(n, absl::kZeroPad8)` / `absl::Dec(n, absl::kZeroPad4)` for hex or padding | `strings/str_cat.h` | adv. |
| `std::ostringstream`, `operator<<` into a string | `StrCat` / `StrAppend` / `StrFormat` | `strings/str_cat.h` | adv. |
| `snprintf` / `sprintf` into a buffer | `absl::StrFormat("%02x:%d", a, b)` (compile-time checked), `StrAppendFormat(&s, ...)` | `strings/str_format.h` | adv. |
| `printf` / `fprintf(stderr, ...)` for output | `absl::PrintF`, `absl::FPrintF(stderr, ...)`; for diagnostics `LOG` | `strings/str_format.h` | 1.7 for logs |
| a `"$0 of $1"` message with reused arguments | `absl::Substitute("$1 of $0", a, b)` (no type letters; no hex or padding: use `StrFormat`) | `strings/substitute.h` | - |
| hand-written `find`/`substr` split loop | `absl::StrSplit(s, ',')`; with `absl::SkipEmpty()`, `absl::MaxSplits(',', 1)`, `absl::ByAnyChar(" \t")`, `absl::ByString`, `absl::ByLength`; result may be `std::vector<std::string_view>` (views into `s`) | `strings/str_split.h` | adv. |
| a loop that inserts a separator | `absl::StrJoin(range, ", ")`; formatters `absl::AlphaNumFormatter`, `StreamFormatter`, or a lambda `(std::string *out, const T &v)` | `strings/str_join.h` | adv. |
| `s.rfind(p, 0) == 0`, `s.compare(0, n, p) == 0`, `s.substr(0, n) == p` | `absl::StartsWith(s, p)` or C++20 `s.starts_with(p)` (the tree uses the member form; either is right) | `strings/match.h` | adv. |
| `s.find(x) != std::string::npos` | `absl::StrContains(s, x)` (`std::string::contains` is C++23, not available at C++20) | `strings/match.h` | adv. |
| `s.substr(0, s.size() - n)` after a suffix test, `if (starts_with) s.remove_prefix(n)` | `absl::ConsumePrefix(&sv, p)` / `ConsumeSuffix`, `StripPrefix` / `StripSuffix` (return the stripped view) | `strings/strip.h` | adv. |
| case-insensitive compare via `tolower` | `absl::EqualsIgnoreCase`, `StartsWithIgnoreCase`, `StrContainsIgnoreCase` (ASCII only) | `strings/match.h` | - |
| manual trim loop, `isspace` | `absl::StripAsciiWhitespace(sv)` (also `StripLeading...`/`StripTrailing...`; the `std::string *` overloads strip in place) | `strings/ascii.h` | - |
| `std::tolower` / `isdigit` / `isspace` (locale-dependent, UB on negative `char`) | `absl::ascii_tolower`, `ascii_isdigit`, `ascii_isspace`, ...; `absl::AsciiStrToLower(s)` / `AsciiStrToUpper(s)` | `strings/ascii.h` | - |
| `atoi`, `strtoul`, `std::stoi` (errors lost or thrown) | `absl::SimpleAtoi(sv, &out)` (bool, tolerates surrounding whitespace, range-checked), `SimpleAtob`, `SimpleAtod`, `SimpleHexAtoi` | `strings/numbers.h` | 1.2 (no exceptions) |
| `strtod`, `std::stod` | `absl::SimpleAtod`; `absl::from_chars` for a pointer range | `strings/numbers.h`, `strings/charconv.h` | - |
| a hand-written hex table / `%02x` loop over bytes | `absl::BytesToHexString(sv)`, `absl::HexStringToBytes(hex, &out)` (bool) | `strings/escaping.h` | adv. |
| hand-written C-style escaping of bytes | `absl::CEscape`, `CHexEscape` (octal vs `\xNN`), `CUnescape`; `Utf8Safe...` variants keep valid UTF-8. NB dcfs's own `EscapeBytes` (`dcfs/escape.h`) is the one escaping for names; `CHexEscape` also escapes `'`, so it is not a drop-in | `strings/escaping.h` | `dcfs/escape.h` is the rule (1.8, "Names are bytes") |
| hand-written base64 | `absl::Base64Escape` / `Base64Unescape`, `WebSafeBase64Escape` / `...Unescape` | `strings/escaping.h` | - |
| hand-written percent-encoding | `absl::UrlEscape` / `UrlUnescape`, `UrlEscapePlus` / `UrlUnescapePlus` (check the usage note: a component, not a whole URL) | `strings/escaping.h` | - |
| `std::string::replace` / `find` loops | `absl::StrReplaceAll(s, {{"a", "b"}, ...})` | `strings/str_replace.h` | - |
| `std::string s(n, '\0')` then fill and `resize(k)` | `absl::StringResizeAndOverwrite(s, n, op)` (no zero fill; C++23 `resize_and_overwrite` polyfill). If the buffer is not a string, `FixedArray<char>` (below) | `strings/resize_and_overwrite.h` | 1.2 (byte buffer) |
| `s.substr(pos)` that must not throw when `pos > size()` | `absl::ClippedSubstr(s, pos, n)` | `strings/string_view.h` | - |
| `std::string_view(p)` where `p` may be null | `absl::NullSafeStringView(p)` | `strings/string_view.h` | - |
| a lookup set of ASCII bytes (`strchr`, a 256-entry table) | `constexpr absl::CharSet` | `strings/charset.h` | - |
| a type printed with `operator<<` for logs/`StrCat`/`StrFormat` | `friend void AbslStringify(Sink &sink, const T &v)` (hidden friend; works in `StrCat`, `StrFormat`, `LOG`, gtest) | `strings/str_cat.h` | 1.2 (hidden friends) |

### Containers, ranges and views

| Instead of | Use | Header | Rule |
|---|---|---|---|
| `std::unordered_map` / `std::unordered_set` | `absl::flat_hash_map` / `flat_hash_set` (Swiss tables; heterogeneous string lookup by `string_view`; invalidates pointers on rehash). `node_hash_map` / `node_hash_set` only for pointer stability | `container/flat_hash_map.h`, `flat_hash_set.h`, `node_hash_map.h` | adv. |
| `std::map` / `std::set` (ordered) | `absl::btree_map` / `btree_set` (B-tree; insertions and erasures may invalidate pointers and iterators, unlike `std::map`) | `container/btree_map.h`, `btree_set.h` | adv. |
| `std::vector<T> v(n)` that is filled once and never resized | `absl::FixedArray<T> v(n)` (small arrays inline, no `push_back`) | `container/fixed_array.h` | 1.2 |
| `std::vector` that is usually tiny | `absl::InlinedVector<T, N>` | `container/inlined_vector.h` | 1.2 (named) |
| `(const T *p, size_t n)` or `const std::vector<T> &` for a read-only parameter | `absl::Span<const T>` (mutable: `Span<T>`; `MakeSpan`, `MakeConstSpan`) | `types/span.h` | adv. |
| a random-access view over a non-contiguous or projected container | `absl::AnySpan<T>` (check the cost note in the header) | `types/any_span.h` | - |
| a `const T *` parameter that is "optional" | `absl::optional_ref<const T>` (an `optional`-like view of a pointer; accepts temporaries). `docs/style.md` 1.2 prefers a reference or `absl_nullable` for a pointer; this is the third option, unruled | `types/optional_ref.h` | no ruling |
| a `std::deque` used as a FIFO | `absl::chunked_queue<T>` (tunable blocks; no random access) | `container/chunked_queue.h` | - |
| an insertion-ordered map or set (vector of pairs plus index) | `absl::linked_hash_map` / `linked_hash_set` | `container/linked_hash_map.h`, `linked_hash_set.h` | - |
| `v.erase(std::remove_if(...), v.end())` on an Abseil hash or btree container | `absl::erase_if(container, pred)` (for `std::vector` use C++20 `std::erase_if`) | each container header | - |
| `std::sort(v.begin(), v.end())` | `absl::c_sort(v)` | `algorithm/container.h` | 1.2 |
| `std::find`, `std::find_if`, `std::count`, `std::any_of`, `std::all_of`, `std::copy`, `std::equal`, `std::transform`, ... on a whole container | the `absl::c_` twin: `c_find`, `c_find_if`, `c_count`, `c_any_of`, `c_all_of`, `c_copy`, `c_equal`, `c_transform`, ... | `algorithm/container.h` | 1.2 |
| `std::find(...) != end` as a membership test | `absl::c_contains(range, value)` | `algorithm/container.h` | adv. (a `c_` form) |
| `std::binary_search` / `std::lower_bound` on a sorted vector | `absl::c_binary_search`, `c_lower_bound`, `c_upper_bound`, `c_equal_range` | `algorithm/container.h` | 1.2 |
| `std::set_intersection` and friends | `absl::c_set_intersection`, `c_set_difference`, `c_set_union` | `algorithm/container.h` | 1.2 |
| hand-written membership over a `std::initializer_list` | `absl::c_linear_search` or `absl::linear_search` | `algorithm/algorithm.h`, `algorithm/container.h` | - |

### Functions, variants, lifetime

| Instead of | Use | Header | Rule |
|---|---|---|---|
| `const std::function<R(A)> &` parameter that is only called during the call | `absl::FunctionRef<R(A)>` (non-owning, no allocation, no null state; never store it or return it) | `functional/function_ref.h` | adv. |
| a stored or owned `std::function` | `absl::AnyInvocable<R(A)>` (owning, move-only; may be `const`/`&&`-qualified: `AnyInvocable<void() &&>` for once-only) | `functional/any_invocable.h` | adv. |
| `std::bind(f, x, _1)` | `absl::bind_front(f, x)` / `absl::bind_back(f, x)`, or a lambda | `functional/bind_front.h`, `bind_back.h` | adv. |
| a hand-written visitor struct for `std::visit` | `std::visit(absl::Overload{[](A) {...}, [](const auto &) {...}}, v)` (the pin has `overload.h`) | `functional/overload.h` | - |
| a hand-written scope guard, `goto cleanup`, a destructor struct for one action | `absl::Cleanup c([&] { ... });` right after the thing it undoes; `std::move(c).Cancel()` at the commit point, `.Invoke()` to run early | `cleanup/cleanup.h` | 1.6a, 1.2 |
| a function-local `static T *x = new T` or a static with a non-trivial destructor | `static const absl::NoDestructor<T> x(args);` | `base/no_destructor.h` | adv. |
| `std::call_once` / `std::once_flag` | `absl::call_once` / `absl::once_flag` (faster; same API). dcfs is single-threaded: a function-local static is usually simpler | `base/call_once.h` | - |
| `reinterpret_cast` / `memcpy` between same-size trivially copyable types | `std::bit_cast` (`absl::bit_cast` is the same name when the standard library has it); `absl::implicit_cast` for a checked upcast | `base/casts.h` | - |
| `__builtin_popcount`, `__builtin_clz`, shift loops | `std::popcount`, `std::countl_zero`, `std::bit_width`, `std::has_single_bit` (Abseil's are the same names at C++20) | `numeric/bits.h` | - |
| `unsigned __int128` | `absl::uint128`, `absl::int128` | `numeric/int128.h` | - |
| `std::hash<T>` specialisation or a hand-written combiner (`h ^ (x + 0x9e3779b9 + ...)`) | `template <typename H> friend H AbslHashValue(H h, const T &v) { return H::combine(std::move(h), v.a, v.b); }` (hidden friend); `absl::HashOf(a, b)` for a one-off hash | `hash/hash.h` | 1.2 (hidden friends) |
| a pointer without nullability | `T *absl_nonnull`, `T *absl_nullable` (macros; this pin has no `absl::Nonnull<T>` alias) | `base/nullability.h` | 1.2 |
| `[[nodiscard]]` on a type | `ABSL_MUST_USE_RESULT` (class attribute); `[[nodiscard]]` is the dcfs spelling | `base/attributes.h` | 1.2 |
| a reference-returning accessor that can dangle | `ABSL_ATTRIBUTE_LIFETIME_BOUND` on the parameter | `base/attributes.h` | adv. |
| `std::mutex`, `std::lock_guard`, `std::condition_variable` | `absl::Mutex`, `absl::MutexLock`, `absl::CondVar`, `absl::Condition`, with `ABSL_GUARDED_BY` etc. dcfs is single-threaded today (note for the coroutine future, section 2 "synchronization") | `synchronization/mutex.h`, `base/thread_annotations.h` | no (nothing to protect) |
| a `std::thread` plus a flag to say "done" | `absl::Notification` (one event, once) | `synchronization/notification.h` | no (single-threaded) |
| `std::variant`, `std::optional`, `std::string_view`, `std::any` | **the `std::` ones**: `absl::variant`, `absl::optional`, `absl::string_view`, `absl::any` are deprecated aliases at C++20 | `types/*.h`, `strings/string_view.h` | 1.2 |

### Errors, logging, time, randomness, checksums

| Instead of | Use | Header | Rule |
|---|---|---|---|
| an `int` error code, `errno` out of a syscall wrapper, a `bool` plus an out parameter | `absl::Status` / `absl::StatusOr<T>` | `status/status.h`, `status/statusor.h` | 1.6 |
| `if (!s.ok()) return s;` | `RETURN_IF_ERROR(expr);` (short name; `ABSL_RETURN_IF_ERROR` is the long one, to be swept) | `status/status_macros.h` | 1.6 |
| `auto r = F(); if (!r.ok()) return r.status(); x = *r;` | `ASSIGN_OR_RETURN(x, F());` | `status/status_macros.h` | 1.6 |
| `absl::InternalError(absl::StrCat(...))` or any `absl::XError(...)` in production | `return InternalErrorBuilder() << "..." << x;` (dcfs helpers over `absl::StatusBuilder`, `dcfs/status.h`); `absl::StatusBuilder(absl::StatusCode::kX)` where no helper exists | `status/status_builder.h` | 1.6 |
| `absl::Status(s.code(), s.message() + "...")` to add context (drops payloads) | `absl::StatusBuilder(s) << "while ..."` or `RETURN_IF_ERROR(expr) << "..."` (keeps the errno payload) | `status/status_builder.h` | 1.6 |
| `absl::ErrnoToStatus(errno, what)` | `dcfs::ErrnoToStatus(errno, what)` (adds the `kErrnoTypeUrl` payload that `StatusToErrno` reads); never Abseil's | `status/status.h` | 1.6, `no` |
| `status.code() == absl::StatusCode::kNotFound` | `absl::IsNotFound(status)` and the other `Is...` predicates | `status/status.h` | 1.6 |
| a dropped status | `status.IgnoreError()` (on purpose, with a reason) | `status/status.h` | 1.2 |
| `assert(...)`, `abort()`, `ABSL_ASSERT`, `ABSL_HARDENING_ASSERT`, `ABSL_UNREACHABLE`, `ABSL_DIE_IF_NULL` in production | `RET_CHECK(...)` (`dcfs/ret_check.h`): returns a `kInternal` `StatusBuilder`. The Abseil crash macros are crashes (russ, 2026-10-09: "no intentional crashes"; plan 25.9) | `base/macros.h`, `log/die_if_null.h` | no |
| `CHECK` / `CHECK_EQ` / `LOG(FATAL)` in production | the same ban (25.9). `docs/style.md` 1.6 still permits `CHECK` where no `Status` can be returned (a libfuse callback, startup) until 25.9 lands; 25 sites today. Allowed in tests for setup failures | `log/check.h` | no (25.9) |
| `std::cerr <<`, `fprintf(stderr, ...)` for diagnostics | `LOG(INFO/WARNING/ERROR)`; `VLOG(n)` guarded by `VLOG_IS_ON(n)` when it builds strings | `log/log.h`, `log/vlog_is_on.h` | 1.7 |
| a log site that can fire per request | `LOG_EVERY_N(sev, n)`, `LOG_EVERY_N_SEC(sev, secs)`, `LOG_FIRST_N(sev, n)`, `LOG_EVERY_POW_2(sev)`; the `_IF_` forms add a condition. `LOG_EVERY_N_SEC` reads the clock: it is a rate limit, not a wait, and is fine under the no-timers rule | `log/log.h` | 1.7 |
| `DLOG`, `DCHECK` | none: the shipped binary is the tested one | `log/log.h` | `no` (1.7) |
| `std::chrono::steady_clock::now()`, `time(nullptr)`, `clock_gettime` for "now" | an injected `absl::Clock &` and `clock.TimeNow()` (`Context::clock`); `absl::Now()` only where nothing can be injected | `time/clock_interface.h`, `time/clock.h` | 1.10 |
| `std::chrono::milliseconds` or a raw `int ms` in a signature | `absl::Duration` (`absl::Seconds(5)`, `ToInt64Milliseconds`, arithmetic, `absl::InfiniteDuration()`); `absl::FromChrono` / `ToChronoMilliseconds` only at a foreign boundary | `time/time.h` | adv. |
| `timespec`/`timeval` arithmetic by hand | `absl::DurationFromTimespec`, `ToTimespec`, `TimeFromTimespec`, `ToTimeval`, `absl::FromUnixNanos` / `ToUnixNanos` | `time/time.h` | adv. |
| `sleep`, `usleep`, `absl::SleepFor`, `absl::Clock::Sleep`, `SleepUntil`, `AwaitWithDeadline`, `Mutex::AwaitWithTimeout` | none: wait on the event (`docs/style.md` 1.11). `Duration` arithmetic is fine; anything that blocks for a duration is banned | `time/clock.h`, `time/clock_interface.h` | no (1.11) |
| a hand-written fake clock | `absl::SimulatedClock` (advance with `AdvanceTime`) | `time/simulated_clock.h` | 1.10 |
| `rand()`, `std::mt19937`, `std::uniform_int_distribution` | `absl::BitGen gen; absl::Uniform<int>(gen, lo, hi)`; take an `absl::BitGenRef` parameter; `absl::MockingBitGen` in tests. Not cryptographic | `random/random.h`, `random/distributions.h`, `random/bit_gen_ref.h` | - |
| a hand-written CRC or checksum | `absl::ComputeCrc32c(sv)`, `ExtendCrc32c`, `ConcatCrc32c`, `absl::crc32c_t` | `crc/crc32c.h` | - |
| large shared or spliced buffers (RPC-style) | `absl::Cord` -- and not for ordinary strings: see section 2, "strings" | `strings/cord.h` | adv. |
| `ASSERT_TRUE(s.ok())`, `EXPECT_EQ(s.code(), ...)` | `EXPECT_THAT(s, IsOk())`, `IsOkAndHolds(m)`, `StatusIs(code, msg_matcher)` (`ABSL_EXPECT_OK`, `ABSL_ASSERT_OK` also exist; there is no `ASSERT_OK_AND_ASSIGN` in the pin) | `status/status_matchers.h` | 2 (tests) |

### Names to re-verify when the pin moves

The swaps above lean on names that are easy to lose or rename:
`absl/status/status_builder.h`, `status_macros.h` (with
`ABSL_DEFINE_UNQUALIFIED_STATUS_MACROS`), `functional/overload.h`,
`strings/resize_and_overwrite.h`, `types/optional_ref.h`,
`types/any_span.h`, `container/linked_hash_map.h`, `chunked_queue.h`,
`time/clock_interface.h`, `time/simulated_clock.h`, `strings/charset.h`, the
`absl_nonnull` / `absl_nullable` macros, `absl::c_contains`,
`absl::erase_if`. All were present in 20260817.0 (checked 2026-10-10).
Not in this pin: `absl::Nonnull<T>` / `Nullable<T>` template aliases (only
the macros), `absl::InternalErrorBuilder` and its siblings (dcfs defines
them in `dcfs/status.h`), `ASSERT_OK_AND_ASSIGN` (dcfs defines it in
`dcfs/testonly/assert_ok_and_assign.h`).

## 2. By directory

For each directory: the headers that matter, one sentence each, and the two or
three entry points a dcfs coder would reach for. "dcfs uses" is the number of
files that include the header: prod / tests (counted at `89b917c`). Internal
(`internal/`) and test-only headers are skipped.

### algorithm

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `algorithm/container.h` | `<algorithm>`, `<numeric>` and `<iterator>` for whole containers: `c_sort`, `c_find`, `c_find_if`, `c_contains`, `c_copy`, `c_equal`, `c_any_of`/`c_all_of`/`c_none_of`, `c_count`, `c_binary_search`, `c_lower_bound`, `c_accumulate`, `c_reverse`, `c_unique_copy`, `c_set_*`, `c_shuffle`. Rule 1.2: a bare `std::` algorithm on `begin()`/`end()` is a finding | 0 / 0 (rule; sweep is 25.17) |
| `algorithm/algorithm.h` | Small extensions: `absl::linear_search(first, last, v)`, `absl::equal`, `absl::rotate` (C++14 shims) | 0 / 0 |

### base

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `base/nullability.h` | Pointer nullability annotations: `absl_nonnull`, `absl_nullable` (and `absl_nullability_unknown`, which dcfs does not use). Rule 1.2; today 0 uses in `dcfs/` (plan 25.16) | 0 / 0 |
| `base/no_destructor.h` | `absl::NoDestructor<T>`: a static that is never destroyed, safely lazy as a function-local static. `static const absl::NoDestructor<std::string> x("foo"); return *x;` | 1 / 0 |
| `base/call_once.h` | `absl::call_once(flag, fn, args...)`, `absl::once_flag`. Threads only | 0 / 0 |
| `base/casts.h` | `absl::implicit_cast<T>(x)`; `absl::bit_cast<Dest>(src)` (= `std::bit_cast`) | 0 / 0 |
| `base/attributes.h` | Portable attributes: `ABSL_MUST_USE_RESULT`, `ABSL_ATTRIBUTE_LIFETIME_BOUND`, `ABSL_DEPRECATED("...")`, `ABSL_FALLTHROUGH_INTENDED`, `ABSL_CONST_INIT`, `ABSL_REQUIRE_EXPLICIT_INIT`, `ABSL_PRINTF_ATTRIBUTE`, `ABSL_ATTRIBUTE_NOINLINE`, `ABSL_ATTRIBUTE_COLD`. Prefer the C++ spelling (`[[nodiscard]]`, `[[fallthrough]]`) where one exists | 0 / 0 |
| `base/macros.h` | `ABSL_ARRAYSIZE`, `ABSL_ASSERT`, `ABSL_HARDENING_ASSERT` (a crash: banned in production, 25.9), `ABSL_DEPRECATE_AND_INLINE` | 0 / 0 |
| `base/optimization.h` | `ABSL_PREDICT_TRUE` / `ABSL_PREDICT_FALSE`, `ABSL_ASSUME`, `ABSL_UNREACHABLE` (undefined behaviour or a crash if reached: rule 1.10a says name the cause with `RET_CHECK` instead), `ABSL_CACHELINE_SIZE` | 1 / 0 |
| `base/log_severity.h` | `absl::LogSeverity`, `LogSeverityName`, `NormalizeLogSeverity` (used by dcfs's `LogSink`s) | 3 / 3 |
| `base/config.h`, `base/options.h`, `base/port.h` | Compiler/platform feature macros and Abseil's own build options: not for dcfs code | 0 / 0 |
| `base/thread_annotations.h` | `ABSL_GUARDED_BY`, `ABSL_EXCLUSIVE_LOCKS_REQUIRED`, `ABSL_LOCKS_EXCLUDED`, `ABSL_NO_THREAD_SAFETY_ANALYSIS`: Clang `-Wthread-safety` annotations. Needed when dcfs gets threads | 0 / 0 |
| `base/prefetch.h`, `base/const_init.h`, `base/dynamic_annotations.h`, `base/fast_type_id.h`, `base/throw_delegate.h` | Cache prefetch, constant-initialisation tag, sanitizer annotations, type ids, exception delegation: not for dcfs code | 0 / 0 |

### cleanup

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `cleanup/cleanup.h` | `absl::Cleanup` (scope guard; CTAD: `absl::Cleanup c([&] {...});`), `absl::MakeCleanup(fn)`, `std::move(c).Cancel()`, `std::move(c).Invoke()`. Rules 1.6a, 1.2 | 4 / 0 |

### container

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `container/flat_hash_map.h`, `flat_hash_set.h` | The default hash map and set (replace `std::unordered_*`): `find`, `contains`, `try_emplace`, `insert_or_assign`, `erase`, `absl::erase_if`; heterogeneous lookup (`std::string` keys, `string_view` lookups); a reservation size, not a bucket count; invalidates pointers on rehash and move | 7+4 / 2+6 |
| `container/node_hash_map.h`, `node_hash_set.h` | Same API with pointer stability, at the cost of an allocation per element; use only when a pointer to an element must survive growth | 0 / 0 |
| `container/btree_map.h`, `btree_set.h` | Ordered containers (replace `std::map`/`std::set`) (the header calls them more efficient in most situations); `btree_multimap`, `btree_multiset`; no pointer stability | 0 / 0 |
| `container/fixed_array.h` | `absl::FixedArray<T, N>`: size chosen at construction, inline when small. Rule 1.2 | 4 / 0 |
| `container/inlined_vector.h` | `absl::InlinedVector<T, N>`: a vector with inline storage for the first N elements | 0 / 0 |
| `container/linked_hash_map.h`, `linked_hash_set.h` | Insertion-ordered map/set with O(1) lookup and stable iterators. Candidate where a table plus an order must be kept together | 0 / 0 |
| `container/chunked_queue.h` | `absl::chunked_queue<T>`: a FIFO with tunable block size and no random access (a lighter `std::deque`) | 0 / 0 |
| `container/hash_container_defaults.h` | `absl::DefaultHashContainerHash<T>` / `DefaultHashContainerEq<T>`: the hash and equality a hash container picks, for generic code | 0 / 0 |

### crc

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `crc/crc32c.h` | CRC32C: `absl::ComputeCrc32c(sv)`, `ExtendCrc32c(crc, sv)`, `ConcatCrc32c(a, b, len_b)`, `RemoveCrc32cPrefix` / `Suffix`, `MemcpyCrc32c` (copy and checksum in one pass); the value type is `absl::crc32c_t` (printable, `AbslStringify`) | 0 / 0 (a candidate for any future integrity check of the cache database) |

### debugging

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `debugging/failure_signal_handler.h` | `absl::InstallFailureSignalHandler(options)`: prints a stack trace on SIGSEGV, SIGABRT, ... Needs `InitializeSymbolizer` first | 0 / 0 |
| `debugging/symbolize.h`, `debugging/stacktrace.h` | `absl::InitializeSymbolizer(argv[0])`, `Symbolize(pc, buf, n)`; `GetStackTrace(...)` | 0 / 0 |
| `debugging/leak_check.h` | LeakSanitizer controls: `HaveLeakSanitizer`, `DoIgnoreLeak(p)`, `FindAndReportLeaks`. Sanitizer builds only | 0 / 0 |

### flags

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `flags/flag.h`, `flags/declare.h` | `ABSL_FLAG(type, name, default, help)`, `ABSL_DECLARE_FLAG`, `absl::GetFlag(FLAGS_x)`, `SetFlag` | 1 / 1 |
| `flags/parse.h` | `absl::ParseCommandLine(argc, argv)`: returns the positional arguments | 1 / 0 |
| `flags/usage.h`, `flags/usage_config.h` | `SetProgramUsageMessage`, `ProgramUsageMessage`, `SetFlagsUsageConfig` | 1+1 / 0 |
| `flags/marshalling.h` | Support for custom flag types (`AbslParseFlag` / `AbslUnparseFlag`); `absl::Duration` and `absl::Time` already work as flags | 0 / 0 |
| `flags/reflection.h`, `flags/commandlineflag.h` | `absl::FindCommandLineFlag(name)`, `FlagSaver` (a test restores flags on scope exit) | 2 / 0 |

### functional

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `functional/function_ref.h` | `absl::FunctionRef<R(A...)>`: a non-owning callable parameter. Not a member, not a return value | 6 / 3 |
| `functional/any_invocable.h` | `absl::AnyInvocable<R(A...) [const] [&&]>`: an owning, move-only callable (a stored callback) | 0 / 0 |
| `functional/bind_front.h`, `bind_back.h` | `absl::bind_front(f, a...)`, `bind_back`: partial application without placeholders | 0 / 0 |
| `functional/overload.h` | `absl::Overload{lambdas...}`: an anonymous visitor for `std::visit`. Prefer named overloads if the visitor is larger than a few lines | 0 / 0 |

### hash

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `hash/hash.h` | The hashing framework: `absl::Hash<T>`, the `AbslHashValue(H h, const T &)` extension point with `H::combine` and `H::combine_contiguous`, `absl::HashOf(a, b, ...)`, `absl::HashState` (type-erased). A hash differs from process to process: never store or send one. Rule 1.2 (hidden friend, `FileHandle::AbslHashValue`) | 0 / 2 (`FileHandle` defines `AbslHashValue`) |

### log

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `log/log.h` | `LOG(sev)`, `LOG_IF`, `PLOG` (appends errno), `VLOG(n)`, the rate-limited `LOG_EVERY_N`, `LOG_EVERY_N_SEC`, `LOG_FIRST_N`, `LOG_EVERY_POW_2`. `DLOG` is not used. Rule 1.7 | 13 / 3 |
| `log/vlog_is_on.h` | `VLOG_IS_ON(n)`: guard a log line that builds strings | 0 / 0 (via `log.h`) |
| `log/check.h` | `CHECK`, `CHECK_EQ`..., `CHECK_OK`, `QCHECK`, `PCHECK`. Crashes: production use is being removed (25.9, 25 sites in `dcfs/`); fine for test setup | 5 / 6 |
| `log/initialize.h`, `log/globals.h` | `absl::InitializeLog()`, `SetStderrThreshold`, `SetMinLogLevel`: `main.cc` sets the WARNING default | 1 / 1 |
| `log/log_sink.h`, `log_sink_registry.h`, `log_entry.h` | `absl::LogSink::Send(const LogEntry &)`, `AddLogSink`, `RemoveLogSink`: dcfs's syslog sink is one | 2+1+3 / 4+4+3 |
| `log/scoped_mock_log.h` | `absl::ScopedMockLog` for gMock expectations on log lines (tests) | 0 / 1 |
| `log/log_streamer.h`, `log/structured.h`, `log/flags.h`, `log/die_if_null.h` | Stream into a callback as a log line; structured fields; flag plumbing; `ABSL_DIE_IF_NULL` (a crash: banned in production) | 0 / 0 |
| `log/absl_log.h`, `log/absl_check.h`, `log/absl_vlog_is_on.h` | The `ABSL_`-prefixed spellings of the macros above, for headers that must not define `LOG`/`CHECK`; dcfs uses the short names | 0 / 0 |

### memory

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `memory/memory.h` | `absl::WrapUnique(raw)` (adopt a raw pointer from a C-style factory; not for `new T`: use `std::make_unique`), `RawPtr`, `ShareUniquePtr`, `WeakenPtr`; `absl::make_unique` is a deprecated alias | 0 / 0 |

### meta

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `meta/type_traits.h` | Mostly deprecated aliases of `std::<type_traits>` (`absl::conjunction`, `void_t`, `add_const_t`, ...: use `std::`), plus `absl::is_detected`-style helpers. Template metaprogramming: avoid unless a template needs it | 0 / 0 |

### numeric

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `numeric/bits.h` | C++20 `<bit>`: `popcount`, `countl_zero`, `countr_zero`, `bit_width`, `has_single_bit`, `bit_ceil`, `rotl`, `rotr`, `absl::endian` (the `std::` names at C++20) | 0 / 0 |
| `numeric/int128.h` | `absl::uint128`, `absl::int128`, `MakeUint128`, `Uint128High64`/`Low64` | 0 / 0 |

### profiling

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `profiling/hashtable.h` | `absl::MarshalHashtableProfile()`: a pprof profile of sampled Abseil hash tables. Diagnostics only | 0 / 0 |

### random

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `random/random.h` | `absl::BitGen` (general), `absl::InsecureBitGen` (faster, weaker): not cryptographic | 1 / 4 |
| `random/distributions.h` | `absl::Uniform<T>(gen, lo, hi)`, `Bernoulli(gen, p)`, `Exponential`, `Gaussian`, `Zipf`, `LogUniform` | 1 / 0 |
| `random/bit_gen_ref.h` | `absl::BitGenRef`: a non-owning parameter that takes any bit generator | 1 / 0 |
| `random/seed_sequences.h`, `random/mocking_bit_gen.h` | `CreateSeedSeqFrom`; `absl::MockingBitGen` (gMock-driven values for tests) | 0 / 0 |
| `random/uniform_int_distribution.h` and the other `*_distribution.h` | The distribution classes behind `distributions.h`; `absl::uniform_int_distribution` is a faster drop-in for the `std::` one | 0 / 0 |

### status

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `status/status.h` | `absl::Status`, `absl::StatusCode`, `OkStatus()`, `absl::XError(msg)` constructors (not in production code: use the builders), `absl::IsNotFound(s)` and the other `Is...`, `StatusCodeToString`, `Status::SetPayload`/`GetPayload`/`IgnoreError`, `absl::ErrnoToStatus` (dcfs's own wrapper is used instead) | 46 / 33 |
| `status/statusor.h` | `absl::StatusOr<T>`: `ok()`, `status()`, `*`/`->`, `value_or`, `emplace` | 48 / 26 |
| `status/status_builder.h` | **Present in this pin.** `absl::StatusBuilder(code or status, loc)` with `<<` for context, `SetPrepend`, `SetAppend`, `SetPayload`, `With(policy)`, and the logging methods `Log`, `LogError`, `LogEveryN`, `VLog`, `EmitStackTrace` (rule: return or log, not both, 1.7). dcfs's `...ErrorBuilder()` helpers (`dcfs/status.h`) and `RET_CHECK` return one | 7 / 1 |
| `status/status_macros.h` | **Present in this pin.** `ABSL_RETURN_IF_ERROR(expr) << "..."`, `ABSL_ASSIGN_OR_RETURN(lhs, expr)`; the unqualified `RETURN_IF_ERROR` / `ASSIGN_OR_RETURN` appear when `ABSL_DEFINE_UNQUALIFIED_STATUS_MACROS` is defined (`//dcfs:status` defines it). Rule 1.6 | 18 / 7 (almost all through the long names: 710 + 127 uses) |
| `status/status_matchers.h` | Test matchers: `IsOk()`, `IsOkAndHolds(m)`, `StatusIs(code, msg)`, `CanonicalStatusIs`, `ABSL_EXPECT_OK`, `ABSL_ASSERT_OK`. Rule 2 | 0 / 28 |
| `status/status_payload_printer.h` | A hook to print payloads in `Status::ToString` | 0 / 0 |

### strings

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `strings/str_cat.h` | `StrCat`, `StrAppend`, `Hex`, `Dec`, `HighPrecision`; the `AbslStringify` customisation point. Not for building statuses (rule 1.6: a builder) | 18 / 14 |
| `strings/str_format.h` | `StrFormat`, `StrAppendFormat`, `PrintF`, `FPrintF`, `SNPrintF`, `FormatStreamed`; `AbslFormatConvert` for custom conversions | 7 / 1 |
| `strings/substitute.h` | `Substitute("$1 and $0", a, b)`, `SubstituteAndAppend` | 0 / 0 |
| `strings/str_split.h` | `StrSplit(sv, delimiter [, predicate])`; delimiters `ByChar`, `ByString`, `ByAnyChar`, `ByLength`, `ByAsciiWhitespace`; predicates `SkipEmpty`, `SkipWhitespace`, `AllowEmpty`; `MaxSplits(d, n)` | 6 / 4 |
| `strings/str_join.h` | `StrJoin(range, sep [, formatter])`, `AlphaNumFormatter`, `StreamFormatter` | 8 / 3 |
| `strings/match.h` | `StrContains`, `StartsWith`, `EndsWith`, and the `...IgnoreCase` forms | 3 / 2 |
| `strings/strip.h` | `ConsumePrefix(&sv, p)`, `ConsumeSuffix`, `StripPrefix`, `StripSuffix` | 2 / 1 |
| `strings/ascii.h` | Locale-free `ascii_is*`/`ascii_to*`, `AsciiStrToLower`/`Upper`, `StripAsciiWhitespace` and its leading/trailing forms | 1 / 0 |
| `strings/numbers.h` | `SimpleAtoi`, `SimpleAtof`, `SimpleAtod`, `SimpleAtob`, `SimpleHexAtoi`: string to number, bool result | 8 / 1 |
| `strings/escaping.h` | `CEscape`, `CHexEscape`, `CUnescape`, `Base64Escape`/`Unescape`, `BytesToHexString`, `HexStringToBytes`, `UrlEscape`/`Unescape` | 0 / 0 |
| `strings/str_replace.h` | `StrReplaceAll(s, {{from, to}, ...})` (copy) and the in-place overload | 2 / 0 |
| `strings/string_view.h` | `absl::string_view` is `std::string_view` (use `std::`); `ClippedSubstr`, `NullSafeStringView` are the extras | 1 / 0 (an unused include) |
| `strings/charset.h` | `absl::CharSet`: a `constexpr` bit set of bytes | 0 / 0 |
| `strings/charconv.h` | `absl::from_chars` for `float`/`double` (a C++17 polyfill) | 0 / 0 |
| `strings/resize_and_overwrite.h` | `absl::StringResizeAndOverwrite(str, n, op)` (C++23 `resize_and_overwrite`) | 0 / 0 |
| `strings/cord.h`, `cord_buffer.h` | `absl::Cord`: a rope of reference-counted chunks, cheap to append, prepend and share, slow for random access. Use for large payloads that are spliced or shared across an API; **not** for ordinary strings (construction overhead). `absl::CopyCordToString`, `Cord::Flatten`, `CordBuffer` for building one in place. dcfs uses it only because `Status::SetPayload` takes a `Cord` (`dcfs/status.cc`, `sqlite.cc`, `backing_capture.cc`) | 4 / 1 |
| `strings/has_absl_stringify.h`, `has_ostream_operator.h` | Detection traits: not for dcfs code | 0 / 0 |

### synchronization

dcfs is single-threaded today (`docs/design.md`), so none of this is used. It
is listed for the coroutine and multi-thread future (`docs/design.md`,
"Concurrency, today and with coroutines"): when threads arrive, annotate with
`base/thread_annotations.h` from the first line, and the contention rules of
`docs/style.md` 1.12 apply (a mutex or unbounded optimistic retry, never a
bounded one).

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `synchronization/mutex.h` | `absl::Mutex` (exclusive and shared locks, deadlock detection, `Condition` predicates), `MutexLock`, `ReaderMutexLock`, `WriterMutexLock`, `CondVar`. `Mutex::Await...WithTimeout` and `CondVar::WaitWithTimeout` are timers (1.11) | 0 / 0 |
| `synchronization/notification.h` | `absl::Notification`: one event, once (`Notify`, `WaitForNotification`, `HasBeenNotified`); the timed waits are timers (1.11) | 0 / 0 |
| `synchronization/barrier.h`, `blocking_counter.h` | `Barrier`, `BlockingCounter`: rendezvous primitives | 0 / 0 |

### time

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `time/time.h` | `absl::Duration` (`Seconds`, `Milliseconds`, ..., arithmetic, `ToInt64...`, `ToDouble...`, `FormatDuration`, `ParseDuration`, `InfiniteDuration`), `absl::Time` (`FromUnixNanos`, `ToUnixNanos`, `InfiniteFuture`, `UnixEpoch`, `FormatTime`, `ParseTime`), `TimeZone`, the `timespec`/`timeval` converters, `FromChrono`/`ToChrono...`. Duration arithmetic is fine under the no-timers rule | 7 / 2 |
| `time/clock_interface.h` | `absl::Clock` (interface): `TimeNow()`, plus `Sleep`, `SleepUntil` and `AwaitWithDeadline`, **which dcfs does not call** (1.11). `Context::clock` is an `absl::Clock *` (1.10) | 1 / 0 |
| `time/simulated_clock.h` | `absl::SimulatedClock`: the fake clock for tests (`AdvanceTime`, `SetTime`) | 0 / 1 |
| `time/clock.h` | `absl::Now()`, `GetCurrentTimeNanos()`, `absl::SleepFor` (banned, 1.11). Prefer the injected `Clock` | 0 / 0 |
| `time/civil_time.h` | `CivilDay`, `CivilSecond`...: calendar arithmetic, with a time zone | 0 / 0 |

### types

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `types/span.h` | `absl::Span<T>`, `Span<const T>`, `MakeSpan`, `MakeConstSpan`: a view of contiguous elements (compare `std::span`, which the header lists the differences from) | 0 / 0 |
| `types/source_location.h` | `absl::SourceLocation::current()` as a defaulted parameter, for builders that record the caller's line (`dcfs/status.h` uses it) | 5 / 3 |
| `types/optional_ref.h` | `absl::optional_ref<T>`: an optional-like view of a pointer that accepts rvalues | 0 / 0 |
| `types/any_span.h` | `absl::AnySpan<T>`: a view over any random-access container, with element projection | 0 / 0 |
| `types/optional.h`, `variant.h`, `any.h`, `compare.h` | Deprecated aliases of `std::optional`, `std::variant`, `std::any`, the ordering types: use `std::`. Rule 1.2 | 1 / 0 (an unused include of `optional.h`) |

### utility

| Header | Purpose and entry points | dcfs uses |
|---|---|---|
| `utility/utility.h` | Deprecated aliases of `std::exchange`, `std::apply`, `std::in_place_*`, `std::index_sequence`: use `std::` | 0 / 0 |

## 3. For reviewers

Ten questions for a diff. Each is advisory unless it also breaks a rule named
in the swaps table (the column "Rule"); cite the table row or the Tip.

1. **A string built by `+`, `+=` chains or `std::to_string`?** `StrCat` /
   `StrAppend`; `StrFormat` if it needs widths or hex. (An error message is a
   `StatusBuilder`, not a `StrCat`: 1.6.)
2. **A loop over a whole container with an iterator pair
   (`std::sort(v.begin(), v.end())`, `std::find`, `std::copy`)?** `absl::c_*`
   (1.2; the repo_shape check is plan step 25.17, not yet in the tree).
3. **A `std::function` parameter that is only called during the call?**
   `absl::FunctionRef`. A stored callback: `absl::AnyInvocable`.
4. **A hand-written `find`/`substr` split, join loop, trim, prefix or suffix
   test, `strtoul`/`atoi`?** `StrSplit`, `StrJoin`, `StripAsciiWhitespace`,
   `StartsWith`/`ConsumePrefix`/`StrContains`, `SimpleAtoi`.
5. **A hand-written hex table, escape, base64 or CRC?**
   `BytesToHexString`, `CEscape`/`CHexEscape`, `Base64Escape`,
   `ComputeCrc32c` (but names in output go through `dcfs::EscapeBytes`).
6. **A `std::vector<T> v(n)` that is never resized?** `FixedArray` (1.2). A
   vector that is nearly always tiny: `InlinedVector`.
7. **`std::unordered_map`, `std::map`, `std::set`?** `flat_hash_map` /
   `flat_hash_set` / `btree_map`; check whether the code holds a pointer into
   the container across an insert (flat and btree move elements).
8. **A pointer or pointer-and-length parameter?** A reference, `absl::Span`,
   or `absl_nonnull`/`absl_nullable` on every pointer (1.2).
9. **A crash, a clock or a wait?** `CHECK`, `LOG(FATAL)`, `assert`,
   `ABSL_UNREACHABLE` in production (25.9: `RET_CHECK`); `std::chrono`,
   `absl::Now()` or `SleepFor` where an injected `Clock` or an event wait
   belongs (1.10, 1.11).
10. **A hand-written error plumbing, scope guard or hash?**
    `if (!s.ok()) return s;` (`RETURN_IF_ERROR`, short name), a status
    rebuilt with `absl::Status(code, msg)` (use `StatusBuilder`: it keeps the
    errno payload), `absl::XError(StrCat(...))` (use the builder helpers), a
    `goto`/destructor scope guard (`absl::Cleanup`), a `std::hash`
    specialisation (`AbslHashValue` as a hidden friend).

Also glance for: a stale `absl/types/optional.h` or
`absl/strings/string_view.h` include (the aliases are deprecated), a
`std::mutex` or `std::thread` in production (the process is single-threaded;
ask why), and `LOG` without a rate limit on a per-request path (1.7).

## 4. Candidates for a sweep

What the tree still hand-rolls, measured at `89b917c` (2026-10-10) with the
commands below, so a follow-up step can act on each row. A row is a candidate,
not a defect: the last column says what to check first. No code was changed
by this document. Counts are lines matched, then files.

Scope for the commands (run from the repository root):

```bash
# prod: dcfs/ and bench/ and tools/ C++, not tests, not dcfs/testonly/
git ls-files 'dcfs/*' 'tools/*' 'bench/*' 'test/*' | grep -E '\.(cc|h)$' \
  | grep -v -E '_test\.cc$|/testonly/' > /tmp/prod.txt
git ls-files 'dcfs/*' 'test/*' | grep -E '\.(cc|h)$' \
  | grep -E '_test\.cc$|/testonly/' > /tmp/tests.txt
xargs grep -nE 'PATTERN' < /tmp/prod.txt     # the pattern is in the row
```

| # | Left-hand pattern (grep -E) | prod | tests | Sites and what to check |
|---|---|---|---|---|
| 1 | string built by `+` with a literal: `" *\+ *[a-z_]\|[a-z_)] *\+ *"` | 21 lines, 4 files (one more is a comment) | 29 lines, 9 files | `bench/dcfs_bench.cc` 15, `bench/dm_delay.cc` 2, `bench/process.cc` 3, `dcfs/umount_helper.cc:101` (the only one in the daemon). `StrCat`. Tests: `syscalls_test.cc` 12, `absolute_paths_test.cc` 6, `umount_helper_test.cc` 5 |
| 2 | `std::to_string` | 3 lines, 2 files | 11 | `bench/dcfs_bench.cc:305,308`, `bench/process.cc:111`: drop into `StrCat`. Tests: `syscalls_test.cc` 6, `umount_helper_test.cc` 3 |
| 3 | `+=` of a literal or `std::string`: `\+= *(std::to_string\|"\|std::string)` | 7 lines, 2 files | 0 | `dcfs/escape.cc` (6: the byte escaper, see row 11), `bench/process.cc:52` |
| 4 | `snprintf`/`sprintf`/`printf`: `\bs?n?printf\(` | 8 lines, 4 files | 1 | `bench/dm_delay.cc` 4, `bench/tree.cc` 2, `bench/dcfs_bench.cc:440`, `test/qemu/testdata/cov_fixture.cc:11` (a fixture whose output a test reads: leave). `StrFormat`; the `dm_delay.cc` ones fill fixed-size C structs, so `snprintf` into a field may stay |
| 5 | `strtoull`/`atoi`/`std::stoi`: `\b(std::sto(i\|l\|ul\|ll\|ull\|d\|f)\|atoi\|atol\|strtoul?l?\|strtod)\(` | 8 lines, 2 files | 0 | `bench/dcfs_bench.cc` 7 (flag parsing: `SimpleAtoi` also reports a bad number, `strtoull` silently yields 0), `bench/process.cc:115` |
| 6 | `.find(x) != npos`: `find\([^)]*\) *(==\|!=) *(std::)?(string\|string_view)::npos` | 4 lines, 3 files | 1 | `bench/process.cc:36,37`, `dcfs/fsck.cc:159`, `dcfs/mount_dcfs.cc:115`: `absl::StrContains` |
| 7 | prefix by `substr` compare (my pattern missed it; found by reading): `substr\(0, [^)]*\) *==` | 1 | - | `dcfs/mounts_below.cc:103`: `absl::StartsWith(mount_point, prefix)` (the line above already checks the size) |
| 8 | hand-written split by `find`/`substr` | 3 sites | - | `dcfs/backing.cc:146` `SplitXattrList` (NUL-separated: `absl::StrSplit(buf, '\0')` leaves a trailing empty piece the loop does not, so add `absl::SkipEmpty()` and check that an empty name cannot occur), `dcfs/mount_dcfs.cc:111,243-248` (a `name[=value]` split where "no `=`" differs from "empty value"; `StrSplit` into a pair would lose that: look before changing), `bench/tree.cc:40,91` |
| 9 | `std::function`: `std::function` | 2 lines, 1 file | 29 lines, 3 files | `dcfs/session_loop.h:50,64`: a stored one-shot callback, `absl::AnyInvocable<void()>`. Tests: `dir_cache_fs_test.cc` 24 (hooks held in statics), `testonly/invariant_checker.h` 4, `startup_channel_fault_test.cc` 1: hooks that are stored are `AnyInvocable`; ones only passed down are `FunctionRef` |
| 10 | `std::map`/`std::set`: `std::(map\|set\|multimap\|multiset)<` | 1 line | 21 lines, 5 files | `bench/dcfs_bench.cc:68` (`by_name`); tests: `testonly/trace_recorder.h` 12, `dir_cache_fs_test.cc` 5. `btree_map` if the order is needed, else `flat_hash_map` |
| 11 | hand-rolled hex: `0123456789abcdef\|%0?[0-9]*l?l?[xX]\|std::hex` | 3 lines, 3 files | 1 | `dcfs/escape.cc:11` (`kHexDigits`, the byte escaper: not identical to `CHexEscape`, which also escapes `'`; keep unless russ rules otherwise), `dcfs/file_handle.cc:109-112` (a `StrAppendFormat("%02x")` loop: `absl::BytesToHexString` after a `string_view` cast), `dcfs/device_id.cc:60` (a dashed 16-byte UUID: no one-call equivalent; leave) |
| 12 | iterator-pair `std::` algorithms: `std::(copy\|sort\|find\|find_if\|any_of\|all_of\|none_of\|count\|count_if\|equal\|fill\|reverse\|remove_if\|unique\|transform\|accumulate\|binary_search\|lower_bound\|upper_bound\|for_each)\(` | 11 lines, 6 files | 12 lines, 5 files | Prod: `backing.cc:1133`, `device_id.cc:47,157,189`, `dir_cache_fs.cc:2547,2559`, `file_handle.cc:184`, `metadata_cache.cc:1864,1870`, `mount_dcfs.cc:286,341`. Rule 1.2, which 25.17 enforces with a shrinking allowlist (six sites); `device_id.cc:47` is a deliberate sub-range |
| 13 | `std::vector<T> name(n)`: `std::vector<[^;]*> +[a-z_]+\([^)]+\);` | 5 lines, 4 files (two are `words(argv+1, argv+argc)`, a range, not a buffer) | 7 | `bench/dcfs_bench.cc:87`, `bench/dm_delay.cc:99` (`FixedArray<char>`; 25.12's check covers `dcfs/` only, so `bench/` is open), `dcfs/fuse_request.cc:178` (resized after: the allowlisted one) |
| 14 | `std::chrono`, `<chrono>` | 0 | 7 lines, 1 file | `dcfs/dir_cache_fs_test.cc:7560-7583`: a `steady_clock` stopwatch in one test; `absl::Now()` and `absl::Duration`, or the test's injected clock |
| 15 | `std::unordered_*`, `std::call_once`, `std::once_flag`, `std::hash`, `hash_combine`, `std::visit`, `std::variant`, `std::bind`, `std::ostringstream`, `std::mt19937`, `rand(`, `__builtin_popcount\|clz\|ctz`, `assert(`, `isspace\|tolower\|...` | 0 | 0 | none: nothing to sweep |
| 16 | `std::mutex`, `std::thread`, `std::atomic` | 0 | 1 | `dcfs/backing_test.cc:1417`: a test thread; nothing to sweep |
| 17 | function-local static leaked with `new`: `static [^=]*= *new ` | 0 | 10 lines, 3 files | `dcfs/backing_fault_test.cc:72`, `device_id_fault_test.cc:59`, `dir_cache_fs_test.cc` (8, e.g. 126, 132): `static absl::NoDestructor<T>` (fault-injection state) |
| 18 | `CHECK` family in production: `(^\|[^_A-Za-z])(CHECK\|CHECK_EQ\|CHECK_NE\|CHECK_GT\|CHECK_GE\|CHECK_LT\|CHECK_LE\|CHECK_OK\|QCHECK\|PCHECK)\(` | 27 lines, 4 files | 51 | `dcfs/fuse_ops.cc` 23 (nearly all `CHECK_NE(ptr, nullptr)`: the `absl_nonnull`/reference work of 25.16 removes them), `backing.cc:281`, `fuse_request.cc:276`; plus `tools/mutation/fixture.cc` (a fixture with its own `CHECK`). This is plan 25.9, not a new sweep |
| 19 | long-name Status macros: `ABSL_RETURN_IF_ERROR\|ABSL_ASSIGN_OR_RETURN` | 710 uses | 127 | `docs/style.md` 1.6 (25.17 per the style text): the short names are the rule; 2 short uses exist today |
| 20 | `absl::XError(...)` in production: `absl::[A-Z][a-z]+Error\(` | 1 (`tools/mutation/fixture.cc:99`, a fixture) | 33 | 1.6: builders in production; tests may use the plain constructors |
| 21 | alias headers still included: `absl/types/optional.h`, `absl/strings/string_view.h` | 2 includes | 0 | `dcfs/file_handle.cc:20` and `dcfs/main.cc:41`: no `absl::optional` or `absl::string_view` is used in the file; delete (style 1.2 says none are left) |
| 22 | pointers without `absl_nonnull`/`absl_nullable` | 0 annotations in `dcfs/` | - | plan 25.16 |
| 23 | `fprintf(stderr, ...)` / `std::cout` / `std::cerr` | 13 lines, 6 files | 4 | `bench/` 7 (`fprintf`), `dcfs/fsck.cc:418,458,472`, `dcfs/main.cc:91,630,733`. The `dcfs` ones are the helpers' own output (`--version`, usage, the fsck report), which the caller reads: not log lines, so not a `LOG` candidate. `bench/` diagnostics could be `LOG(ERROR)`; ask before changing |
| 24 | `memcpy` | 8 lines, 4 files | 25 | `backing_capture.cc`, `dir_cache_fs.cc`, `fuse_ops.cc`, `session_loop.cc`: struct to byte-buffer copies of different sizes (cmsg data, the FUSE header, a partial copy with `std::min`): `std::bit_cast` does not fit; nothing to sweep |

What dcfs already uses from Abseil is the "dcfs uses" column of section 2
(prod files / test files that include each header). Abseil headers dcfs never
includes, in prod or tests: `algorithm/container.h` (rule 1.2 is new; the
sweep is 25.17), `container/btree_*`, `inlined_vector.h`, `types/span.h`,
`functional/any_invocable.h`, `bind_front.h`, `overload.h`,
`strings/substitute.h`, `strings/escaping.h`, `crc/crc32c.h`,
`synchronization/*` and `base/nullability.h` (25.16).
