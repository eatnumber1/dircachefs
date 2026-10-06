# tools/sbom: the SBOM of every pin, for OSV-Scanner

Plan step 5.3. OSV-Scanner does not read `MODULE.bazel.lock`, so
`sbom.py generate` writes the pins as a CycloneDX 1.5 SBOM
(`osv/dcfs.cdx.json`, not checked in) that the `osv` CI job scans.

What goes in (a pin missing from this list fails `//tools/sbom:sbom_test`):

| Source | Entries | purl |
|---|---|---|
| `bazel_dep` in `MODULE.bazel` | the version named there (Bazel's resolution may pick a higher one for a module another module also needs; the direct pins are what we choose) | `pkg:github/...` or `pkg:generic/...` per `pins.json` |
| `http_archive`, `http_file`, `qemu_repo` (and QEMU's dtc) in `MODULE.bazel` | version from `strip_prefix` or the URL | per `pins.json` |
| `third_party/debian/debs.lock` | every `.deb`, under its **source package** name | `pkg:deb/debian/<source>@<version>?distro=bookworm` |
| `.github/ci/prepare.sh` | Bazelisk | `pkg:github/bazelbuild/bazelisk` |

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

Run it locally: `python3 tools/sbom/sbom.py generate --out osv/dcfs.cdx.json`
(`.github/ci/osv.sh` does this and checks the ignores).

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
