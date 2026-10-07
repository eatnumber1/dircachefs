#!/bin/bash
# Prepares a CI runner (a GitHub-hosted ubuntu-24.04 or an `act` job
# container, see third_party/act/README.md) for `bazel test`:
#
#   1. makes /dev/kvm usable if the runner has one (otherwise the tests fall
#      back to TCG, see test/qemu/README.md) and records the result;
#   2. installs the pinned Bazelisk (the repository pins Bazel itself in
#      .bazelversion);
#   3. writes the CI-only settings to user.bazelrc (which .bazelrc imports
#      and .gitignore ignores).
#
# Everything else the build needs is fetched and built by Bazel. What the
# build and the tests still take from the host is listed in README.md under
# "Host requirements" and installed by the workflow before this runs.
set -euo pipefail

SUDO=""
if [ "$(id -u)" != 0 ]; then
	SUDO="sudo"
fi

# --- 0. Host tools ----------------------------------------------------------
# What the build and the tests still take from the host (README.md, "Host
# requirements"). GitHub's hosted image has most of it already; `act`'s
# runner image lacked flex, bison, cpio and ninja, which is how they were
# found (Phase 5.2). Packages named here and not in a fresh runner are
# non-hermetic dependencies: make one hermetic, then drop it from this list.
#   build-essential  libc6-dev and linux-libc-dev: the glibc and Linux UAPI
#                    headers and glibc's static libraries every C/C++ compile
#                    and link uses (the compiler is the pinned clang; gcc is
#                    not used, but comes with the package)
#   libxml2          libxml2.so.2: the pinned LLVM's ld.lld links it (and ICU,
#                    liblzma) dynamically; --no-install-recommends skips it
#   cpio             test/qemu/scripts/mkinitramfs.sh packs every initramfs
#   python3          .github/ci/osv.sh (the SBOM generator; Bazel's own
#                    Python is hermetic)
#   coreutils (truncate), curl, xz-utils, git: tests' scratch disks, Bazelisk,
#                    Bazel's archive extraction, repository rules
HOST_PACKAGES="build-essential libxml2 cpio python3 coreutils curl xz-utils git"
export DEBIAN_FRONTEND=noninteractive
if ! dpkg -s $HOST_PACKAGES >/dev/null 2>&1; then
	$SUDO apt-get update -qq
	$SUDO apt-get install -y -qq --no-install-recommends $HOST_PACKAGES
fi

# --- 1. KVM -----------------------------------------------------------------
if [ -e /dev/kvm ]; then
	if [ ! -w /dev/kvm ]; then
		# GitHub's documented recipe for hosted runners; it does nothing
		# useful inside a container (no udev there), hence the chmod below.
		echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
			$SUDO tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null || true
		$SUDO udevadm control --reload-rules 2>/dev/null || true
		$SUDO udevadm trigger --name-match=kvm 2>/dev/null || true
	fi
	[ -w /dev/kvm ] || $SUDO chmod a+rw /dev/kvm || true
fi
if [ -w /dev/kvm ]; then
	echo "prepare.sh: /dev/kvm is usable; the tests run under KVM"
	echo "DCFS_CI_KVM=1" >>"$GITHUB_ENV"
else
	echo "prepare.sh: no usable /dev/kvm; the tests run under TCG (slow)"
	echo "DCFS_CI_KVM=0" >>"$GITHUB_ENV"
fi
ls -l /dev/kvm 2>&1 || true

# --- 2. Bazelisk ------------------------------------------------------------
# Pinned release, checked against the sha256 recorded here (the release's
# own .sha256 file agrees). To update, change both values together (README.md,
# "CI").
BAZELISK_VERSION=v1.29.0
BAZELISK_SHA256=5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992
mkdir -p "$HOME/.local/bin"
curl -fsSL -o "$HOME/.local/bin/bazel" \
	"https://github.com/bazelbuild/bazelisk/releases/download/$BAZELISK_VERSION/bazelisk-linux-amd64"
echo "$BAZELISK_SHA256  $HOME/.local/bin/bazel" | sha256sum -c -
chmod +x "$HOME/.local/bin/bazel"
echo "$HOME/.local/bin" >>"$GITHUB_PATH"

# --- 3. CI-only Bazel settings ----------------------------------------------
{
	echo "# Written by .github/ci/prepare.sh."
	echo "build --disk_cache=$HOME/.cache/bazel-disk-cache"
	echo "build --repository_cache=$HOME/.cache/bazel-repo-cache"
	# GitHub's repository-wide cache quota is 10 GB for everything; keep the
	# disk cache well inside it.
	echo "build --experimental_disk_cache_gc_max_size=4G"
	# Logs, not a terminal: one progress line every 30 s, no colors.
	echo "common --color=no --curses=no --show_progress_rate_limit=30"
	if [ -w /dev/kvm ]; then
		:
	else
		# Bazel's timeouts apply on top of run-qemu.sh's own TCG limits (unit
		# 300 s, e2e 7200 s). Phase 5.1 measured pjdfstest on ext4 at 3502 s
		# under TCG, 98% of the default `eternal` 3600 s; this gives short,
		# moderate, long and eternal tests room (seconds).
		echo "test --test_timeout=300,1800,3600,7200"
	fi
	# Extra settings for a constrained runner (act on a shared machine):
	# ";"-separated lines, e.g. "build --jobs=3;startup --host_jvm_args=-Xmx1g".
	if [ -n "${DCFS_CI_BAZELRC:-}" ]; then
		echo "$DCFS_CI_BAZELRC" | tr ';' '\n'
	fi
} >user.bazelrc
cat user.bazelrc
