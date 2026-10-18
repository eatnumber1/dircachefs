# Index of style rules

Read this before you write or review any code or document: naming, errors,
logging, comments, types, formatting, Bazel files, shell, Python and docs style.
Each line is a ratified rule, in russ's words with the date where there is one,
and links to the section that holds its reasoning. `AGENTS.md` says which
indexes to read for which task.

- **Bazel files** follow Bazel's BUILD style guide as buildifier enforces it.
  [style 3](../style.md#rule-bazel-style)
- **Third-party code is fetched and built by Bazel** (from phase 4), not checked
  in unless there is no other way. Prefer the Bazel Central Registry (in
  `MODULE.bazel`); otherwise `http_archive`/`http_file` with a sha256 and Bazel
  rules that build it. `third_party/<name>/` holds only what we write for it
  (BUILD overlays, rules, patches, config, lock files) and a README saying how
  to update the pin. Every tool the build or the tests use is pinned this way.
  The exception is Alpine's packages (from phase 24: the test kernel, QEMU, the
  mkfs tools, busybox): they are pinned to a stable release branch, not to a
  hash, and verified by Alpine's package signatures
  (`third_party/alpine/README.md`).
  [style 3](../style.md#rule-third-party-by-bazel)
- **Warnings are errors.** Today: `-Werror`. From phase 7: clang's `-Weverything
  -Werror` for our code, with every disabled warning listed in `.bazelrc` with a
  reason. [style 1.1](../style.md#rule-warnings-are-errors)
- **Mechanize every rule you can** (russ, 2026-10-09: "Mechanical invariant
  enforcement is more valuable."; 2026-10-17: "Remember to mechanize all of the
  style rules you can."). [style 1.1](../style.md#rule-mechanize-rules); move
  logic to compile time (russ, 2026-10-17):
  [style 1.1](../style.md#rule-compile-time-logic)
- **Express invariants in the type system, not in comments** (russ, 2026-10-17:
  "It's easy to ignore a comment. The compiler enforces types.").
  [style 1.4](../style.md#rule-types-over-comments),
  [style 1.4](../style.md#rule-enum-class)
- **Errors are `absl::Status`/`StatusOr`** and nothing else; built with
  `StatusBuilder` and `ErrnoToStatus`, propagated with Abseil's macros; a
  function that does several things is guards, then one straight line of
  effects. [style 1.6](../style.md#rule-error-handling-one-page),
  [style 1.6](../style.md#rule-status-only-error-channel),
  [style 1.6](../style.md#rule-statusbuilder),
  [style 1.6](../style.md#rule-propagate-with-macros),
  [style 1.6](../style.md#rule-status-messages),
  [style 1.6](../style.md#rule-status-never-dropped) (russ, 2026-10-17:
  "*ALWAYS* include the Status"), [style 1.6a](../style.md#rule-function-shape),
  [style 1.6a](../style.md#rule-signature-says-can-fail),
  [style 1.6a](../style.md#rule-program-output) (russ, 2026-10-10:
  `absl::PrintF`, never iostreams or C printf)
- **Return a failed Status or log, never both**; log levels (russ, 2026-10-08).
  [style 1.7](../style.md#rule-status-or-log),
  [style 1.7](../style.md#rule-logging-levels)
- **Comments** are facts at the line that needs them:
  [style 1.9](../style.md#rule-comments-self-explanatory),
  [style 1.9](../style.md#rule-comments-invariants),
  [style 1.9](../style.md#rule-no-echo-docstrings),
  [style 1.9](../style.md#rule-comments-lower-layer),
  [style 1.9](../style.md#rule-comments-name-source),
  [style 1.9](../style.md#rule-comments-length).
- **Rely on library defaults, and test the ones we rely on** (russ, 2026-10-18:
  "Rely on defaults, but test that our relied-upon defaults are true. That way
  prod code is minimized while still having protections.").
  [style 1.13](../style.md#rule-rely-on-defaults)
- **Follow Abseil's Tips of the Week as design guidance** (russ, 2026-10-09:
  "It's a 'rule', but not as strong of a rule as the Google style guide"): below
  `AGENTS.md`, `docs/style.md` and the Google guides.
  [style](../style.md#rule-precedence-abseil-tips), and reach for Abseil before
  writing a helper: [style 1.2](../style.md#rule-reach-for-abseil)
- **Format C++ with the repository's `.clang-format`.** From phase 7, clang-tidy
  (`.clang-tidy`) runs on our code and its findings are errors.
  [style 1.1](../style.md#rule-formatting-enforced),
  [style 1.1](../style.md#rule-structural-style-checks), a mechanical reformat
  commit uses the `style:` prefix:
  [style 1.1](../style.md#rule-reformat-commit); include what you use:
  [style 1.1](../style.md#rule-include-what-you-use); no copies of UAPI
  definitions: [style 1.1](../style.md#rule-no-uapi-copies)
- **Narrowing Google's C++ rules**:
  [optional and string_view](../style.md#rule-std-optional-string-view),
  [function names](../style.md#rule-function-names),
  [locals non-const (russ, 2026-10-11)](../style.md#rule-locals-non-const),
  [flags only in main (russ, 2026-10-11)](../style.md#rule-flags-in-main-only),
  [no function pointers for callables (russ, 2026-10-11)](../style.md#rule-no-function-pointers),
  [Abseil time types (russ, 2026-10-17)](../style.md#rule-absl-time),
  [container algorithms (russ, 2026-10-10)](../style.md#rule-absl-container-algorithms),
  [composition over inheritance (russ, 2026-10-18)](../style.md#rule-composition-over-inheritance),
  [`[[nodiscard]]` (russ, 2026-10-10)](../style.md#rule-nodiscard),
  [ownership](../style.md#rule-ownership),
  [module layout](../style.md#rule-module-layout),
  [ASCII only](../style.md#rule-ascii-only),
  [no exceptions](../style.md#rule-no-exceptions),
  [`ReadOne`](../style.md#rule-readone-unique-lookups); namespaces:
  [flat](../style.md#rule-flat-namespaces) (russ, 2026-10-07),
  [unqualified `dcfs` names](../style.md#rule-unqualified-dcfs-names) (russ,
  2026-10-10).
- **Shell, Python, TLA+ and host tools**: minimize shell, new tools are C++
  (russ, 2026-10-11/17: "we can do it in C++ with only a few helper functions
  just fine"): [style 4](../style.md#rule-cpp-over-shell),
  [host scripts are bash](../style.md#rule-host-scripts-bash),
  [guest scripts are busybox sh](../style.md#rule-guest-scripts-busybox-sh);
  host tooling has a style and is reviewed (russ, 2026-10-17: "I want *some*
  style. I just don't care what it is."):
  [style 5](../style.md#rule-host-tooling-has-style),
  [Python](../style.md#rule-python-style).
- **Follow `docs/style.md`**: naming, includes, how errors are built
  (`StatusBuilder`, `ErrnoToStatus`) and worded, logging, comments, tests,
  Bazel, shell, Python, docs and commit messages.
- Prose and Markdown follow the style guide: full sentences, plain and specific.
  [style 7](../style.md#rule-prose-style),
  [style 7](../style.md#rule-markdown-style)
- **Sync-point terms** (russ, 2026-10-18: "fsync _is_ a sync point, just not a
  syncfs point."): a file's sync point runs no syncfs; "the periodic sync
  point"; an interruption point is named by where it is ("an fsync's
  syscall"). [style 1.9](../style.md#rule-sync-point-terms)
