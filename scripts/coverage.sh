#!/usr/bin/env bash
# The coverage table (docs/COVERAGE.md): what sift accelerates and what falls back to the normal PostgreSQL plan, generated from one file,
# docs/coverage.tsv, and proven by running its sample queries on a test cluster.
#
#   scripts/coverage.sh [--check] [--rows REGEX] [--keep] [-v]                      run the proofs of the rows (--check also proves the proofs)
#   scripts/coverage.sh --gen                                                       write docs/COVERAGE.md and the coverage block of README.md
#
# Each row of docs/coverage.tsv that has a sample query runs with EXPLAIN on a private fixture database (test/col/fixture.sql,
# test/col/coverage_data.sql): the plan must have the node it claims (Custom Scan (SiftAgg), SiftScan, SiftCount, a bitmap scan on the
# sift index) or none (the normal plan), and the result must equal the one of the same query with every sift plan off
# (sift.enable_agg, enable_scan, enable_count and enable_col off, no btree or bitmap scans). A row claiming an error runs its statement and
# expects that SQLSTATE, and, when the row names a setting that avoids it, the statement without the error under that setting. Rows
# without a query are "doc only" (none left: a claim about a setting, a catalog or a replica has a query that observes it, `true`, or an
# `external:` proof on a second cluster). --check exits 1 when a claim is wrong, a sample query returns another result than the normal plan, the
# table is malformed, or docs/COVERAGE.md and the coverage block of README.md are not the ones --gen writes (the docs would drift).
#
# A row with a `since` feature is the claim of a v3 branch that is not merged yet. The file declares the feature in a line
# `#feature<TAB>id<TAB>branch<TAB>marker file[<TAB>regex]`; the feature is in the build when the marker file (a fixture the branch adds, or one
# whose `-- expect:` it changes: the regex) is in the tree. Until
# then the table says "pending merge" and --check expects the claim NOT to hold (the result must still equal the normal plan); once the marker
# is in the tree the same row is a plain row that must hold, so a merge needs no edit but `scripts/coverage.sh --gen` for the docs.
#
# Needs the scripts/pgtest.sh cluster installed and started (SIFT_TMP, SIFT_PORT); scripts/ci.sh runs it as the job `coverage`.
# Columns of docs/coverage.tsv (tab separated, no tab or newline inside a field; `\n` in a query is a newline):
#   1 id  2 area  3 query/feature  4 status (Yes|Partly|No|Error)  5 condition or reason (the setting that avoids an Error)  6 engine
#   7 example  8 notes  9 proof (fixture or test)  10 check  11 query (or @fixture name; `\n` is a newline; a line `-- pre: PSQL` runs first)
#   12 since (a #feature id)  13 avoid ("name = value", Error rows)  14 README family ("NN:short SQL": the rows with the same NN are one row of the
#      table in README.md, in the order of NN; its status is the one they share, Partly when they differ; its engine, the first of its first row)
# check: agg | plain | scan | count | bitmap | noscan | ok | true | error:SQLSTATE | external:NAME (a proof on a second cluster, below), each of
#   the first six optionally followed by " ~ REGEX" or " !~ REGEX" on the plan (test/col/coverage.sql)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch}
TSV=${COVERAGE_TSV:-$REPO/docs/coverage.tsv}
OUT=$SIFT_TMP/coverage
DB=sift_coverage
CHECK= GEN= ROWS= KEEP= VERBOSE= FIXROWS=${COVERAGE_ROWS:-2000}

while (( $# )); do
	case $1 in
		--check) CHECK=1 ;; --gen) GEN=1 ;; --keep) KEEP=1 ;; -v|--verbose) VERBOSE=1 ;;
		--rows) ROWS=$2; shift ;;
		*) sed -n '2,30p' "$0"; exit 1 ;;
	esac
	shift
done

log() { printf '[coverage %s] %s\n' "$(date +%T)" "$*" >&2; }
sql() { PGOPTIONS='-c client_min_messages=warning -c lc_messages=C' SIFT_DB=${SQLDB:-$DB} "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 -qAt "$@"; }

# ---------- the table ----------
# the features of the rows that are not in this tree yet: " id id ", from the #feature lines (id, branch, marker file that the branch adds)
pending_features() {
	awk -F'\t' -v repo="$REPO" '$1 == "#feature" { f = repo "/" $4; if (system("test -e \"" f "\"" ($5 == "" ? "" : " && grep -Eq -e \"" $5 "\" \"" f "\"")) != 0) p = p " " $2 } END { print p " " }' "$TSV"
}

# validate: the rules of the file; prints the errors and returns 1 when there is one
validate() {
	awk -F'\t' '
		function bad(m) { printf "docs/coverage.tsv:%d: %s: %s\n", NR, $1, m; n++ }
		$1 == "#feature" { if (NF < 4 || NF > 5 || $2 == "" || $4 == "") bad("a #feature line is #feature, id, branch, marker file, optional regex of the file"); feat[$2] = 1; next }
		/^#/ || $1 == "id" { next }
		NF != 14 { bad("expected 14 tab separated fields, found " NF); next }
		rows != "" && $1 !~ rows { next }
		{
			pend = $12 != "" && index(P, " " $12 " ")
			if ($12 != "") used[$12] = $1
			if ($1 !~ /^[A-Z]+[0-9]+$/) bad("id must look like A01")
			if ($1 in seen) bad("duplicate id"); seen[$1] = 1
			if ($4 !~ /^(Yes|Partly|No|Error)$/) bad("status must be Yes, Partly, No or Error")
			if ($2 == "" || $3 == "" || $6 == "" || $7 == "") bad("area, feature, engine and example are required")
			if ($4 == "Yes" && $9 == "") bad("a Yes row names its proof (fixture or test)")
			if ($4 != "Yes" && $5 == "") bad("a Partly, No or Error row says why (condition, reason, or the setting to avoid)")
			if ($10 !~ /^((agg|plain|scan|count|bitmap|noscan|ok)( !?~ .+)?|true|error:[0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z]|external:[a-z-]+)$/) bad("unknown check " $10)
			if ($11 == "-" || $11 == "") bad("every row has a proof query")
			if ($4 == "Yes" && $10 !~ /^(agg|scan|count|bitmap|ok|true|external:)/) bad("a Yes row claims a sift plan, a result or a probe")
			if ($4 == "No" && $10 !~ /^(plain|noscan|ok|true)/) bad("a No row claims the normal plan")
			if ($4 == "Error" && $10 !~ /^(error:|true|external:)/) bad("an Error row claims a SQLSTATE")
			if ($10 !~ /^error:/ && $13 != "") bad("only an error row has an avoid setting")
			if ($14 != "" && $14 !~ /^[0-9][0-9]:./) bad("README family must be NN:short SQL")
			if ($14 != "") { k = substr($14, 1, 2); if (k in famname && famname[k] != $14) bad("README family " k " has two names"); famname[k] = $14 }
			if ($11 ~ /^@/ && !pend && system("test -f \"" fx "/" substr($11, 2) ".sql\"") != 0) bad("no fixture " substr($11, 2))
			np = split($9, pf, ",")
			for (i = 1; i <= np && !pend; i++) {
				k = index(pf[i], ":"); kind = substr(pf[i], 1, k - 1); name = substr(pf[i], k + 1)
				if (kind == "fixture" && system("test -f \"" fx "/" name ".sql\"") != 0) bad("no fixture " name)
				else if (kind == "test" && system("test -f \"" fx "/../../sql/" name ".sql\" || ls \"" fx "/../../t/" name ".\"* >/dev/null 2>&1") != 0) bad("no test " name)
				else if (kind == "script" && system("test -f \"" fx "/../../../scripts/" name ".sh\"") != 0) bad("no script " name)
				else if (kind == "scenario" && name !~ /^(insert|update|delete|abort|vacuum|hot|recycle|held|maint)$/) bad("no scenario " name)
				else if (kind !~ /^(fixture|test|script|scenario)$/) bad("proof must be fixture:NAME, test:NAME, script:NAME or scenario:NAME, not " pf[i])
			}
		}
		END { for (u in used) if (!(u in feat)) bad("since " u " (" used[u] ") is no #feature"); exit n > 0 }
	' fx="$REPO/test/col/fixtures" rows="$ROWS" P="$(pending_features)" "$TSV"
}

# ---------- generation ----------
# status of a row as the table shows it
STATUS_AWK='
	function md(s) { gsub(/\|/, "\\|", s); return s }
	function pend(f) { return f != "" && index(P, " " f " ") }
	function status(st, cond, since) {
		if (pend(since)) cond = (cond == "" ? "" : cond "; ") "in v3, pending merge"
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
- **in v3, pending merge**: the feature is on a v3 branch that is not in this tree yet; the build of this tree falls back to the normal plan
  for it, and the check fails when that stops being true. The check finds the feature by a file the branch adds (the `#feature` lines of
  the `.tsv`), so after the merge the same row is checked as a plain Yes and only `scripts/coverage.sh --gen` is left to run.
- Every row has a sample query. A `true` query is a self-check (same result as the normal plan inside a transaction, with a second
  session writing, at several worker counts, a catalog or a setting); a few run on a second cluster (a hot standby, a logical publisher).

To see what a query does: `EXPLAIN (COSTS OFF) <query>`; a `Custom Scan (SiftAgg)` with `Pushed Aggregates`, `Group Keys` and `Lookup Joins`
lines is the column index at work, `SiftScan` / `SiftCount` is the BM25 index, anything else is the normal plan.

EOF
	awk -F'\t' -v P="$(pending_features)" "$STATUS_AWK"'
		/^#/ || $1 == "id" { next }
		{ n[$4]++; total++; if (pend($12)) npend++; if ($10 ~ /^external:/) ext++; row[++r] = $0 }
		END {
			printf "%d rows: %d Yes, %d Partly, %d No, %d Error; every row has a sample query (%d run on a second cluster); %d pending merge.\n\n", total, n["Yes"], n["Partly"], n["No"], n["Error"], ext, npend
			print "| Id | Area | Query / feature | Covered by sift? | Index or engine | Example | Notes |"
			print "|---|---|---|---|---|---|---|"
			for (i = 1; i <= r; i++) {
				split(row[i], f, "\t")
				notes = md(f[8]); if (f[9] != "") notes = notes (notes == "" ? "" : " ") "Proof: " proofs(f[9]) "."
				printf "| %s | %s | %s | %s | %s | %s | %s |\n", f[1], md(f[2]), md(f[3]), status(f[4], f[5], f[12]), md(f[6]), code(f[7]), notes
			}
		}' "$TSV"
}

# the coverage block of README.md: the counts and one row per README family (column 14) of docs/coverage.tsv
gen_block() {
	echo '<!-- coverage:start (generated by `scripts/coverage.sh --gen` from docs/coverage.tsv; do not edit by hand) -->'
	awk -F'\t' '
		/^#/ || $1 == "id" { next }
		{ n[$4]++; total++ }
		$14 != "" {
			k = $14 + 0; name[k] = substr($14, index($14, ":") + 1); max = k > max ? k : max
			if (!((k, $4) in seen)) { seen[k, $4] = 1; kinds[k]++; st[k] = $4 }
			if (!(k in eng)) { eng[k] = $6; sub(/[ ,+].*/, "", eng[k]) }
		}
		END {
			printf "[docs/COVERAGE.md](docs/COVERAGE.md) lists %d query shapes with a sample each (%d Yes, %d Partly, %d No, %d Error), checked on a test cluster by `scripts/coverage.sh --check`. The analytics shapes by family:\n\n", total, n["Yes"], n["Partly"], n["No"], n["Error"]
			print "| query shape | covered | engine |"
			print "|---|---|---|"
			for (k = 1; k <= max; k++) if (k in name) printf "| %s | %s | %s |\n", name[k], (kinds[k] > 1 ? "Partly" : st[k]), eng[k]
		}' "$TSV"
	echo; echo '<!-- coverage:end -->'
}

# README.md with its coverage block replaced by the generated one
readme_with_block() {
	local blk
	blk=$(mktemp "${TMPDIR:-/tmp}/sift-coverage-block.XXXXXX")
	gen_block >"$blk"
	awk 'FNR == NR { b = b $0 "\n"; next } /^<!-- coverage:start/ { printf "%s", b; skip = 1; next } /^<!-- coverage:end -->/ { skip = 0; next } !skip' "$blk" "$REPO/README.md"
	rm -f "$blk"
}

generate() { gen_full >"$1/COVERAGE.md"; readme_with_block >"$1/README.md"; }

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
		/^#/ || $1 == "id" || $10 ~ /^external:/ { next }
		rows != "" && $1 !~ rows { next }
		{
			pre = ""
			if ($11 ~ /^@/) src = fx "/" substr($11, 2) ".sql"
			else {
				q = $11; gsub(/\\n/, "\n", q); src = dir "/" $1 ".sql"; n = split(q, ln, "\n"); body = ""
				for (i = 1; i <= n; i++) if (ln[i] ~ /^-- pre: /) pre = pre substr(ln[i], 9) "\n"; else body = body ln[i] "\n"
				printf "%s", body > src; close(src)
			}
			printf "%s", pre
			print "RESET ALL;"
			print "\\set q `cat " src "`"
			print "SELECT coverage.check(" lit($1) ", " lit($10) ", :\047q\047, " ($13 == "" ? "NULL" : lit($13)) ");"
		}' "$TSV" >"$OUT/rows.sql.body"
	{ echo "\\set ON_ERROR_STOP 0"; echo "LOAD 'pg_sift';"; echo "\\o /dev/null"; echo "TRUNCATE coverage.results;"; cat "$OUT/rows.sql.body"; echo "\\o"; } >"$OUT/rows.sql"
}

# ---------- proofs on a second cluster (check external:NAME, the function ext_NAME) ----------
# side NAME ARGS: scripts/pgtest.sh on the cluster $SIFT_TMP/side-NAME (own data, socket and leak gate; the install and port of this run)
side() { local n=$1 root=${SIFT_ROOT:-$SIFT_TMP/root}; shift; SIFT_TMP=$SIFT_TMP/side-$n SIFT_ROOT=$root SIFT_PORT=${SIFT_PORT:-56100} "$REPO/scripts/pgtest.sh" "$@"; }
# side_conf NAME LINES...: the postgresql.conf of a fresh side cluster, small enough to run beside the others
side_conf() {
	local n=$1; shift
	{ echo "port = ${SIFT_PORT:-56100}"; echo "listen_addresses = ''"; echo "unix_socket_directories = '$SIFT_TMP/side-$n/sock'"; echo "shared_buffers = 32MB"; side "$n" conf; printf '%s\n' "$@"; } \
		>>"$SIFT_TMP/side-$n/data/postgresql.conf"
}

# L10: a published sift_table under wal_level = logical raises 0A000 on the first change without pg_sift in shared_preload_libraries
ext_logical_run() {
	local out
	side logical new-data "$SIFT_TMP/side-logical/data" || return 1
	side_conf logical "wal_level = logical" || return 1
	side logical start || return 1
	side logical psql -qAt -c "CREATE EXTENSION pg_sift" -c "CREATE TABLE cv_pub (id int PRIMARY KEY, v int) USING sift_table" -c "CREATE PUBLICATION cv_p FOR TABLE cv_pub" >/dev/null || return 1
	out=$(side logical psql -v VERBOSITY=sqlstate -qAt -c "INSERT INTO cv_pub VALUES (1, 1)" 2>&1 || true)
	[[ $out == *0A000* ]] || { echo "the first INSERT into a published sift_table raised: ${out:-nothing}"; return 1; }
	side logical stop || return 1
	echo "shared_preload_libraries = 'pg_sift'" >>"$SIFT_TMP/side-logical/data/postgresql.conf"
	side logical start || return 1
	side logical psql -qAt -c "INSERT INTO cv_pub VALUES (1, 1)" >/dev/null || { echo "the INSERT still fails with pg_sift in shared_preload_libraries"; return 1; }
}

# D07: a hot standby (pg_basebackup of this cluster, then WAL replay of later changes) serves the primary's result from a SiftAgg plan on a
# sift_col index and on a sift_table, and a BM25 match from a SiftScan / SiftCount plan
ext_standby_run() {
	local lsn q want got bm="SELECT count(*) FROM orders WHERE memo @@> 'clean'"
	local aggs=('SELECT grp, count(*), sum(v) FROM cv_dml GROUP BY grp' 'SELECT grp, count(*), sum(i4) FROM cv_tt GROUP BY grp')
	pg_basebackup -h "$SIFT_TMP/sock" -p "${SIFT_PORT:-56100}" -U postgres -D "$SIFT_TMP/side-standby/data" -X stream -c fast -R || return 1
	side_conf standby || return 1
	side standby start || return 1
	sql -c "INSERT INTO cv_dml SELECT 9000 + i, i % 5, i, i, 'r' FROM generate_series(1, 300) i" -c "INSERT INTO cv_tt SELECT * FROM cv_types WHERE id <= 60" || return 1
	lsn=$(sql -c "SELECT pg_current_wal_lsn()")
	for _ in $(seq 150); do
		[[ $(SIFT_DB=$DB side standby psql -qAt -c "SELECT pg_last_wal_replay_lsn() >= '$lsn'") == t ]] && break
		sleep 0.2
	done
	for q in "${aggs[@]}"; do
		want=$(sql -c "SELECT coverage.rows_text(\$\$$q\$\$)")
		got=$(SIFT_DB=$DB side standby psql -qAt -c "LOAD 'pg_sift'" -c "SELECT CASE WHEN pg_is_in_recovery() AND coverage.same(\$\$$q\$\$) THEN coverage.rows_text(\$\$$q\$\$) END" | grep -v '^LOAD$')
		[[ -n $got && $got == "$want" ]] || { echo "the standby returned [${got:-no SiftAgg plan or not in recovery}] for $q, the primary [$want]"; return 1; }
	done
	want=$(sql -c "$bm")
	got=$(SIFT_DB=$DB side standby psql -qAt -c "LOAD 'pg_sift'" -c "SELECT CASE WHEN coverage.plan_of(\$q\$$bm\$q\$) ~ 'Custom Scan \\(Sift(Scan|Count)\\)' THEN ($bm)::text END" | grep -v '^LOAD$')
	[[ -n $got && $got == "$want" ]] || { echo "the standby returned [${got:-no SiftScan plan}] for the BM25 match, the primary [$want]"; return 1; }
}

ext_record() {
	sql -v id="$1" -v ok="$2" -v detail="$3" <<<"SELECT coverage.record(:'id', 'external', :'ok'::boolean, CASE WHEN :'ok'::boolean THEN 'ok' ELSE 'plan' END, nullif(:'detail', ''), NULL)" >/dev/null
}

external_proofs() {
	local id check name msg ok
	while IFS=$'\t' read -r id check; do
		[[ -z $ROWS || $id =~ $ROWS ]] || continue
		name=ext_${check#external:}; name=${name//-/_}
		rm -rf "$SIFT_TMP/side-${check#external:}"; mkdir -p "$SIFT_TMP/side-${check#external:}"
		ok=true; msg=$("${name}_run" 2>&1) || ok=false
		side "${check#external:}" stop >/dev/null 2>&1 || true
		rm -rf "$SIFT_TMP/side-${check#external:}"
		ext_record "$id" "$ok" "$msg"
	done < <(awk -F'\t' '$10 ~ /^external:/ { print $1 "\t" $10 }' "$TSV")
}

# run: the verdict of every row; exit status 1 when a claim is wrong
run_proofs() {
	local fails pend
	write_script
	sql -f "$OUT/rows.sql" >/dev/null 2>"$OUT/rows.err" || true
	external_proofs
	pend=$(pending_features)
	sql -F $'\t' -c "SELECT id, ok, kind, coverage.nodes(plan), replace(coalesce(detail, ''), E'\n', ' ') FROM coverage.results ORDER BY id" >"$OUT/results.tsv"
	fails=$(awk -F'\t' -v P="$pend" -v verbose="$VERBOSE" -v rows="$ROWS" '
		FILENAME == ARGV[1] { if ($0 !~ /^#/ && $1 != "id" && (rows == "" || $1 ~ rows)) { claim[$1] = $10; since[$1] = $12; status[$1] = $4 } ; next }
		{ got[$1] = 1
		  ok = ($2 == "t"); kind = $3; nodes = $4; detail = $5
		  pending = (since[$1] != "" && index(P, " " since[$1] " "))
		  if (pending && ok) { verdict = "FAIL"; msg = "the claim holds in a tree without the feature " since[$1] ": fix its marker file in the #feature line" }
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
	# a psql error outside coverage.check (a failing `-- pre:` line) is a broken proof, not a pass
	! grep -q 'ERROR' "$OUT/rows.err" || { grep -E 'ERROR|LINE' "$OUT/rows.err" | head -10 >&2; fails=$((fails + 1)); }
	[[ $fails == 0 ]] || { grep -v -E 'NOTICE|skipping' "$OUT/rows.err" | head -20 >&2; return 1; }
}

cleanup() {
	[[ -n $KEEP ]] && { log "kept: database $DB, files in $OUT"; return; }
	SQLDB=postgres sql -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "DROP ROLE IF EXISTS sift_cov_user" >/dev/null 2>&1 || true
	rm -rf "$OUT"
}

# --check proves the proofs themselves: a table with wrong claims must fail on exactly those rows
selftest() {
	local mut=$OUT/mutated.tsv out n
	awk -F'\t' -v OFS='\t' '$1 == "A01" { $10 = "plain" } $1 == "T07" { $10 = "agg" } $1 == "T05" { $10 = "error:42601" } $1 == "A02" { $12 = "selftest" } $1 == "X03" { $11 = "SELECT false" } { print } END { print "#feature\tselftest\tnone\tno/such/marker" }' "$TSV" >"$mut"
	out=$(TSV=$mut ROWS='^(A01|T07|T05|A02|X03)$' run_proofs 2>&1 >/dev/null || true)
	n=$(grep -c '^FAIL' <<<"$out" || true)
	(( n == 5 )) || { echo "coverage: selftest: 5 wrong claims (a plan that is not there, a node that is, a SQLSTATE, a feature that is not there, a probe that is false) must fail, $n did:" >&2; echo "$out" >&2; return 1; }
	log "selftest: the 5 wrong claims failed as they should"
}

# ---------- main ----------
validate || { echo "coverage: docs/coverage.tsv is malformed" >&2; exit 1; }
if [[ -n $GEN ]]; then
	grep -q '^<!-- coverage:start' "$REPO/README.md" || { echo "coverage: README.md has no <!-- coverage:start --> ... <!-- coverage:end --> block" >&2; exit 1; }
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/sift-coverage.XXXXXX")
	generate "$tmp"
	cp "$tmp/COVERAGE.md" "$REPO/docs/COVERAGE.md"
	cp "$tmp/README.md" "$REPO/README.md"
	rm -rf "$tmp"
	log "wrote docs/COVERAGE.md and the coverage block of README.md ($(grep -c -v -E '^(#|id	)' "$TSV") rows)"
	exit 0
fi
status=0
if [[ -n $CHECK ]]; then
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/sift-coverage.XXXXXX")
	generate "$tmp"
	cmp -s "$tmp/COVERAGE.md" "$REPO/docs/COVERAGE.md" || { echo "coverage: docs/COVERAGE.md is not what docs/coverage.tsv generates: run scripts/coverage.sh --gen" >&2; status=1; }
	cmp -s "$tmp/README.md" "$REPO/README.md" || { echo "coverage: the coverage block of README.md is not what docs/coverage.tsv generates: run scripts/coverage.sh --gen" >&2; status=1; }
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
