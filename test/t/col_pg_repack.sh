#!/usr/bin/env bash
# pg_repack of a heap table with a sift_col and a sift (BM25) index, with writers running: the copy is built with the index definitions, the log of the
# writers is applied to it, the swap changes every ctid, and the rebuilt indexes answer as the heap does. The sift_table that pg_repack would destroy is refused.
# Skipped where the pg_repack binary and extension are not installed (postgresql-NN-repack).
. "$(dirname "$0")/lib.bash"

plan 13
command -v pg_repack >/dev/null || { skip 13 "pg_repack is not installed"; finish; }
node_init rp 28 "autovacuum = off"
node_start rp || bail "start failed"
sql rp "CREATE EXTENSION pg_sift CASCADE" >/dev/null || bail "pg_sift"
sql rp "CREATE EXTENSION pg_repack" >/dev/null 2>&1 || { skip 13 "the pg_repack extension is not available"; finish; }
sql rp "CREATE TABLE facts (id int PRIMARY KEY, grp int, kind text, amount numeric(10,2), title text, doc jsonb, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, (g * 7919) % 200, 'cat_' || (g % 8), g % 997, 'alpha beta ' || (g % 50), jsonb_build_object('score', g % 11), repeat('x', 100 + g % 400) FROM generate_series(1, 40000) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, grp, kind, amount, doc) WITH (paths = 'doc.score float8')" \
	"CREATE INDEX facts_bm ON facts USING sift (title)" "VACUUM facts" \
	"CREATE TABLE st (id int PRIMARY KEY, v int) USING sift_table" "INSERT INTO st SELECT g, g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
D=$(dir rp)
repack() { pg_repack -h "$D/sock" -p "$(port rp)" -U postgres -d postgres "$@"; }
# the answers of the column scan (the normal plan priced out) and of the heap, as one string
SCAN="SELECT kind, grp % 5 g, count(*), sum(amount), sum((doc->>'score')::float8) FROM facts GROUP BY 1, 2"
answers() {
	sql rp "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 1000" "SET sift.enable_agg = $1" \
		"SELECT string_agg(x::text, ',' ORDER BY x::text) FROM ($SCAN) x" | tail -1
}
defs() { sql rp "SELECT string_agg(regexp_replace(pg_get_indexdef(indexrelid), 'index_[0-9]+', 'index'), E'\n' ORDER BY indexrelid::regclass::text) FROM pg_index WHERE indrelid = 'facts'::regclass AND indexrelid::regclass::text IN ('facts_col', 'facts_bm')"; }
points() { sql rp "SELECT round(point_units) FROM sift.col_clustering('facts_col') WHERE path = 'grp'"; }

before_defs=$(defs)
before_points=$(points)
writer() {
	for i in $(seq 1 25); do
		sql rp "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $i" "DELETE FROM facts WHERE id % 100 = $i + 50 AND id < 40000" \
			"INSERT INTO facts SELECT 100000 + $i * 1000 + g, g % 200, 'cat_' || (g % 8), g % 997, 'alpha beta ' || (g % 50), jsonb_build_object('score', g % 11), 'y' FROM generate_series(1, 200) g" \
			>/dev/null 2>>"$D/writer.err" || echo failed >>"$D/writer.err"
	done
}
: >"$D/writer.err"
writer & wp=$!
repack -t facts -o "grp, id" >"$D/repack.log" 2>&1
check "pg_repack -o grp, id finished beside the writer" test "$?" == 0 -a -z "$(grep -E 'ERROR|WARNING' "$D/repack.log")"
wait "$wp"
check "the writer's transactions all committed" test ! -s "$D/writer.err"
check "the rebuilt sift_col and sift indexes have the definitions of the old ones" test "$(defs)" == "$before_defs"
check "the sift_col index verifies against the heap" test -z "$(sql rp "SELECT sift.col_verify('facts_col', true)" 2>&1)"
check "the BM25 index verifies against the heap" test -z "$(sql rp "SELECT sift.verify('facts_bm', true)" 2>&1)"
check "the column scan is the plan" test "$(sql rp "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 1000" "EXPLAIN (COSTS OFF) $SCAN" | grep -c SiftAgg)" == 1
check "the column scan answers as the heap does" test "$(answers on)" == "$(answers off)"
check "the BM25 index finds the rows the heap has" test "$(sql rp "SELECT count(*) FROM facts WHERE title @@> 'alpha beta 7'")" == "$(sql rp "SELECT count(*) FROM facts WHERE title LIKE 'alpha beta 7'")"
check "the new order tightens the zones of grp (units an equality touches: $before_points before)" test "$(points)" -lt "$before_points"
check "the rewrite left the visibility map empty (the doc tells to VACUUM)" test "$(sql rp "SELECT relallvisible FROM pg_class WHERE oid = 'facts'::regclass")" == 0
sql rp "VACUUM (ANALYZE) facts" >/dev/null
check "after VACUUM the visibility map is set again" test "$(sql rp "SELECT relallvisible = relpages AND relpages > 0 FROM pg_class WHERE oid = 'facts'::regclass")" == t
check "pg_repack refuses the sift_table and leaves it readable" test "$(repack -t st 2>&1 | grep -c 'cannot rewrite the sift_table')|$(sql rp "SELECT count(*), sum(v) FROM st")" == "1|1000|500500"
check "pg_repack --only-indexes of the sift_table is fine" test "$(repack -t st --only-indexes >/dev/null 2>&1; echo $?)|$(sql rp "SELECT count(*) FROM st")" == "0|1000"
finish
