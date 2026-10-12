#!/usr/bin/env bash
# Isolation suite (test/isolation) driven by PG18's pg_isolation_regress.
#
#   scripts/isolation.sh [--emulate] [SPEC...]
#
# Runs the specs against sift in the scripts/pgtest.sh cluster, skipping a
# spec while a feature named on its "# requires:" line reports "not
# implemented". --emulate runs every spec against a stand-in (GIN index DDL,
# SQL-reference matching; test/isolation/emulate.sql) to validate the specs,
# their choreography and the expected output before the features land.
# Results in $SIFT_TMP/isolation/<mode>. Exit 1 on any failure.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGOPTIONS='-c client_min_messages=warning'
ISO=$(dirname "$(pg_config --pgxs)")/../test/isolation/pg_isolation_regress
MODE=sift
[[ ${1:-} == --emulate ]] && { MODE=emulate; shift; }
OUT=${SIFT_TMP:-/tmp/sift-arch}/isolation/$MODE
DB=sift_iso_$MODE
SPECS=("$@")
(( ${#SPECS[@]} )) || SPECS=($(basename -s .spec "$REPO"/test/isolation/specs/*.spec))

# Emulation swaps sift index DDL for GIN, in specs and expected output alike
translate() {
	if [[ $MODE == emulate ]]; then
		sed -E "s/USING sift \(body\)( WITH \([^)]*\))?/USING gin (to_tsvector('simple', body))/"
	else
		cat
	fi
}
db() { psql -X -v ON_ERROR_STOP=1 -qAt -d "$DB" "$@"; }
# A feature is missing only while sift says so; other errors surface in the specs
has() {
	local err
	err=$(db -c "$1" 2>&1 >/dev/null) || true
	[[ $err != *"not implemented"* ]]
}

rm -rf "$OUT" && mkdir -p "$OUT/in/specs" "$OUT/in/expected"
psql -X -qAt -d postgres -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB"
declare -A ok=([build]=1 [insert]=1 [vacuum]=1 [scan]=1 [promote]=1 [merge]=1 [verify]=1 [score]=1)
if [[ $MODE == emulate ]]; then
	db -v sift_ref_lib=1 -f "$REPO/test/sql/reference.sql" -f "$REPO/test/isolation/emulate.sql"
else
	db -c "CREATE EXTENSION sift" -c "ALTER DATABASE $DB SET iso.mode = 'sift'" -v sift_ref_lib=1 -f "$REPO/test/sql/reference.sql"
	db -c "CREATE TABLE iso_probe (id int, body text)" -c "INSERT INTO iso_probe VALUES (1, 'probe')"
	has "CREATE INDEX iso_probe_idx ON iso_probe USING sift (body)" || ok=()
	if [[ -n ${ok[build]:-} ]]; then
		has "INSERT INTO iso_probe VALUES (2, 'probe two')" || unset 'ok[insert]'
		has "VACUUM iso_probe" || unset 'ok[vacuum]'
		has "SET enable_seqscan = off; SELECT count(*) FROM iso_probe WHERE body @@> 'probe'" || unset 'ok[scan]'
		has "SELECT sift.promote('iso_probe_idx')" || unset 'ok[promote]'
		has "SELECT sift.merge('iso_probe_idx')" || unset 'ok[merge]'
		has "SELECT sift.verify('iso_probe_idx')" || unset 'ok[verify]'
		has "SET enable_seqscan = off; SELECT sift.score(ctid) FROM iso_probe WHERE body @@> 'probe' ORDER BY 1 DESC LIMIT 1" ||
			unset 'ok[score]'
	fi
	db -c "DROP TABLE iso_probe"
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
	translate <"$f" >"$OUT/in/specs/$s.spec"
	translate <"$REPO/test/isolation/expected/$s.out" >"$OUT/in/expected/$s.out"
	run+=("$s")
done
(( ${#run[@]} )) || { echo "isolation ($MODE): nothing runnable yet"; exit 0; }
"$ISO" --use-existing --dbname="$DB" --inputdir="$OUT/in" --outputdir="$OUT" --bindir= "${run[@]}"
