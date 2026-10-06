# Working on dcfs

Rules for anyone changing this repository, human or agent. `README.md`
explains what dcfs is; `docs/design.md` explains how it works; the plan
is `docs/plan/README.md` (one file per phase in `docs/plan/phases/`).
Rules marked "(from phase N)" take effect when that plan phase lands.

## Tests

- **Test first.** Every bug fix and every behavior change starts with a
  test that fails on the current code. Show the failure (quote it in the
  commit message or the review), then fix.
- **Tests that need root or control over the kernel run in QEMU
  guests**, and that is every test of dcfs itself, unit tests included:
  no code paths exist only so such a test can run outside the guest. The
  guests boot a minimal kernel in about a second. Checks that need
  neither (clang-tidy, formatting, model checking a specification) may
  run on the host.
- **Fakes, not mocks.** Inject dependencies and use small fake
  implementations. Test-only code never lives in production files: it
  goes in `*_test.cc`, or in a `testonly/` directory whose BUILD targets
  set `testonly = 1`. Syscall fault injection uses link-time
  `-Wl,--wrap` fakes.
- **Coverage** (from phase 7/8): new code arrives fully covered by tests,
  error paths included. CI fails if total coverage drops. Gaps that
  cannot be covered are listed with a reason in `docs/coverage.md`.
- **Test sizes are tiers**: Bazel `size` is the tier, and every test
  declares `size` and `timeout` explicitly (the QEMU test macros refuse a
  missing one). `small` tests run constantly during development
  (`bazel test --config=fast //...`), `medium` before submitting
  (`--config=presubmit`), `large`/`enormous` in CI (plain `bazel test
  //...`). The soak test is tagged `manual` and only runs when asked for
  by name. See `test/qemu/README.md` "Test tiers".
- Never weaken a test to make it faster or to make it pass.

## Running Bazel

- Run Bazel with access to `/dev/kvm`, e.g. `sg kvm -c 'bazel test
  //...'` if your login session predates joining the `kvm` group. A
  Bazel server started without KVM access makes every guest fall back to
  slow emulation: run `bazel shutdown` first when switching.
- Never pipe Bazel through `tail` or `head` in an `&&` chain: the pipe
  hides Bazel's exit status.
- Shut down Bazel servers you started in temporary worktrees when you are
  done with them.

## Code

- **Root is required.** dcfs runs as root; there are no unprivileged
  fallbacks.
- **Warnings are errors.** Today: `-Werror`. From phase 7: clang's
  `-Weverything -Werror` for our code, with every disabled warning listed
  in `.bazelrc` with a reason.
- **Tri-state records.** Every database record that mirrors something on
  the backing filesystem is present, absent or unknown, and a mutation
  sets it unknown before the backing syscall and present/absent after.
- **The protocol has a model.** A change to the write-through protocol
  (mutation phases, fills and their guards, completeness, the dirty set,
  sync points, recovery) updates the TLA+ model in `formal/` in the same
  change, and `bazel test //formal/...` passes (see `formal/README.md`,
  "Changing the model"). A protocol bug the model can express gets a
  `formal/known_bugs/` variant whose test expects its counterexample.
  Fixing a gap listed in `formal/findings/` moves its configuration into
  the real model. From phase 12.2, trace validation fails if the code does
  something the model does not allow.
- **File names are bytes.** Never treat a name, symlink target or xattr
  name as text; escape it whenever it is printed (from phase 9).
- **Third-party code is fetched and built by Bazel** (from phase 4), not
  checked in unless there is no other way. Prefer the Bazel Central
  Registry (in `MODULE.bazel`); otherwise `http_archive`/`http_file` with
  a sha256 and Bazel rules that build it. `third_party/<name>/` holds only
  what we write for it (BUILD overlays, rules, patches, config, lock
  files) and a README saying how to update the pin. Every tool the build
  or the tests use is pinned this way.
- Format C++ with the repository's `.clang-format`. From phase 7,
  clang-tidy (`.clang-tidy`) runs on our code and its findings are errors.

## Docs and commits

- Design decisions and their reasons go in `docs/design.md`; user-facing
  behavior and limitations in `README.md`.
- When a plan phase changes, update its file in `docs/plan/phases/` and
  the status table in `docs/plan/README.md`; append to
  `docs/plan/log.md`.
- Commit subjects start with the plan step they belong to, e.g.
  `3.2: third_party/linux: build the pinned kernel with Bazel`.
- Never push, publish or send anything outside the repository unless the
  maintainer asks.
