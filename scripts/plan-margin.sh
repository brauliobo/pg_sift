#!/usr/bin/env bash
# The gap between the price of the scan and of the normal plan, for a plan a test asserts: a plan chosen on a tie (a few percent) flips with the statistics or the runner.
#
#   scripts/plan-margin.sh [--min PCT] [-d DB] STATEMENT     per arm of the statement (sift.explain_why): the scan's price, the normal plan's, the gap
#   scripts/plan-margin.sh [--min PCT] [--all] [--tsv] --tests TEST...  each test/sql/TEST.sql run in a scratch database with sift_helpers.margin = on: the plan helpers of
#                                                            pg_sift_helpers print the same line for every statement they classify; one line per test
#
# The gap is |scan - normal| / the dearer, in percent; --min (default 25) fails below it. A test that asserts a plan the planner chose on its costs keeps the numbers beside the
# assertion (`-- scan 940.9 against normal 472.9: a gap of 50%`), as test/sql/agg_few_keys_handful_units.sql does. Needs the scripts/pgtest.sh cluster, installed and started.
# The EXECUTE 'EXPLAIN ...' || q of a test's own helper is rewritten on the way in to price q first; a test that reads the plan another way prints nothing: run its statement
# through the first form. A test that forces the scan (sift.agg_force = on) is not decided by the gap: explain_why prices the arms without the force.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
MIN=25 DB=${SIFT_DB:-postgres} TESTS=() ALL= TSV=
while (( $# )); do
	case $1 in
		--min) MIN=$2; shift ;; --all) ALL=1 ;; --tsv) TSV=1 ;; -d) DB=$2; shift ;; --tests) shift; TESTS=("$@"); break ;;
		-h|--help) sed -n 2,11p "$0"; exit 0 ;;
		*) break ;;
	esac
	shift
done
psql() { SIFT_DB=$1 "$REPO/scripts/pgtest.sh" psql -X -v ON_ERROR_STOP=1 "${@:2}"; }

gap() {	# the arms of a statement: arm, scan, normal, gap; fails when one is under MIN
	psql "$DB" -qAt -F $'\t' -c "SELECT arm, round(scan_cost::numeric, 1), round(normal_cost::numeric, 1), round((100 * abs(scan_cost - normal_cost) / greatest(scan_cost, normal_cost))::numeric, 0)
		FROM sift.explain_why(\$q\$$1\$q\$) WHERE scan_cost < 'Infinity' AND normal_cost < 'Infinity' ORDER BY arm" |
		awk -F'\t' -v min="$MIN" '{ printf "%s: scan %s, normal %s, gap %s%%\n", $1, $2, $3, $4; if ($4 + 0 < min) bad = 1 } END { exit bad }'
}

if (( ${#TESTS[@]} == 0 )); then
	[[ $# -eq 1 ]] || { sed -n 2,11p "$0"; exit 1; }
	gap "$1"
	exit
fi

rc=0
for t in "${TESTS[@]}"; do
	f=$REPO/test/sql/$t.sql
	[[ -f $f ]] || { echo "plan-margin: no $f" >&2; rc=1; continue; }
	db=plan_margin_$t
	psql postgres -qAt -c "SET client_min_messages = warning" -c "DROP DATABASE IF EXISTS $db WITH (FORCE)" -c "CREATE DATABASE $db" >/dev/null
	# the test's own plan helpers (EXECUTE 'EXPLAIN ...' || q) are made to print the price too: the line before them calls margin_note(q)
	psql "$db" -q -c 'SET client_min_messages = warning' -c 'CREATE EXTENSION pg_sift_helpers CASCADE' >/dev/null
	out=$(sed -E "s/(FOR [a-z_]+ IN )?EXECUTE 'EXPLAIN ([^']*)' \|\| ([A-Za-z_0-9.]+(\([^)]*\))?)/PERFORM sift_helpers.margin_note(\3); \1EXECUTE 'EXPLAIN \2' || \3/" "$f" |
		PGOPTIONS='-c client_min_messages=notice -c sift_helpers.margin=on' "$REPO/scripts/pgtest.sh" psql -X -q -d "$db" 2>&1 | grep -o 'plan-margin: .*' || true)
	psql postgres -qAt -c "DROP DATABASE IF EXISTS $db WITH (FORCE)" >/dev/null
	# how the test fixes its plan: sift.agg_force or the steering of the costs make the gap beside the point, else the plan is the planner's own choice
	how=chosen; grep -qE 'seq_page_cost|random_page_cost|enable_(seqscan|indexscan|bitmapscan|agg)|_mode\(' "$f" && how=steered; grep -q 'sift.agg_force' "$f" && how=forced
	if [[ -n $TSV ]]; then
		if [[ -z $out ]]; then printf '%s\t0\t-\t%s\tits own helper reads the plan\n' "$t" "$how"
		else awk -v t="$t" -v how="$how" '{ n++; g = $NF; sub(/%/, "", g); if (m == "" || g + 0 < m + 0) m = g } END { printf "%s\t%d\t%s\t%s\tpriced\n", t, n, m, how }' <<<"$out"; fi
		continue
	fi
	if [[ -z $out ]]; then echo "$t: no statement priced (its own helper reads the plan, or the plan is forced)"; continue; fi
	[[ -z $ALL ]] || awk -v min="$MIN" '{ g = $NF; sub(/%/, "", g); if (g + 0 < min) print "    " $0 }' <<<"$out"
	awk -v t="$t" -v min="$MIN" '{ n++; g = $NF; sub(/%/, "", g); if (min_g == "" || g + 0 < min_g + 0) { min_g = g; worst = $0 } }
		END { printf "%s: %d statements priced, narrowest %s%%%s\n", t, n, min_g, (min_g + 0 < min) ? "  <-- " worst : ""; exit (min_g + 0 < min) }' <<<"$out" || rc=1
done
exit $rc
