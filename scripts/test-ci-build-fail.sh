#!/usr/bin/env bash
# A run whose build fails ends at once with a non-zero status, with or without the harness checks of shard 1 still running (a hosted run sat until the step timeout
# after `ci: FAIL build`). A copy of scripts/ci.sh whose pgtest.sh fails the build, whose lint and span test run for a minute, and whose initdb is a stub. Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
bad=0
TM=(); for tm in timeout gtimeout; do command -v $tm >/dev/null && { TM=($tm 30); break; }; done   # macOS has neither without coreutils: a hang then ends at the lint job limit
fail() { echo "FAIL: $*"; bad=1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
R=$T/repo
mkdir -p "$R/scripts" "$R/sqlite" "$T/bin"
for f in "$REPO"/* ; do [[ ${f##*/} == scripts || ${f##*/} == sqlite ]] || ln -s "$f" "$R/${f##*/}"; done
for f in "$REPO"/scripts/*; do ln -s "$f" "$R/scripts/${f##*/}"; done
rm "$R/scripts/ci.sh" "$R/scripts/ci" "$R/scripts/pgtest.sh" "$R/scripts/lint.sh"
cp "$REPO/scripts/ci.sh" "$R/scripts/ci.sh"
cp -r "$REPO/scripts/ci" "$R/scripts/ci"
printf '#!/usr/bin/env bash\ncase $1 in initdb-template) mkdir -p "$2" ;; install) echo "the build failed"; exit 1 ;; esac\n' >"$R/scripts/pgtest.sh"
printf '#!/usr/bin/env bash\nsleep 60\n' >"$R/scripts/lint.sh"
printf 'span-test span-test-asan:\n\tsleep 60\n' >"$R/sqlite/Makefile"
printf '#!/bin/sh\nexit 0\n' >"$T/bin/initdb"
chmod +x "$R/scripts/pgtest.sh" "$R/scripts/lint.sh" "$T/bin/initdb"

# build_fails NAME [ENV...]: the status and the seconds of a run that cannot build
build_fails() {
	local name=$1 t0=$SECONDS rc
	shift
	( cd "$R" && env CI=1 PATH="$T/bin:$PATH" SIFT_CI_TMP="$T/run-$name" "$@" "${TM[@]}" scripts/ci.sh quick >"$T/$name.log" 2>&1 )
	rc=$?
	(( rc != 124 )) || fail "$name: the run was still going after 30 s"
	(( rc != 0 )) || fail "$name: the run exited 0 after a failed build"
	(( SECONDS - t0 < 20 )) || fail "$name: the run took $((SECONDS - t0)) s to end"
	grep -q 'the build failed' "$T/$name.log" || fail "$name: the log of the build is not shown"
}
build_fails alone SIFT_CI_NO_HARNESS=1
build_fails harness SIFT_CI_SHARD=1/1
exit "$bad"
