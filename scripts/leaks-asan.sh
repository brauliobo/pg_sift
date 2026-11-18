#!/usr/bin/env bash
# The leak set (scripts/valgrind-leaks.tests) on the ASan and UBSan build: the same tests the memcheck leak gate runs (scripts/valgrind.sh --leaks), in a private copy with its own
# cluster, then the sanitizer report (scripts/pgtest.sh sanitizer-report: AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer findings of every process of the run fail it).
# LeakSanitizer sees malloc, strdup and handle leaks, not palloc (memcheck's job); ASan and UBSan see overruns, use after free and undefined arithmetic that memcheck does not on a build with
# the server's own checks. In scripts/ci.sh: the job asan-leaks, queued by SIFT_CI_LEAKS=1 or SIFT_CI_JOBS=asan-leaks.
#
#   scripts/leaks-asan.sh [TEST...]        (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh)
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
source "$REPO/scripts/source-snapshot.sh"
TMP=${SIFT_TMP:-/tmp/sift-leaks-asan-$$}
WORK=$TMP/leaks-asan
TESTS=("$@")
(( ${#TESTS[@]} )) || mapfile -t TESTS < <(awk -F'\t' '!/^#/ && NF { print $1 }' "$REPO/scripts/valgrind-leaks.tests")
export SANITIZE=1

rm -rf "$WORK" && mkdir -p "$WORK/tree"
copy_git_sources "$REPO" "$WORK/tree"
cd "$WORK/tree"
export SIFT_TMP=$WORK/cluster SIFT_ROOT=$WORK/root
scripts/pgtest.sh install
trap 'scripts/pgtest.sh stop >/dev/null 2>&1 || true; [[ -n ${KEEP:-} ]] || rm -rf "$WORK"' EXIT
scripts/pgtest.sh start
scripts/pgtest.sh regress "${TESTS[@]}"
scripts/pgtest.sh stop
scripts/pgtest.sh sanitizer-report
echo "leaks-asan: ${#TESTS[@]} tests passed on the ASan and UBSan build, no sanitizer finding"
