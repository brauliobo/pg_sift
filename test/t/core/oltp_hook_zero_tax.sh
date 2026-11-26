#!/usr/bin/env bash
# A plain statement costs the hooks of the library next to nothing, in a database that has no extension and in one that has it beside plain tables: a 6-table B-tree join, a single-row SELECT,
# an INSERT and an ALTER TABLE on plain heap tables with 5 B-tree indexes each ask no format about an index of another access method (the probes of the testing library count them), read no more of
# pg_depend than a server that has not loaded the library, and plan as fast as it does (the same database on both servers: the extension itself adds operator families that every plan of the
# database sees). The statements of the library still plan as its own.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

TABLES=6
ROUNDS=$(knob SIFT_HOOK_ROUNDS 4 3)
PLAN_RUNS=$(knob SIFT_HOOK_PLAN_RUNS 150 100)
LOOKUP_RUNS=$(knob SIFT_HOOK_LOOKUP_RUNS 3000 2000)
plan 10

node_init oz 4 "shared_preload_libraries = 'pg_sift'
autovacuum = off"
node_init un 5 "autovacuum = off"
node_start oz || bail "start failed"
node_start un || bail "start failed"

db() { local node=$1 d=$2; shift 2; psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$node")/sock" -p "$(port "$node")" -U postgres -d "$d" "${@/#/--command=}"; }

schema() {
	local node=$1 d=$2 i
	for i in $(seq $TABLES); do
		db "$node" "$d" "CREATE TABLE t$i (id int PRIMARY KEY, a int, b int, c int, d int)" \
			"CREATE INDEX ON t$i (a)" "CREATE INDEX ON t$i (b)" "CREATE INDEX ON t$i (c)" "CREATE INDEX ON t$i (d)" \
			"INSERT INTO t$i SELECT g, g, g, g, g FROM generate_series(1, 200) g" "ANALYZE t$i" >/dev/null || bail "schema of $node/$d failed"
	done
}

JOIN="SELECT count(*) FROM t1 JOIN t2 USING (id) JOIN t3 USING (id) JOIN t4 USING (id) JOIN t5 USING (id) JOIN t6 USING (id) WHERE t1.a < 50"
LOOKUP="SELECT * FROM t1 WHERE id = 7"
FILL_PROBE="CREATE FUNCTION probes(bool) RETURNS int8 AS 'pg_sift', 'sift_test_foreign_probes' LANGUAGE C STRICT"

for d in postgres mixed; do
	[[ $d == postgres ]] || db oz postgres "CREATE DATABASE mixed" >/dev/null
	schema oz $d
	db oz $d "$FILL_PROBE" >/dev/null || bail "probe function failed"
done
schema un postgres
db un postgres "CREATE DATABASE mixed" >/dev/null
schema un mixed
db oz mixed "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
db un mixed "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# the probes of the statements a plain application runs, in one backend
probes() {
	db oz "$1" "SELECT probes(true)" "EXPLAIN (COSTS off) $JOIN" "EXPLAIN (COSTS off) $LOOKUP" "$LOOKUP" "INSERT INTO t1 VALUES (1000, 1, 1, 1, 1)" \
		"DELETE FROM t1 WHERE id = 1000" "SELECT probes(false)" | tail -1
}

# the rows of pg_depend an ALTER TABLE reads, from the statistics of the server
depend_scans() {
	local node=$1 d=$2 i before after
	db "$node" "$d" "SELECT pg_stat_force_next_flush()" >/dev/null
	before=$(db "$node" "$d" "SELECT sum(pg_stat_get_numscans(oid)) FROM pg_class WHERE relname IN ('pg_depend', 'pg_depend_depender_index', 'pg_depend_reference_index')")
	for i in $(seq 20); do db "$node" "$d" "ALTER TABLE t2 SET (fillfactor = $((90 - i)))" >/dev/null; done
	db "$node" "$d" "SELECT pg_stat_force_next_flush()" >/dev/null
	after=$(db "$node" "$d" "SELECT sum(pg_stat_get_numscans(oid)) FROM pg_class WHERE relname IN ('pg_depend', 'pg_depend_depender_index', 'pg_depend_reference_index')")
	echo $((after - before))
}

# the CPU time of the backend for one statement in us, over RUNS statements of one session (a scheduler that is busy with other work does not count)
latency() {
	local node=$1 d=$2 file=$3 runs=$4 start=$(dir "$1")/cpu.start end=$(dir "$1")/cpu.end
	{
		echo "SELECT pg_backend_pid() AS pid \\gset"
		echo "\\setenv BACKEND_PID :pid"
		echo "\\o /dev/null"
		echo "\\! cut -d' ' -f1 /proc/\$BACKEND_PID/schedstat >$start"
		yes "$(<"$file");" | head -n "$runs"
		echo "\\! cut -d' ' -f1 /proc/\$BACKEND_PID/schedstat >$end"
	} | psql -X -q -h "$(dir "$node")/sock" -p "$(port "$node")" -U postgres -d "$d" >/dev/null 2>&1
	awk -v runs="$runs" -v s="$(<"$start")" -v e="$(<"$end")" 'BEGIN { printf "%.1f\n", (e - s) / runs / 1000 }'
}
least() { awk -v a="$1" -v b="$2" 'BEGIN { print (a == "" || b < a) ? b : a }'; }
# the best of ROUNDS runs on the server that has not loaded the library (un) and on the one that has (oz), taken in turns
latencies() {
	local d=$1 file=$2 runs=$3 rounds=$4 r
	LAT_UN= LAT_OZ=
	for r in $(seq "$rounds"); do
		LAT_UN=$(least "$LAT_UN" "$(latency un "$d" "$file" "$runs")")
		LAT_OZ=$(least "$LAT_OZ" "$(latency oz "$d" "$file" "$runs")")
	done
}
within() { awk -v a="$1" -v b="$2" -v pct="$3" 'BEGIN { exit !(a <= b * (1 + pct / 100)) }'; }

SCRIPTS=$(dir oz)/lat
mkdir -p "$SCRIPTS"
echo "EXPLAIN (COSTS off) $JOIN" >"$SCRIPTS/join.sql"
echo "$LOOKUP" >"$SCRIPTS/lookup.sql"

out=$(probes postgres)
check "a database without the extension: no relation of another access method is opened by a hook ($out)" test "$out" -eq 0
out=$(probes mixed)
check "a database with the extension and plain tables: none either ($out)" test "$out" -eq 0

un_scans=$(depend_scans un postgres)
out=$(depend_scans oz postgres)
check "an ALTER TABLE of a plain table reads no more of pg_depend than without the library ($out of $un_scans scans)" test "$out" -eq "$un_scans"
out=$(depend_scans oz mixed)
check "the same in the database with the extension ($out of $un_scans scans)" test "$out" -eq "$un_scans"

if [[ $(sql un "SHOW debug_assertions") == on ]]; then
	skip 4 "an assert build checks every allocation: its CPU times say nothing of the hooks"
else
	for q in join:$PLAN_RUNS:5:$ROUNDS lookup:$LOOKUP_RUNS:10:$((ROUNDS * 2 - 1)); do
		IFS=: read -r name runs pct rounds <<<"$q"
		for d in postgres mixed; do
			latencies $d "$SCRIPTS/$name.sql" "$runs" "$rounds"
			diag "cpu per statement, $name, $d: $LAT_OZ us with the library, $LAT_UN us without"
			check "the $name statement takes within $pct% of the CPU time of a server that has not loaded the library ($d: $LAT_OZ us, $LAT_UN us)" within "$LAT_OZ" "$LAT_UN" "$pct"
		done
	done
fi

db oz mixed "CREATE TABLE docs (id int, body text)" "INSERT INTO docs SELECT g, 'word' || g % 50 || ' text' FROM generate_series(1, 2000) g" \
	"CREATE INDEX docs_i ON docs USING sift (body)" "CREATE TABLE facts (id int, k int, v int)" \
	"INSERT INTO facts SELECT g, g % 7, g FROM generate_series(1, 5000) g" "CREATE INDEX facts_i ON facts USING sift_col (k, v)" "VACUUM ANALYZE docs, facts" >/dev/null || bail "library setup failed"
out=$(db oz mixed "SET enable_seqscan = off" "EXPLAIN (COSTS off) SELECT id FROM docs WHERE body @@> 'word7'")
check "a sift index still serves its query in the database with plain tables" grep -q 'Sift' <<<"$out"
out=$(db oz mixed "SET sift.enable_agg = on" "SET sift.agg_force = on" "EXPLAIN (COSTS off) SELECT k, count(*), sum(v) FROM facts GROUP BY k")
check "a sift_col index still serves its aggregate" grep -q 'Sift' <<<"$out"
finish
