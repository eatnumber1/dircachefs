#!/bin/bash
# Step 26.28: commits the badges' endpoint JSON to the `badges` branch of this
# repository, where shields.io reads it (README.md, "Status"). The `badges` job
# of ci.yml and of mutation.yml runs it on main, with the workflow's token
# (`contents: write` on that job alone); nothing else in the project pushes.
#
#   badges.sh <run id> <directory of <name>.json files>
#
# Run from the checkout of the default branch, with credentials persisted
# (actions/checkout's default). The branch is created as an orphan branch the
# first time. Nothing is committed when no file changed.
set -euo pipefail

run_id=$1
in=$2

# No producer ran (the badges job still runs, `if: always()`): nothing to do,
# and the branch is not touched.
shopt -s nullglob
files=("$in"/*.json)
if [ ${#files[@]} -eq 0 ]; then
	echo "no badge files, nothing to do"
	exit 0
fi

rc=0
git ls-remote --exit-code --heads origin badges >/dev/null || rc=$?
case $rc in
0)
	git fetch --depth 1 origin badges
	git worktree add -B badges badges-branch FETCH_HEAD
	;;
2)
	echo "badges: no badges branch yet; creating it as an orphan branch"
	git worktree add --orphan -b badges badges-branch
	;;
*)
	echo "badges.sh: git ls-remote failed ($rc)" >&2
	exit "$rc"
	;;
esac

cp "${files[@]}" badges-branch/
cd badges-branch
if [ -z "$(git status --porcelain)" ]; then
	echo "badges: no change"
	exit 0
fi
git add -A
git -c user.name="github-actions[bot]" \
	-c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
	commit -q -m "badges: $run_id"
git push origin HEAD:refs/heads/badges
