# Alpine packages (Phase 24)

The test kernel (`linux-virt`) and the host-side test tools (QEMU, the mkfs
tools, busybox) are Alpine's own packages, not source builds. This directory
holds the repository rules that fetch them and the keys that check them.

## The pin is the branch

`MODULE.bazel` names one Alpine release branch (`branch = "v3.24"` on
`alpine_index`). A stable branch carries one kernel series and one
major.minor of each tool for its life (v3.24: kernel 6.18, QEMU 11.0, e2fsprogs
1.47, xfsprogs 7.0, btrfs-progs 6.17, busybox 1.37), so the branch is the
series. Alpine publishing a new build inside the branch (a kernel 6.18.56, a
tool's patch release) is taken as it comes and must never fail a test or a
job: no package version appears in our tree, and nothing compares versions.

What each fetched repository took is in its `resolved.json` (name, origin,
version, branch, repository); the SBOM and the logs use it. Bazel's output
base has them, e.g. `bazel info output_base`/external/+alpine_package+alpine_linux_virt/resolved.json.

## What the rules check

- `alpine_index` downloads each repository's `APKINDEX.tar.gz` and verifies
  its RSA signature against the keys in `keys/`. All repositories and all
  packages resolve against this one snapshot (two fetches seconds apart
  could otherwise disagree about a dependency).
- `alpine_package` takes the branch's current version of each named
  package (and with `closure = True` of every dependency, found through the
  index's `D:` and `p:` fields), downloads each apk and checks three things
  before unpacking: the apk's RSA signature over its control segment, the
  index's `C:` checksum of that segment, and the control segment's
  `datahash` of the data segment. Anything unexpected fails the fetch with
  the reason.
- Everything runs in `apk.py` under the hermetic Python from `rules_python`:
  no host `apk`, `openssl` or `tar`. RSA verification is PKCS#1 v1.5 with
  Python's `pow` (standard library only; it handles public data, so no
  constant-time concern).

`//third_party/alpine:signature_test` checks that a tampered index, a
tampered apk (control or data segment), a signature by an unknown key and a
package missing from the branch are all refused, and that a real Alpine
apk (`testdata/`, `busybox-binsh`, 1.5 KB) verifies against the checked-in keys.

## Fetching, and getting a newer build

Bazel re-runs a repository rule only when its inputs change, and the rules
read the network, so a developer keeps what they fetched until they ask for
a newer build:

    bazel fetch --force --repo=@alpine_index

This refetches the index; every package repository depends on the index file,
so those whose index changed are refetched on the next build. To refetch one
repository regardless: `bazel fetch --force --repo=@alpine_linux_virt`.

CI starts with no fetched repositories (the external directory is not in its
cache) and so tests the branch's current packages. A developer and CI may
therefore differ within the series. The weekly scheduled run keeps running
the whole suite, so an Alpine update that breaks something surfaces there.

Nothing here is recorded in `MODULE.bazel.lock` (`--lockfile_mode=error`):
the rules are `use_repo_rule` repositories, which the lock file never holds.
Alpine removes superseded packages from its mirror within days, so there is
no way to fetch an old build; that is why the pin is the branch and not a
version.

## Changing the branch

Edit `branch` on `alpine_index` in `MODULE.bazel` (a release is listed at
<https://alpinelinux.org/releases/>; Alpine supports a branch for two
years, so a branch change is due about every year or two). Then
`bazel fetch --force --repo=@alpine_index` and run the suite.

## The keys

`keys/` holds the three Alpine release signing keys that alpine-keys 2.6-r0
ships for x86_64 (the apks of v3.24 are signed by `6165ee59`), taken from
the alpine-keys package and checked against
<https://gitlab.com/alpine/aports/-/tree/master/main/alpine-keys/>:

| file | sha256 |
|---|---|
| `alpine-devel@lists.alpinelinux.org-4a6a0840.rsa.pub` | `9c102bcc376af1498d549b77bdbfa815ae86faa1d2d82f040e616b18ef2df2d4` |
| `alpine-devel@lists.alpinelinux.org-5261cecb.rsa.pub` | `12f899e55a7691225603d6fb3324940fc51cd7f133e7ead788663c2b7eecb00c` |
| `alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub` | `207e4696d3c05f7cb05966aee557307151f1f00217af4143c1bcaf33b8df733f` |

A new branch may be signed by a key not listed here; the fetch then fails with
"signed by key X, which is not one of the checked-in keys". Add the key (from
the new alpine-keys package or aports, checking it against both) and record
its hash above.
