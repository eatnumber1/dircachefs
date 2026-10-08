# tools/sbom: SBOMs of the pins, for OSV-Scanner

Plan steps 5.3 and 5.3c. OSV-Scanner does not read `MODULE.bazel.lock`, so
`sbom.py generate` writes the pins as two CycloneDX 1.5 SBOMs in `osv/` (not
checked in), and the `osv` CI job scans them differently (README.md at the
root, "Dependency vulnerability scanning", says what that covers):

- `shipped.cdx.json`: what the dcfs binaries link. **Gates.**
- `testonly.cdx.json`: everything else we pin. Informational.

## The shipped SBOM

Its members are the external repositories of the Bazel dependency graph of
`//dcfs:main` and `//dcfs:main_static`, which `//dcfs:linked_deps` (a
`genquery`) writes out. `//tools/sbom:sbom_test` reads that list and fails
when

- a linked repository has no entry under `shipped` in `pins.json` (nor one
  under `build_only`, for repositories that contribute no code: Bazel's
  tools, platform constraints, rules_cc);
- an entry under `shipped` or `build_only` is no longer linked;
- a shipped module's resolved version in `MODULE.bazel.lock` differs from
  the `version` recorded in `pins.json`;
- a shipped entry has no 40-digit git commit.

So a new linked dependency (or a version bump of one) cannot slip past the
scan: the fast tier fails until `pins.json` has its tag and commit. To add
or update one:

```
git ls-remote <repo> 'refs/tags/<tag>' 'refs/tags/<tag>^{}'   # the ^{} line, if any, is the commit
```

then put `version` (the resolved BCR module version), `repo`, `tag`,
`commit` and `osv` (`records` if `https://api.osv.dev/v1/query` with
`{"commit": ...}` for some older commit of that repository reports advisories,
else `no-records`) under `shipped`. `python3 tools/sbom/sbom.py verify-commits`
(network; the `osv` job runs it) checks every pinned tag against upstream.

Why commits: OSV holds C/C++ advisories as `GIT` ranges over a repository's
commits, and has no package ecosystem to match a purl against (a
`pkg:github/...@<commit>` entry in an SBOM reports nothing; measured with
osv-scanner v2.6.0). The CLI takes a commit only from a git root, so
`sbom.py git-roots` writes `osv/shipped-git/<name>/.git/{HEAD,config,objects,refs}`
(a detached HEAD naming the commit; no objects) per shipped component, and the
job runs `osv-scanner scan source --include-git-root --no-ignore -r
osv/shipped-git` (`--no-ignore` because `/osv/` is in `.gitignore`).

The self-check fixture `testdata/seeded_vulnerable.cdx.json` is a shipped-format
SBOM with libfuse 3.2.0 (commit `cfdca8c6a0f901f409d0a66dd158bd6c8b470bb6`,
CVE-2018-10906); the job turns it into a git root with the same command and
scans it with the same arguments, and fails if nothing is found.

## The test-only SBOM

What goes in (a pin missing from this list fails `//tools/sbom:sbom_test`):

| Source | Entries | purl |
|---|---|---|
| `bazel_dep` in `MODULE.bazel` that is not shipped | the version named there | `pkg:github/...` or `pkg:generic/...` per `pins.json` |
| `http_archive`, `http_file` in `MODULE.bazel` | version from `strip_prefix` or the URL | per `pins.json` |
| `third_party/debian/debs.lock` | every `.deb`, under its **source package** name | `pkg:deb/debian/<source>@<version>?distro=bookworm` |
| the `llvm_distribution` call in `MODULE.bazel` (`sysroot_debs`, `runtime_debs`; `third_party/llvm`) | every `.deb` of the toolchain's sysroot and runtime libraries, by the same table (`debian_sources.tsv`'s optional fifth column names a release other than bookworm: trixie's `linux-libc-dev`). The sysroot's glibc (`libc6-dev`, listed under `shipped_debs` in `pins.json`) is linked statically into the dcfs binaries, so it is in the shipped SBOM too (and alone in `shipped-debs.cdx.json`, which the `osv` job scans by package: `git-roots` skips it); the rest are test-only | `pkg:deb/debian/<source>@<version>?distro=<release>` |
| `.github/ci/prepare.sh` | Bazelisk | `pkg:github/bazelbuild/bazelisk` |
| each `alpine_package` repository's `resolved.json` | every package the fetch took (the kernel, QEMU, the filesystem tools, busybox, strace), under its **origin** package | `pkg:apk/alpine/<origin>@<version>?distro=alpine-<release>` |

Only the Debian and Alpine entries are matched by OSV; the rest are carried
so the SBOM is complete.

The Alpine entries use the origin package (`o:` in Alpine's index) because
that is what OSV's Alpine advisories name: the binary `linux-virt` is built
from `linux-lts`, and an SBOM entry named `linux-virt` reports "no issues",
silently (osv-scanner v2.6.0, measured in the Phase 24 spike). `sbom_test`
fails when a component is named after a binary subpackage, when an
`alpine_package` in `MODULE.bazel` has no `resolved.json`, and when a
`resolved.json` lacks a package the repository asks for. What a fetch took
is whatever the Alpine branch had at that time; the scan sees the same.
`sbom.py generate --alpine <repository>=<resolved.json>` takes one per
repository (`.github/ci/osv.sh` fetches them: `sbom.py alpine-repos` lists
the repositories).

`pins.json` maps each pin to its purl. `debian_sources.tsv` maps each
Debian binary package to its source package and source version, because
OSV's Debian advisories are per source package (`libc6` is `glibc`,
`mount` is `util-linux`); it comes from the `Packages` index of the pinned
snapshots (see third_party/debian/README.md), and the
test fails when `debs.lock` has a package with no row. When the Debian pin
moves, regenerate it:

```
# the Packages indexes of bookworm and bookworm-updates (archive/debian) and bookworm-security (archive/debian-security), concatenated:
curl -fsSL https://snapshot.debian.org/archive/debian/<timestamp>/dists/bookworm/main/binary-amd64/Packages.xz | xz -dc > Packages
# for each name_version in debs.lock: take Package, Source (name and optional
# "(version)") and Version; the source version is the "(version)" when given,
# else the binary Version without a "+bN" binNMU suffix
```

Run it locally (`.github/ci/osv.sh` does all of this and checks the ignores
and pinned commits; add `--graph bazel-bin/dcfs/linked_deps` after
`bazel build //dcfs:linked_deps` to check the pins against the graph, which
the test does):

```
python3 tools/sbom/sbom.py generate --out-dir osv
python3 tools/sbom/sbom.py git-roots --sbom osv/shipped.cdx.json --out-dir osv/shipped-git
```

## Ignores

`osv-scanner.toml` lists accepted findings; `sbom.py check-ignores` (run by
`.github/ci/osv.sh` and by the test) fails on an entry without a `reason`,
without `ignoreUntil`, or with a date in the past.

## Pins

The scanner is the image of `google/osv-scanner-action` at the commit pinned
in `.github/workflows/ci.yml` (tag v2.6.0, commit
`a345acffa64b0eaede81a3d9aae6141214d9c8fc`; the action runs
`ghcr.io/google/osv-scanner-action:v2.6.0`). To update: look up the commit of
the new tag (`gh api repos/google/osv-scanner-action/git/ref/tags/<tag>`),
change both uses in the workflow and this file, and run
`bazel run //third_party/act -- -j osv`.
