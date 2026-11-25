#!/usr/bin/env bash
# scripts/focus/NAME.tsv, the table of `scripts/ci.sh affected --focus` (one file for each regression test, and _hot.tsv): for each regression test, the functions of src/ it executes, measured with gcov.
#
#   scripts/focus-gen.sh [TEST...]    PostgreSQL 18 (SIFT_TMP, SIFT_PORT as scripts/pgtest.sh); no names: every test of test/sql and the hot list is made again
#
# A coverage build in a private copy of the tracked sources and one cluster life for each test (started with no counters, stopped when the test ends: a process
# writes its counters when it exits, and the first test after a start runs the shared state a feature sets up on first use, so a row would depend on the order
# of the tests otherwise). What the library executes on load (CREATE EXTENSION) is left out, and so is every function that more than HOT_PCT (10) percent of the tests execute: a change
# there can break any of them, `affected --focus` runs the tests of the area for it. With names, only their rows are replaced, and a file is written only when its bytes
# change (a new test registers with `scripts/focus-gen.sh NAME`; scripts/ci.sh --selftest fails on a test without a row); the hot list stays as it is.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
source "$REPO/scripts/source-snapshot.sh"
TESTS=("$@")
(( ${#TESTS[@]} )) || mapfile -t TESTS < <(cd "$REPO/test/sql" && ls ./*.sql | sed 's|^\./||; s|\.sql$||')
WORK=${SIFT_TMP:-/tmp/sift-focus-$$}/focus
rm -rf "$WORK" && mkdir -p "$WORK/tree" "$WORK/cov"
copy_git_sources "$REPO" "$WORK/tree"
cd "$WORK/tree"
export COPT=--coverage SHLIB_LINK=--coverage SIFT_TMP=$WORK/cluster SIFT_ROOT=$WORK/root SIFT_SUDO= SIFT_BUILD_DIR=$WORK/build
unset SANITIZE SIFT_INITDB_TEMPLATE INITDB_TEMPLATE
trap 'scripts/pgtest.sh stop >/dev/null 2>&1 || true; [[ -n ${KEEP:-} ]] || rm -rf "$WORK"' EXIT
scripts/pgtest.sh install >"$WORK/build.log" 2>&1 || { tail -n 20 "$WORK/build.log" >&2; exit 1; }

# the functions run since the counters were reset, as JSON on stdout: {"src/agg/cte/hook.c": ["cte_floor", ...]}
executed() {
	local f
	f=$(find "$SIFT_BUILD_DIR/src" -name '*.gcda')
	[[ -n $f ]] || { echo '{}'; return; }
	# shellcheck disable=SC2086
	gcov --json-format --stdout $f 2>/dev/null | python3 -I -c '
import json, re, sys
out, dec, s, i = {}, json.JSONDecoder(), sys.stdin.read(), 0
while True:
    while i < len(s) and s[i].isspace(): i += 1
    if i >= len(s): break
    o, i = dec.raw_decode(s, i)
    for f in o.get("files", []):
        if not f["file"].startswith("src/"): continue
        for fn in f.get("functions", []):
            if fn.get("execution_count", 0) > 0:
                out.setdefault(f["file"], set()).add(re.sub(r"(\.(isra|constprop|part|cold|lto_priv)(\.[0-9]+)?)+$", "", fn["name"]))
json.dump({k: sorted(v) for k, v in out.items()}, sys.stdout)'
}
reset() { find "$SIFT_BUILD_DIR/src" -name '*.gcda' -delete; }

# measure OUT COMMAND...: the functions the command runs in a cluster life of its own, as JSON in OUT
measure() {
	local out=$1
	shift
	reset
	scripts/pgtest.sh start >/dev/null
	"$@" >/dev/null 2>&1 || echo "focus-gen: $* failed (its rows are what ran)" >&2
	scripts/pgtest.sh stop >/dev/null || echo "focus-gen: the leak gate failed after $* (its rows are what ran)" >&2
	executed >"$out"
}

measure "$WORK/load.json" scripts/pgtest.sh psql -c 'CREATE EXTENSION pg_sift' -c 'DROP EXTENSION pg_sift'
for t in "${TESTS[@]}"; do measure "$WORK/cov/$t.json" nice scripts/pgtest.sh regress "$t"; done

OLD=$REPO/scripts/focus ALL=$(( ${#TESTS[@]} == $(ls "$REPO/test/sql"/*.sql | wc -l) )) HOT_PCT=${HOT_PCT:-10} \
	python3 -I "$REPO/scripts/focus-merge.py" "$WORK" "${TESTS[@]}"
echo "focus-gen: ${#TESTS[@]} tests measured"
