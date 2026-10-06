#!/bin/bash
# nektos/act with this repository's settings (see README.md):
#
#   bazel run //third_party/act -- -j fast
#   bazel run //third_party/act -- -j presubmit
#   bazel run //third_party/act -- -j full
#
# Arguments after `--` go to act. Run it from the repository root, in a
# shell that is in the `docker` group (`sg docker -c '...'` if the login
# session predates the group). Docker on this machine may be a production
# system: act only creates containers and volumes of its own and removes them
# afterwards; nothing here prunes or modifies anything else.
set -euo pipefail

# --- begin runfiles.bash initialization v3 ---
set +e
f=bazel_tools/tools/bash/runfiles/runfiles.bash
source "${RUNFILES_DIR:-/dev/null}/$f" 2>/dev/null ||
	source "$(grep -sm1 "^$f " "${RUNFILES_MANIFEST_FILE:-/dev/null}" | cut -f2- -d' ')" 2>/dev/null ||
	source "$0.runfiles/$f" 2>/dev/null ||
	source "$(grep -sm1 "^$f " "$0.runfiles_manifest" | cut -f2- -d' ')" 2>/dev/null ||
	source "$(grep -sm1 "^$f " "$0.exe.runfiles_manifest" | cut -f2- -d' ')" 2>/dev/null ||
	{
		echo >&2 "cannot find $f"
		exit 1
	}
set -e
# --- end runfiles.bash initialization v3 ---

ACT="$(rlocation @ACT_RLOCATION@)"
IMAGE="$(cat "$(rlocation _main/third_party/act/runner_image.txt)")"

# `bazel run` starts us in the runfiles tree; the repository is where the
# user ran bazel. Run through bazel-bin directly and it is the current
# directory.
REPO="${BUILD_WORKSPACE_DIRECTORY:-$PWD}"
STATE="${DCFS_ACT_STATE:-$REPO/.act}"
mkdir -p "$STATE/actions" "$STATE/cache" "$STATE/artifacts"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker pull "$IMAGE"
fi

KVM_GID="$(getent group kvm | cut -d: -f3)"
# The memory cap keeps a job's out-of-memory kill inside its own container
# (a global OOM kill on a shared machine picks the biggest process of anyone).
OPTS="--label dcfs.owner=act --memory ${DCFS_ACT_MEMORY:-6g}"
if [ -c /dev/kvm ] && [ -z "${DCFS_ACT_NO_KVM:-}" ]; then
	OPTS="$OPTS --device /dev/kvm"
	[ -n "$KVM_GID" ] && OPTS="$OPTS --group-add $KVM_GID"
else
	echo "act.sh: no /dev/kvm for the job container (none on this host, or DCFS_ACT_NO_KVM set); it will use TCG" >&2
fi

ENV_ARGS=()
if [ -n "${DCFS_CI_BAZELRC:-}" ]; then
	ENV_ARGS+=(--env DCFS_CI_BAZELRC)
fi

cd "$REPO"
exec "$ACT" \
	--platform "ubuntu-latest=$IMAGE" \
	--platform "ubuntu-24.04=$IMAGE" \
	--container-architecture linux/amd64 \
	--container-options "$OPTS" \
	--pull=false \
	--rm \
	--action-cache-path "$STATE/actions" \
	--cache-server-path "$STATE/cache" \
	--artifact-server-path "$STATE/artifacts" \
	--workflows .github/workflows/ci.yml \
	"${ENV_ARGS[@]}" \
	"$@"
