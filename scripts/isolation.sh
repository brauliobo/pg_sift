#!/usr/bin/env bash
# Isolation suite (test/isolation) driven by the server's pg_isolation_regress.
#
#   scripts/isolation.sh [--emulate] [--slice K/N] [SPEC...]
#
# Runs the specs against sift in the scripts/pgtest.sh cluster, skipping a
# spec while a feature named on its "# requires:" line reports "not
# implemented". --emulate runs every spec against a stand-in (GIN index DDL,
# SQL-reference matching; test/isolation/emulate.sql) to validate the specs,
# their choreography and the expected output before the features land.
# --slice K/N runs the Kth of N slices of the specs, of about the same time each: the longest spec first, each into the slice that has the least so far, by the
# seconds of scripts/isolation-weights.tsv (1 for a spec it does not list). --jobs J (SIFT_ISO_JOBS, default 4) runs the slice as J pg_isolation_regress at once,
# each in a database of its own of the cluster, packed the same way. A spec the weights mark quiet reads a state of the whole cluster (the xmin of a snapshot, the shared row locks), which the
# transactions the specs of the other databases keep open change: those run one after the other once the jobs are done. Results in $SIFT_TMP/isolation/<mode>/<job>. Exit 1 on any failure.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGOPTIONS='-c client_min_messages=warning'
ISO=$(dirname "$(pg_config --pgxs)")/../test/isolation/pg_isolation_regress
MODE=sift
[[ ${1:-} == --emulate ]] && { MODE=emulate; shift; }
OUT=${SIFT_TMP:-/tmp/sift-arch}/isolation/$MODE
DB=sift_iso_$MODE
SLICE=1/1 JOBS=${SIFT_ISO_JOBS:-4}
while [[ ${1:-} == --slice || ${1:-} == --jobs ]]; do
	if [[ $1 == --slice ]]; then SLICE=$2; else JOBS=$2; fi
	shift 2
done
SPECS=("$@")
(( ${#SPECS[@]} )) || mapfile -t SPECS < <(cd "$REPO/test/isolation/specs" && find . -name '*.spec' | sed 's|^\./||; s|\.spec$||' | LC_ALL=C sort)

# pack N: the specs on stdin as "bin<TAB>spec", bin 1..N: the longest first, each into the bin that holds the fewest seconds so far (the lowest on a tie)
WEIGHTS=$REPO/scripts/isolation-weights.tsv
pack() {
	awk 'FNR == NR { if ($1 !~ /^#/) w[$1] = $2; next } { print ($0 in w ? w[$0] : 1), $0 }' "$WEIGHTS" - | LC_ALL=C sort -k1,1nr -k2,2 |
		awk -v n="$1" '{ b = 1; for (i = 2; i <= n; i++) if (load[i] < load[b]) b = i; load[b] += $1; print b "\t" $2 }'
}
mapfile -t SPECS < <(printf '%s\n' "${SPECS[@]}" | pack "${SLICE#*/}" | awk -F'\t' -v k="${SLICE%/*}" '$1 == k { print $2 }')

# Emulation swaps sift index DDL for GIN, in specs and expected output alike
translate() {
	if [[ $MODE == emulate ]]; then
		sed -E "s/USING sift \(body\)( WITH \([^)]*\))?/USING gin (to_tsvector('simple', body))/"
	else
		cat
	fi
}
db() { psql -X -v ON_ERROR_STOP=1 -qAt -d "$DB" "$@"; }

rm -rf "$OUT" && mkdir -p "$OUT/in/specs" "$OUT/in/expected"
psql -X -qAt -d postgres -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB"
declare -A ok=([build]=1 [insert]=1 [vacuum]=1 [scan]=1 [promote]=1 [merge]=1 [verify]=1 [score]=1)
if [[ $MODE == emulate ]]; then
	db -v sift_ref_lib=1 -f "$REPO/test/sql/bm25/reference.sql" -f "$REPO/test/isolation/emulate.sql"
else
	db -c "CREATE EXTENSION pg_sift" -v sift_ref_lib=1 -f "$REPO/test/sql/bm25/reference.sql"
	ok[facets]=1 ok[index_info]=1
	[[ -n $(db -c "SELECT 1 FROM pg_am WHERE amname = 'sift_table'") ]] && ok[tam]=1
	[[ -n $(db -c "SELECT 1 FROM pg_am WHERE amname = 'sift_col'") ]] && ok[col]=1
	db -c "LOAD 'pg_sift'" -c "SHOW sift.test_phase_gate" >/dev/null 2>&1 && ok[gates]=1
fi
db -f "$REPO/test/isolation/helpers.sql"

run=()
for s in "${SPECS[@]}"; do
	f=$REPO/test/isolation/specs/$s.spec
	missing=$(for r in $(sed -n 's/^# requires: //p' "$f"); do [[ -n ${ok[$r]:-} ]] || echo "$r"; done)
	if [[ -n $missing ]]; then
		echo "SKIP $s (not implemented yet: $(echo $missing))"
		continue
	fi
	mkdir -p "$OUT/in/specs/$(dirname "$s")" "$OUT/in/expected/$(dirname "$s")"
	translate <"$f" >"$OUT/in/specs/$s.spec"
	for e in "$REPO/test/isolation/expected/$s.out" "$REPO/test/isolation/expected/${s}"_[0-9].out; do
		[[ ! -f $e ]] || translate <"$e" >"$OUT/in/expected/${e#"$REPO/test/isolation/expected/"}"
	done
	run+=("$s")
done
(( ${#run[@]} )) || { echo "isolation ($MODE): nothing runnable yet"; exit 0; }
source "$REPO/scripts/leakcheck.sh"
TMP=${SIFT_TMP:-/tmp/sift-arch}

# job NAME SPEC...: the specs in the database $DB_NAME (a copy of $DB), the output in $OUT/NAME, under the leak gate of that database
job() {
	local j=$1 jdb=${DB}_$1 rc=0
	shift
	printf "$OUT/$j/results/%s\n" "${@%/*}" | xargs mkdir -p
	psql -X -qAt -d postgres -c "DROP DATABASE IF EXISTS $jdb WITH (FORCE)" -c "CREATE DATABASE $jdb TEMPLATE $DB"
	[[ $MODE == emulate ]] || psql -X -qAt -d postgres -c "ALTER DATABASE $jdb SET iso.mode = 'sift'"
	leak_before "isolation-$MODE-$j" "$TMP/postgres.log"
	"$ISO" --use-existing --dbname="$jdb" --inputdir="$OUT/in" --outputdir="$OUT/$j" --bindir= "$@" >"$OUT/$j.log" 2>&1 || rc=1
	leak_after "isolation-$MODE-$j" "$TMP/data" "$jdb" >>"$OUT/$j.log" 2>&1 || rc=1
	psql -X -qAt -d postgres -c "DROP DATABASE $jdb" || rc=1
	echo "== job $j"
	cat "$OUT/$j.log"
	return $rc
}
mapfile -t quiet < <(printf '%s\n' "${run[@]}" | grep -Fxf <(awk -F'\t' '$3 == "quiet" { print $1 }' "$WEIGHTS") || true)
mapfile -t busy < <(printf '%s\n' "${run[@]}" | grep -vFxf <(printf '%s\n' "${quiet[@]}") || true)
(( JOBS <= ${#busy[@]} )) || JOBS=${#busy[@]}
pids=()
for ((j = 1; j <= JOBS; j++)); do
	mapfile -t specs < <(printf '%s\n' "${busy[@]}" | pack "$JOBS" | awk -F'\t' -v j="$j" '$1 == j { print $2 }')
	job "$j" "${specs[@]}" >"$OUT/$j.report" &
	pids+=($!)
done
rc=0
for ((j = 1; j <= JOBS; j++)); do wait "${pids[j - 1]}" || rc=1; cat "$OUT/$j.report"; done
(( ! ${#quiet[@]} )) || job quiet "${quiet[@]}" || rc=1
exit $rc
