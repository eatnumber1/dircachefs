# Index of style rules

Read this before you write or review any code or document: naming, errors,
logging, comments, types, formatting, Bazel files, shell, Python, docs and
commit style. Each line is a ratified rule, in russ's words, or marked as an
orchestrator ruling, with the date where there is one and a link to the section
that holds its reasoning. `AGENTS.md` says which indexes to read for which task.

- Precedence: AGENTS.md, these indexes and the rules they link, then Google's
  guides, then Abseil's Tips of the Week as guidance (russ, 2026-10-09).
  [style](/docs/style.md#rule-precedence-abseil-tips),
  [style 1.2](/docs/style.md#rule-reach-for-abseil)
- Google's style rulings, when unambiguous and not overruled by our style
  guide, are law; judgement only where the guides are ambiguous (obvious
  cases you settle, otherwise ask russ) (russ, 2026-10-19).
  [style](/docs/style.md#rule-google-is-law)
- Format with the repository's `.clang-format`; clang-tidy and the clang-query
  matchers run on our code and their findings are errors.
  [style 1.1](/docs/style.md#rule-formatting-enforced),
  [style 1.1](/docs/style.md#rule-structural-style-checks)
- A mechanical reformat commit uses the `style:` prefix (orchestrator ruling,
  2026-10-18). [style 1.1](/docs/style.md#rule-reformat-commit)
- Include what you use, and no copies of UAPI definitions.
  [style 1.1](/docs/style.md#rule-include-what-you-use),
  [style 1.1](/docs/style.md#rule-no-uapi-copies),
  [style 1.1 (russ, 2026-10-18)](/docs/style.md#rule-kernel-headers-match-test-kernel)
  (its one exception: the `USER_NS_INIT_INO` `#ifndef` block, russ 2026-10-18)
- Prefer `consteval`, or `constexpr`, whenever feasible, so bugs are emitted
  at compile time instead of needing a test; overrides Google's "avoid
  complexifying function definitions" only where the design is not distorted
  (russ, 2026-10-19). [style 1.1](/docs/style.md#rule-prefer-consteval)
- Warnings are errors: `-Werror` today, `-Weverything -Werror` from phase 7.
  [style 1.1](/docs/style.md#rule-warnings-are-errors)
- Narrowing Google's C++ rules:
  [optional and string_view](/docs/style.md#rule-std-optional-string-view),
  [function names: CamelCase, except the `Syscalls` interface's members (lower-case man-page names, the thin layer in `RealSyscalls`); `Syscalls` is the only seam, owning types are RAII at creation (russ, 2026-10-19)](/docs/style.md#rule-function-names),
  [macros are discouraged; a new one needs russ's approval
  (russ, 2026-10-19)](/docs/style.md#rule-macros-discouraged),
  [non-const locals (russ, 2026-10-11)](/docs/style.md#rule-locals-non-const),
  [flags only in main (russ, 2026-10-11)](/docs/style.md#rule-flags-in-main-only),
  [no function pointers (russ, 2026-10-11)](/docs/style.md#rule-no-function-pointers),
  [Abseil time types (russ, 2026-10-17)](/docs/style.md#rule-absl-time),
  [container algorithms (russ, 2026-10-10)](/docs/style.md#rule-absl-container-algorithms),
  [composition over inheritance (russ, 2026-10-18)](/docs/style.md#rule-composition-over-inheritance),
  [`[[nodiscard]]` (russ, 2026-10-10)](/docs/style.md#rule-nodiscard),
  [ownership](/docs/style.md#rule-ownership),
  [module layout](/docs/style.md#rule-module-layout),
  [ASCII only](/docs/style.md#rule-ascii-only),
  [no exceptions](/docs/style.md#rule-no-exceptions),
  [`ReadOne`](/docs/style.md#rule-readone-unique-lookups).
- Namespaces are flat (russ, 2026-10-07) and `dcfs` names are not qualified
  inside `dcfs` (russ, 2026-10-10).
  [style 1.3](/docs/style.md#rule-flat-namespaces),
  [style 1.3](/docs/style.md#rule-unqualified-dcfs-names)
- Express invariants in the type system, not in comments (russ, 2026-10-17:
  "It's easy to ignore a comment. The compiler enforces types.").
  [style 1.4](/docs/style.md#rule-types-over-comments),
  [style 1.4](/docs/style.md#rule-enum-class)
- Errors are `absl::Status`/`StatusOr` only, built with `StatusBuilder` and
  propagated with the short macros.
  [style 1.6](/docs/style.md#rule-error-handling-one-page),
  [style 1.6](/docs/style.md#rule-status-only-error-channel),
  [style 1.6](/docs/style.md#rule-statusbuilder),
  [style 1.6](/docs/style.md#rule-propagate-with-macros)
- A Status is never dropped from a message (russ, 2026-10-17: "*ALWAYS* include
  the Status"), and messages follow the accumulation rules.
  [style 1.6](/docs/style.md#rule-status-messages),
  [style 1.6](/docs/style.md#rule-status-never-dropped)
- A function that does several things is guards, then one straight line of
  effects; the signature says whether it can fail; program output is
  `absl::PrintF` (russ, 2026-10-10).
  [style 1.6a](/docs/style.md#rule-function-shape),
  [style 1.6a](/docs/style.md#rule-signature-says-can-fail),
  [style 1.6a](/docs/style.md#rule-program-output)
- Return a failed Status or log, never both; log levels (russ, 2026-10-08).
  [style 1.7](/docs/style.md#rule-status-or-log),
  [style 1.7](/docs/style.md#rule-logging-levels)
- Comments say the non-obvious why, never the obvious what.
  [style 1.9](/docs/style.md#rule-comments-self-explanatory),
  [style 1.9](/docs/style.md#rule-comments-invariants),
  [style 1.9](/docs/style.md#rule-no-echo-docstrings),
  [style 1.9](/docs/style.md#rule-comments-lower-layer),
  [style 1.9](/docs/style.md#rule-comments-name-source)
- Comment length (orchestrator ruling, 2026-10-18).
  [style 1.9](/docs/style.md#rule-comments-length)
- Sync-point terms (russ, 2026-10-18: "fsync _is_ a sync point, just not a
  syncfs point."). [style 1.9](/docs/style.md#rule-sync-point-terms)
- Bazel files follow buildifier's BUILD style; everything goes through Bazel;
  third-party code is fetched and pinned by Bazel.
  [style 3](/docs/style.md#rule-bazel-style),
  [style 3](/docs/style.md#rule-everything-through-bazel),
  [style 3](/docs/style.md#rule-third-party-by-bazel)
- Unmodified third-party code we carry copies of is exempt from our rules; if
  we modify it, all of it matches our rules, with TDD (russ, 2026-10-19:
  "However, if we need to modify the code at all, it must all be changed to
  match our rules (including TDD testing!!!!).").
  [style 3](/docs/style.md#rule-carried-third-party)
- Minimize shell: new tools are C++ (russ, 2026-10-11/17); host scripts are bash
  and guest scripts busybox sh. [style 4](/docs/style.md#rule-cpp-over-shell),
  [style 4](/docs/style.md#rule-host-scripts-bash),
  [style 4](/docs/style.md#rule-guest-scripts-busybox-sh)
- Host tooling has a style and is reviewed (russ, 2026-10-17: "I want *some*
  style. I just don't care what it is."); Python follows Google's guide.
  [style 5](/docs/style.md#rule-host-tooling-has-style),
  [style 5](/docs/style.md#rule-python-style)
- Docs: decisions go in design.md, behavior in README.md, plan files belong to
  the orchestrator. [style 7](/docs/style.md#rule-where-things-go)
- Prose and Markdown follow the style guide.
  [style 7](/docs/style.md#rule-prose-style),
  [style 7](/docs/style.md#rule-markdown-style)
- Commit subjects start with the plan step; the body says why and ends with the
  Co-Authored-By trailer of the model that wrote the commit (russ, 2026-10-18).
  [style 7](/docs/style.md#rule-commit-subject),
  [style 7](/docs/style.md#rule-commit-body)
- No test-only things in production code (russ, 2026-10-09), and no test peers.
  [style 8](/docs/style.md#rule-no-test-only-things),
  [style 8](/docs/style.md#rule-no-test-peers)
