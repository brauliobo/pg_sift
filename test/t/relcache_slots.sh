#!/usr/bin/env bash
# PostgreSQL gives a backend 10 relcache callback slots (MAX_RELCACHE_CALLBACKS) and every library loaded in it shares them; running out is a FATAL
# that ends the session. sift must leave the others room: none of its paths takes a slot. A session first takes N slots (sift._test_relcache_burn),
# then runs the statements of every sift path; the largest N it survives, against the one of a session that runs the same statements without sift,
# is the number of slots sift took. Each attempt is a fresh psql, a fresh backend.
. "$(dirname "$0")/lib.bash"

plan 3
node_init rs 31 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start rs || bail "start failed"
sql rs "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"
# the same three tables twice: with sift (sift_table, sift_col indexes) and plain heaps (suffix _h), which a session without sift reads
for sfx in "" _h; do
	using=; col=
	[[ -n $sfx ]] || { using="USING sift_table"; col=1; }
	sql rs "CREATE TABLE tt$sfx (id int, v int) $using" \
		"INSERT INTO tt$sfx SELECT i, i % 7 FROM generate_series(1, 300) i" \
		"CREATE TABLE pt$sfx (id int, d date NOT NULL, v int) PARTITION BY LIST (v)" \
		"CREATE TABLE pt${sfx}_a PARTITION OF pt$sfx FOR VALUES IN (0, 1, 2, 3, 4, 5, 6, 7, 8, 9)" \
		"CREATE TABLE pt${sfx}_b PARTITION OF pt$sfx DEFAULT" \
		"INSERT INTO pt$sfx SELECT i, DATE '2024-01-01' + i * 7 % 360, i % 100 FROM generate_series(1, 3000) i" \
		"CREATE TABLE facts$sfx (id int, day date, dim_a_id int, items jsonb)" \
		"INSERT INTO facts$sfx SELECT i, DATE '2026-01-01' + i % 15, 1 + i % 20, jsonb_build_array(jsonb_build_object('qty', i % 5)) FROM generate_series(1, 3000) i" \
		${col:+"CREATE INDEX pt_col ON pt USING sift_col (id, d, v)"} \
		${col:+"CREATE INDEX facts_col ON facts USING sift_col (id, day, dim_a_id, items) WITH (paths = 'items[]{qty float8}')"} \
		"VACUUM (FREEZE, ANALYZE) pt$sfx" "VACUUM (FREEZE, ANALYZE) facts$sfx" >/dev/null || bail "setup of the tables$sfx failed"
done

# the statements of the paths: a sift_table scan, a partitioned table read in one piece (bounds and facts of the partitions), a shared CTE (the verdict cache)
paths() {
	cat <<SQL
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SELECT count(*), sum(v) FROM tt$1;
SELECT count(*), sum(v) FROM pt$1 WHERE v = 3;
SET plan_cache_mode = force_generic_plan;
PREPARE g(int) AS SELECT count(*), sum(v) FROM pt$1 WHERE v = \$1;
EXECUTE g(3);
WITH s AS (SELECT id, day, i.qty FROM facts$1 CROSS JOIN LATERAL jsonb_to_recordset(items) AS i(qty float8))
 SELECT 'day', day::text, count(DISTINCT id), sum(qty) FROM s GROUP BY day UNION ALL SELECT 'all', NULL, count(DISTINCT id), sum(qty) FROM s;
SQL
}
with_sift=$(paths "")
without_sift=$(paths _h)

# survives STATEMENTS N: the session takes N slots, then runs the statements
survives() { echo "SELECT sift._test_relcache_burn($2); $1" | sql rs >/dev/null 2>&1; }
# room STATEMENTS: the most slots a session can take and still run the statements
room() {
	local n
	for n in $(seq 10 -1 0); do survives "$1" "$n" && { echo "$n"; return; }; done
	echo -1
}

base=$(room "$without_sift")
used=$(room "$with_sift")
diag "slots free to a session without sift: $base, with sift's paths run: $used"
check "the baseline session runs" test "$base" -ge 0
check "all the paths of sift run in a session that leaves them room" test "$used" -ge 0
check "no path of sift takes a relcache callback slot" test $((base - used)) -le 0

finish
