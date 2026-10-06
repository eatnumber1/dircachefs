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
- `overrides/tlc2/overrides/TLCOverrides.java` and the `tlc_overrides_jar`
  rule (in `tlc.bzl`; target `:tlc_overrides`): the registry of Java
  operator overrides TLC loads, compiled with the pinned JDK. Trace
  validation (`formal/Trace.tla`, `formal/trace.bzl`) puts it first on the
  class path, ahead of the CommunityModules jar. Why: TLC loads the class
  `tlc2.overrides.TLCOverrides` and every override it lists; the
  CommunityModules jar's own registry lists all of its modules', and its
  `FiniteSetsExt` override refers to `tlc2.value.impl.KSubsetValue`, which
  exists only in TLC's 1.8.0 line, so TLC 1.7.4 stops with a
  `NoClassDefFoundError` as soon as it loads it (every CommunityModules
  release tried back to 2021 does this: the modules track TLC's main
  branch). The registry here lists only what trace validation uses:
  `IOUtils` (environment variables), `Json` (`ndJsonDeserialize`) and
  `SequencesExt`.

## Pins

All are declared in `MODULE.bazel`.

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

- **CommunityModules** (with dependencies) release 202610040242, the latest
  as of 2026-10-06. It runs on TLC 1.7.4 through the trimmed override
  registry above (checked by running trace validation; so does the
  release of 2024-09-18, 202409181925). With the jar's own registry, every
  release tried, back to 202110210339, stops TLC 1.7.4.
  - URL: `https://github.com/tlaplus/CommunityModules/releases/download/202610040242/CommunityModules-deps-202610040242.jar`
    (the dated asset; the undated `CommunityModules-deps.jar` name is
    reused by every release).
  - sha256: `dddb19c3d7596913d92c43073b60d1408ac8f6d86716af4d19dd6861a95bfb7d`
    (downloaded twice and hashed).
  - Repository: `@tla_community_modules` (`http_file`); the jar is
    `@tla_community_modules//file`.

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

For the CommunityModules jar: pick a release from
`https://github.com/tlaplus/CommunityModules/releases`, hash its dated
`CommunityModules-deps-<release>.jar` twice, update the `tla_community_modules`
`http_file` and the version above, and run
`bazel test //dcfs:trace_fault_injection_test //dcfs:dir_cache_fs_trace_test`
(trace validation reads every trace through it). If TLC reports a
`NoClassDefFoundError` or a missing operator, the release's overrides need
a newer TLC than the one pinned; keep `overrides/.../TLCOverrides.java`
listing only the modules `formal/Trace.tla` uses.

For a newer JDK, change `remotejdk21_linux` (in `MODULE.bazel`'s
`use_repo` and `tlc.bzl`'s `_jdk`) to another remote JDK that rules_java
provides, and rerun step 5.
