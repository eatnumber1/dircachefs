# TLA+ tools (pinned)

Why: `//formal` checks the TLA+ model of dcfs's write-through protocol
with the TLC model checker (`formal/README.md`). TLC is a Java program
shipped as one jar; it runs on the host (it needs neither root nor kernel
control) on a hermetic JDK, never the host's.

What is here:

- `tlc.bzl`: the `tlc_test` rule. It copies the specification's modules and
  configuration into the test's scratch directory, runs TLC, and passes if
  TLC finds no error, or, with `expect_violation`, if TLC reports exactly
  that violation (exit status 12 or 13 and the given text). Any other
  outcome fails, a parse error (exit status 150 and up) included.
- `tlc_test_runner.sh.tpl`: the test script `tlc_test` fills in.

## Pins

Both are declared in `MODULE.bazel`.

- **tla2tools.jar** v1.7.4, the latest stable release as of 2026-10-05
  (v1.8.0 is a rolling pre-release whose asset is replaced in place, so
  it cannot be pinned by hash).
  - URL: `https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar`
  - sha256: `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`
    (downloaded twice and hashed; GitHub publishes no digest for it).
  - Repository: `@tla2tools` (`http_file`); the jar is `@tla2tools//file`.
- **JDK**: rules_java's remote JDK 21 (Azul Zulu, pinned with a sha256 by
  rules_java itself), `@remotejdk21_linux//:jdk`, through `bazel_dep(name =
  "rules_java")` and its `toolchains` extension. `tlc_test` names it
  directly rather than going through Java toolchain resolution, whose
  default runtime (`--java_runtime_version=local_jdk`) is the host's JDK.
  Linux x86_64 only, like the rest of the build.

The TLA+ CommunityModules jar (for reading JSON traces) is not pinned yet:
nothing uses it until trace validation (plan step 12.2).

## Update procedure

1. Check `https://github.com/tlaplus/tlaplus/releases` for a newer stable
   (non-pre-release) version.
2. Hash its `tla2tools.jar`:
   `curl -sL <url> | sha256sum` (twice, to be sure the asset is stable).
3. Update the `urls` and `sha256` of the `tla2tools` `http_file` in
   `MODULE.bazel`, and the version above.
4. TLC's exit statuses are part of `tlc_test_runner.sh.tpl`'s contract:
   check that a violation still exits with 12 (safety) or 13 (liveness),
   for example by running one known-bug test, and that a parse error exits
   with something else.
5. `bazel test //formal/...`: every test must pass, the known-bug and
   finding tests included (they fail if TLC stops reporting their
   counterexample).

For a newer JDK, change `remotejdk21_linux` (in `MODULE.bazel`'s
`use_repo` and `tlc.bzl`'s `_jdk`) to another remote JDK that rules_java
provides, and rerun step 5.
