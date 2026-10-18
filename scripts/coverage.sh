#!/usr/bin/env bash
# The coverage table (docs/COVERAGE.md): what sift accelerates and what falls back to the normal PostgreSQL plan, generated from one file,
# docs/coverage.tsv, and proven by running its sample queries on a test cluster.
#
#   scripts/coverage.sh [--check] [--rows REGEX] [--assume-merged] [--keep] [-v]   run the proofs of the rows (--check also proves the proofs)
#   scripts/coverage.sh --gen                                                       write docs/COVERAGE.md and docs/README-coverage-block.md
#
# Each row of docs/coverage.tsv that has a sample query runs with EXPLAIN on a private fixture database (test/col/fixture.sql,
# test/col/coverage_data.sql): the plan must have the node it claims (Custom Scan (SiftAgg), SiftScan, SiftCount, a bitmap scan on the
# sift index) or none (the normal plan), and the result must equal the one of the same query with every sift plan off
# (sift.enable_agg, enable_scan, enable_count and enable_col off, no btree or bitmap scans). A row claiming an error runs its statement and
# expects that SQLSTATE, and, when the row names a setting that avoids it, the statement without the error under that setting. Rows
# without a query are "doc only". --check exits 1 when a claim is wrong, a sample query returns another result than the normal plan, the
# table is malformed, or docs/COVERAGE.md and the README block are not the ones --gen writes (the docs would drift).
#
# A row with a `since` branch is a feature of a branch that is not merged yet: the table says "pending merge" and --check expects its claim
# NOT to hold in this build (the result must still equal the normal plan), and fails when it does (drop `since`). --assume-merged expects
# those claims to hold: the run on a build of that branch.
#
# Needs the scripts/pgtest.sh cluster installed and started (SIFT_TMP, SIFT_PORT); scripts/ci.sh runs it as the job `coverage`.
# Columns of docs/coverage.tsv (tab separated, no tab or newline inside a field; `\n` in a query is a newline):
#   1 id  2 area  3 query/feature  4 status (Yes|Partly|No|Error)  5 condition or reason (the setting that avoids an Error)  6 engine
#   7 example  8 notes  9 proof (fixture or test)  10 check  11 query (or @fixture name, or -)  12 since (branch, pending merge)
#   13 avoid ("name = value", Error rows)  14 readme group (Search|Analytics|Tables: the row is in the README block)
# check: doc | agg | plain | scan | count | bitmap | noscan | ok | error:SQLSTATE, each of the first eight optionally followed by
#   " ~ REGEX" or " !~ REGEX" on the plan (test/col/coverage.sql)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch}
TSV=${COVERAGE_TSV:-$REPO/docs/coverage.tsv}
OUT=$SIFT_TMP/coverage
DB=sift_coverage
CHECK= GEN= ROWS= MERGED= KEEP= VERBOSE= FIXROWS=${COVERAGE_ROWS:-2000}

while (( $# )); do
	case $1 in
		--check) CHECK=1 ;; --gen) GEN=1 ;; --assume-merged) MERGED=1 ;; --keep) KEEP=1 ;; -v|--verbose) VERBOSE=1 ;;
		--rows) ROWS=$2; shift ;;
		*) sed -n '2,26p' "$0"; exit 1 ;;
	esac
	shift
done

log() { printf '[coverage %s] %s\n' "$(date +%T)" "$*" >&2; }
sql() { PGOPTIONS='-c client_min_messages=warning -c lc_messages=C' SIFT_DB=${SQLDB:-$DB} "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 -qAt "$@"; }

# ---------- the table ----------
# validate: the rules of the file; prints the errors and returns 1 when there is one
validate() {
	awk -F'\t' '
		function bad(m) { printf "docs/coverage.tsv:%d: %s: %s\n", NR, $1, m; n++ }
		/^#/ || $1 == "id" { next }
		NF != 14 { bad("expected 14 tab separated fields, found " NF); next }
		rows != "" && $1 !~ rows { next }
		{
			if ($1 !~ /^[A-Z]+[0-9]+$/) bad("id must look like A01")
			if ($1 in seen) bad("duplicate id"); seen[$1] = 1
			if ($4 !~ /^(Yes|Partly|No|Error)$/) bad("status must be Yes, Partly, No or Error")
			if ($2 == "" || $3 == "" || $6 == "" || $7 == "") bad("area, feature, engine and example are required")
			if ($4 == "Yes" && $9 == "") bad("a Yes row names its proof (fixture or test)")
			if ($4 != "Yes" && $5 == "") bad("a Partly, No or Error row says why (condition, reason, or the setting to avoid)")
			if ($10 !~ /^(doc|(agg|plain|scan|count|bitmap|noscan|ok)( !?~ .+)?|error:[0-9A-Z]{5})$/) bad("unknown check " $10)
			if (($10 == "doc") != ($11 == "-")) bad("doc rows have the query -, the others a query")
			if ($4 == "Yes" && $10 !~ /^(doc|agg|scan|count|bitmap|ok)/) bad("a Yes row claims a sift plan or only a result")
			if ($4 == "No" && $10 !~ /^(doc|plain|noscan|ok)/) bad("a No row claims the normal plan")
			if ($4 == "Error" && $10 !~ /^(doc|error:)/) bad("an Error row claims a SQLSTATE")
			if ($10 !~ /^error:/ && $13 != "") bad("only an error row has an avoid setting")
			if ($14 != "" && $14 !~ /^(Search|Analytics|Tables)$/) bad("readme group must be Search, Analytics or Tables")
			if ($11 ~ /^@/ && system("test -f \"" fx "/" substr($11, 2) ".sql\"") != 0) bad("no fixture " substr($11, 2))
			np = split($9, pf, ",")
			for (i = 1; i <= np && $12 == ""; i++) {
				k = index(pf[i], ":"); kind = substr(pf[i], 1, k - 1); name = substr(pf[i], k + 1)
				if (kind == "fixture" && system("test -f \"" fx "/" name ".sql\"") != 0) bad("no fixture " name)
				else if (kind == "test" && system("test -f \"" fx "/../../sql/" name ".sql\" || ls \"" fx "/../../t/" name ".\"* >/dev/null 2>&1") != 0) bad("no test " name)
				else if (kind == "script" && system("test -f \"" fx "/../../../scripts/" name ".sh\"") != 0) bad("no script " name)
				else if (kind == "scenario" && name !~ /^(insert|update|delete|abort|vacuum|hot|recycle|held|maint)$/) bad("no scenario " name)
				else if (kind !~ /^(fixture|test|script|scenario)$/) bad("proof must be fixture:NAME, test:NAME, script:NAME or scenario:NAME, not " pf[i])
			}
		}
		END { exit n > 0 }
	' fx="$REPO/test/col/fixtures" rows="$ROWS" "$TSV"
}

# ---------- generation ----------
# status of a row as the table shows it
STATUS_AWK='
	function md(s) { gsub(/\|/, "\\|", s); return s }
	function status(st, cond, since) {
		if (since != "") cond = (cond == "" ? "" : cond "; ") "in v3, pending merge"
		return st (cond == "" ? "" : " (" md(cond) ")")
	}
	function code(s) { return s == "" ? "" : "`" md(s) "`" }
	function proofs(p,   a, n, i, k, out) {
		n = split(p, a, ",")
		for (i = 1; i <= n; i++) { k = index(a[i], ":"); out = out (i > 1 ? ", " : "") substr(a[i], 1, k - 1) " `" substr(a[i], k + 1) "`" }
		return out
	}
'

gen_full() {
	cat <<'EOF'
# What sift covers

Generated from [coverage.tsv](coverage.tsv) by `scripts/coverage.sh --gen`; `scripts/coverage.sh --check` runs the sample query of each row with
`EXPLAIN` on a test cluster and fails when a row claims what the plan does not do (the job `coverage` of `scripts/ci.sh`). Edit the `.tsv`,
never this file.

- **Yes**: the query runs on a sift index or table (`Custom Scan (SiftAgg)` for aggregates on a `sift_col` index or a `sift_table`,
  `Custom Scan (SiftScan)` / `SiftCount` for the BM25 `sift` index) when the planner can prove the result equals the normal plan's and the cost
  says it pays. A table of a few thousand rows is cheaper on the normal plan: the check raises `seq_page_cost` so that the shape, not the size,
  decides. The proof of a row is its fixture (`test/col/fixtures/<name>.sql`, run by `scripts/coldiff.sh` against `sift.enable_agg = off`
  on every variant and heap state) or a regress test.
- **Partly (condition)**: Yes when the condition holds; otherwise the row below it, or the normal plan.
- **No (reason)**: the query keeps the normal PostgreSQL plan. It is correct, never wrong and never an error, only not accelerated.
  `SET sift.enable_agg = off` shows that plan for any query.
- **Error (setting)**: sift raises an error (SQLSTATE `0A000` unless the row says so) instead of a result; the row names the setting or the
  rewrite that avoids it.
- **in v3, pending merge**: the feature is on a v3 branch that is not merged yet; the build of this tree still falls back to the normal plan
  for it, and the check fails when that stops being true, so the row cannot outlive the merge.
- **doc only**: no sample query (the claim rests on the test named in the notes).

To see what a query does: `EXPLAIN (COSTS OFF) <query>`; a `Custom Scan (SiftAgg)` with `Pushed Aggregates`, `Group Keys` and `Lookup Joins`
lines is the column index at work, `SiftScan` / `SiftCount` is the BM25 index, anything else is the normal plan.

EOF
	awk -F'\t' "$STATUS_AWK"'
		/^#/ || $1 == "id" { next }
		{ n[$4]++; total++; if ($12 != "") pend++; if ($10 == "doc") docs++; row[++r] = $0 }
		END {
			printf "%d rows: %d Yes, %d Partly, %d No, %d Error; %d with a proof query, %d doc only; %d pending merge.\n\n", total, n["Yes"], n["Partly"], n["No"], n["Error"], total - docs, docs, pend
			print "| Id | Area | Query / feature | Covered by sift? | Index or engine | Example | Notes |"
			print "|---|---|---|---|---|---|---|"
			for (i = 1; i <= r; i++) {
				split(row[i], f, "\t")
				notes = md(f[8]); if (f[9] != "") notes = notes (notes == "" ? "" : " ") "Proof: " proofs(f[9]) "."
				if (f[10] == "doc") notes = notes (notes == "" ? "" : " ") "(doc only)"
				printf "| %s | %s | %s | %s | %s | %s | %s |\n", f[1], md(f[2]), md(f[3]), status(f[4], f[5], f[12]), md(f[6]), code(f[7]), notes
			}
		}' "$TSV"
}

gen_block() {
	cat <<'EOF'
<!-- coverage:start (generated by `scripts/coverage.sh --gen` from docs/coverage.tsv; do not edit by hand) -->
## What is covered

**Yes** means the query runs on a sift index or table: `Custom Scan (SiftScan)` or `SiftCount` for the BM25 `sift` index, `Custom Scan (SiftAgg)`
for aggregates over a `sift_col` index or a `sift_table`. **Partly** is Yes under the condition in brackets. **No** means the query keeps
the normal PostgreSQL plan: the result is the same, correct, only not accelerated, never wrong and never an error; a **No** is a query shape that is
not proven yet, not a limit of what you can write. **Error** is the few cases where sift raises an error instead (the setting that avoids it is in
brackets). To check a query, run `EXPLAIN (COSTS OFF)` on it: a `Custom Scan (SiftAgg)` (or `SiftScan`, `SiftCount`) node is sift, anything else is
the normal plan, and `SET sift.enable_agg = off` gives the plan PostgreSQL has without sift to compare results. Every row below has a sample query that is run on a test cluster by
`scripts/coverage.sh --check`; the complete table (every aggregate, type, key, predicate, join and limit, with the proof of each row) is in
[docs/COVERAGE.md](docs/COVERAGE.md).

EOF
	local g
	for g in Search Analytics Tables; do
		case $g in
			Search) echo "**Search** (BM25 \`sift\` index)"; echo ;;
			Analytics) echo "**Analytics** (\`sift_col\` index on a heap table or a \`sift_table\`: aggregates, \`GROUP BY\`)"; echo ;;
			Tables) echo "**Tables** (\`sift_table\` access method)"; echo ;;
		esac
		awk -F'\t' -v g="$g" "$STATUS_AWK"'
			/^#/ || $1 == "id" || $14 != g { next }
			NR_ok == 0 { print "| Query / feature | Covered by sift? | Index or engine | Example |"; print "|---|---|---|---|"; NR_ok = 1 }
			{ printf "| %s | %s | %s | %s |\n", md($3), status($4, $5, $12), md($6), code($7) }' "$TSV"
		echo
	done
	echo '<!-- coverage:end -->'
}

generate() { gen_full >"$1/COVERAGE.md"; gen_block >"$1/README-coverage-block.md"; }

# ---------- the proofs ----------
setup_db() {
	SQLDB=postgres sql -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "DROP ROLE IF EXISTS sift_cov_user" -c "CREATE DATABASE $DB"
	sql -c "CREATE EXTENSION pg_sift"
	sql -f "$REPO/test/col/run.sql" -f "$REPO/test/col/coverage.sql"
	sql -v n="$FIXROWS" -v seed=0 -f "$REPO/test/col/fixture.sql" >/dev/null
	sql -f "$REPO/test/col/coverage_data.sql"
	sql -c "VACUUM (FREEZE, ANALYZE)"
}

# the psql script that runs every selected row, one RESET ALL and one coverage.check each
write_script() {
	mkdir -p "$OUT/q"
	awk -F'\t' -v dir="$OUT/q" -v fx="$REPO/test/col/fixtures" -v rows="$ROWS" '
		function lit(s) { gsub(/\047/, "\047\047", s); return "\047" s "\047" }
		/^#/ || $1 == "id" || $10 == "doc" { next }
		rows != "" && $1 !~ rows { next }
		{
			if ($11 ~ /^@/) src = fx "/" substr($11, 2) ".sql"
			else { q = $11; gsub(/\\n/, "\n", q); src = dir "/" $1 ".sql"; print q > src; close(src) }
			print "RESET ALL;"
			print "\\set q `cat " src "`"
			print "SELECT coverage.check(" lit($1) ", " lit($10) ", :\047q\047, " ($13 == "" ? "NULL" : lit($13)) ");"
		}' "$TSV" >"$OUT/rows.sql.body"
	{ echo "\\set ON_ERROR_STOP 0"; echo "LOAD 'pg_sift';"; echo "\\o /dev/null"; echo "TRUNCATE coverage.results;"; cat "$OUT/rows.sql.body"; echo "\\o"; } >"$OUT/rows.sql"
}

# run: the verdict of every row; exit status 1 when a claim is wrong
run_proofs() {
	local fails
	write_script
	sql -f "$OUT/rows.sql" >/dev/null 2>"$OUT/rows.err" || true
	sql -F $'\t' -c "SELECT id, ok, kind, coverage.nodes(plan), replace(coalesce(detail, ''), E'\n', ' ') FROM coverage.results ORDER BY id" >"$OUT/results.tsv"
	fails=$(awk -F'\t' -v merged="$MERGED" -v verbose="$VERBOSE" -v rows="$ROWS" '
		FILENAME == ARGV[1] { if ($0 !~ /^#/ && $1 != "id" && $10 != "doc" && (rows == "" || $1 ~ rows)) { claim[$1] = $10; since[$1] = $12; status[$1] = $4 } ; next }
		{ got[$1] = 1
		  ok = ($2 == "t"); kind = $3; nodes = $4; detail = $5
		  pending = (since[$1] != "" && merged == "")
		  if (pending && ok) { verdict = "FAIL"; msg = "the claim holds in this build: drop `since` (" since[$1] " is merged)" }
		  else if (pending && !ok && kind == "plan") { verdict = "pend"; msg = "not in this build yet (" since[$1] "): " detail }
		  else if (ok) { verdict = "ok"; msg = "" }
		  else { verdict = "FAIL"; msg = detail }
		  nv[verdict]++
		  if (verdict == "FAIL" || verbose != "") printf "%-4s %-5s %-34s [%s] %s\n", verdict, $1, claim[$1], nodes, msg > "/dev/stderr"
		}
		END {
			for (id in claim) if (!(id in got)) { printf "FAIL %-5s %-34s no result (the check itself raised an error: see rows.err)\n", id, claim[id] > "/dev/stderr"; nv["FAIL"]++ }
			printf "coverage: %d rows run: %d ok, %d pending merge (claim not in this build), %d FAIL\n", length(claim), nv["ok"], nv["pend"], nv["FAIL"] > "/dev/stderr"
			print nv["FAIL"] + 0
		}' "$TSV" "$OUT/results.tsv")
	[[ $fails == 0 ]] || { [[ ! -s $OUT/rows.err ]] || grep -v -E 'NOTICE|skipping' "$OUT/rows.err" | head -20 >&2; return 1; }
}

cleanup() {
	[[ -n $KEEP ]] && { log "kept: database $DB, files in $OUT"; return; }
	SQLDB=postgres sql -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "DROP ROLE IF EXISTS sift_cov_user" >/dev/null 2>&1 || true
	rm -rf "$OUT"
}

# --check proves the proofs themselves: a table with wrong claims must fail on exactly those rows
selftest() {
	local mut=$OUT/mutated.tsv out n
	awk -F'\t' -v OFS='\t' '$1 == "A01" { $10 = "plain" } $1 == "T07" { $10 = "agg" } $1 == "T05" { $10 = "error:42601" } $1 == "A02" { $12 = "feat/v3-merged" } { print }' "$TSV" >"$mut"
	out=$(TSV=$mut ROWS='^(A01|T07|T05|A02)$' MERGED= run_proofs 2>&1 >/dev/null || true)
	n=$(grep -c '^FAIL' <<<"$out" || true)
	(( n == 4 )) || { echo "coverage: selftest: 4 wrong claims (a plan that is not there, a node that is, a SQLSTATE, a merged branch) must fail, $n did:" >&2; echo "$out" >&2; return 1; }
	log "selftest: the 4 wrong claims failed as they should"
}

# ---------- main ----------
validate || { echo "coverage: docs/coverage.tsv is malformed" >&2; exit 1; }
if [[ -n $GEN ]]; then
	generate "$REPO/docs"
	log "wrote docs/COVERAGE.md and docs/README-coverage-block.md ($(grep -c -v -E '^(#|id	)' "$TSV") rows)"
	exit 0
fi
status=0
if [[ -n $CHECK ]]; then
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/sift-coverage.XXXXXX")
	generate "$tmp"
	for f in COVERAGE.md README-coverage-block.md; do
		cmp -s "$tmp/$f" "$REPO/docs/$f" || { echo "coverage: docs/$f is not what docs/coverage.tsv generates: run scripts/coverage.sh --gen" >&2; status=1; }
	done
	rm -rf "$tmp"
fi
trap cleanup EXIT
mkdir -p "$OUT"
log "database: $FIXROWS orders"
setup_db
run_proofs || status=1
[[ -z $CHECK || -n $ROWS ]] || selftest || status=1
[[ -n $CHECK ]] || exit 0
exit $status
