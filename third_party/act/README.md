# act (pinned) and the CI runner image

Why: `.github/workflows/ci.yml` is developed and checked locally with
[nektos/act](https://github.com/nektos/act) before anything is pushed to
GitHub, and `act` is also the detector for host dependencies: its job
container has only what a fresh GitHub runner has, so a build or test that
quietly depends on a tool or library of this machine (as QEMU's host zlib
and the host mke2fs did in Phase 4) fails there. See "CI" in `/README.md`.

## Pins

Both are declared here and in `MODULE.bazel`; neither is a host install.

- **act** v0.2.89 (latest release as of 2026-10-06; Bazel repository
  `@act`, an `http_archive` of the Linux x86_64 release tarball).
  - URL: `https://github.com/nektos/act/releases/download/v0.2.89/act_Linux_x86_64.tar.gz`
  - sha256: `0191d6f1f3b716b5c55820032605d05fc3c1cdbf581ebeff655019e5dd1524c0`
    (the digest GitHub shows for the asset, the release's `checksums.txt`,
    and a local download hashed twice all agree).
- **Runner image** `ghcr.io/catthehacker/ubuntu@sha256:62d572b92f9f32d3427b6d220ad1f9dca9c7b6ffad37d295425037dbff78abaf`
  (`runner_image.txt`). This is the digest the tag `act-latest` pointed at on
  2026-10-06 (the same as `act-24.04`: Ubuntu 24.04 with the tools of
  GitHub's `ubuntu-24.04` image that actions commonly need). It is a
  multi-architecture index; `act.sh.tpl` selects `linux/amd64`. act maps
  `runs-on: ubuntu-latest` and `ubuntu-24.04` to it.

## Running a job

```
bazel run //third_party/act -- -j fast          # or presubmit, full
bazel run //third_party/act -- --list
```

or, from a shell that is not yet in the `docker` group (and builds first,
so the Bazel server is not started without the `kvm` group):

```
sg kvm -c 'bazel build //third_party/act'
sg docker -c 'bazel-bin/third_party/act/act -j fast'
```

Everything after `--` goes to act. The wrapper (`act.sh.tpl`, the only place
these settings live) adds:

- `--platform ubuntu-latest=<image> --platform ubuntu-24.04=<image>` with the
  pinned digest; it runs `docker pull` once if the image is absent
  (`--pull=false` afterwards: the digest cannot change).
- `--container-options "--label dcfs.owner=act --device /dev/kvm
  --group-add <kvm gid>"` (the gid from `getent group kvm`; the device is
  left out, with a message, when the host has no `/dev/kvm`) and `--rm`.
  Every job container therefore carries `dcfs.owner=act`; clean up only by
  that label (`docker ps -a --filter label=dcfs.owner=act`).
- `--action-cache-path`, `--cache-server-path` and `--artifact-server-path`
  under `.act/` in the checkout (gitignored; `DCFS_ACT_STATE` overrides), so
  act's downloaded actions, its `actions/cache` store and its uploaded
  artifacts stay in the repository checkout and not in `~/.cache`. Deleting
  `.act/` is a cold start: no Bazel caches, no actions.
- `.github/workflows/ci.yml` as the workflow.

The job container runs as root with `network=host` (act's default), copies
the checkout in (files ignored by `.gitignore`, such as `bazel-*` and
`user.bazelrc`, stay out) and does not see this machine's `~/.cache`: the
Bazel disk cache is restored from, and saved to, act's cache server, as on
GitHub.

Docker on russ's machine is a production system: act creates only its own
containers, volumes (`act-toolcache`, left in place between runs: it holds the
Node toolchain act gives actions) and networks; nothing here prunes, stops or
reconfigures anything else, and pulled runner images stay in place.

## Differences from a GitHub-hosted runner

- The container is not a VM: no systemd or udev (the KVM udev rule in
  `.github/ci/prepare.sh` is a no-op; `--device`/`--group-add` give access
  instead), Bazel's `linux-sandbox` is unavailable (it falls back to
  `processwrapper-sandbox`) and the user is root, not `runner`.
- `/dev/kvm` is the host's, so KVM works in the container exactly as on the
  host (`run-qemu.sh` logs the accelerator it chose).
- The image has fewer pre-installed packages than GitHub's, which is the
  point of the detector.

## Updating

- **act**: take the new release from
  `https://github.com/nektos/act/releases`, hash
  `act_Linux_x86_64.tar.gz` (compare with `checksums.txt` and with GitHub's
  digest), update `MODULE.bazel` and this file, rebuild
  (`bazel build //third_party/act`) and rerun a job.
- **Runner image**: look up the digest of the tag
  (`docker buildx imagetools inspect ghcr.io/catthehacker/ubuntu:act-latest`
  or the registry's `Docker-Content-Digest` header), replace it in
  `runner_image.txt` and above, and rerun the `full` job: a new image can
  expose or hide host dependencies.
