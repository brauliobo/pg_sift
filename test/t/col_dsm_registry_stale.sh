#!/usr/bin/env bash
# The shared tables of the server (col/dsmreg.c) live in the dynamic shared memory registry, which keeps the name and size of the first request until the
# postmaster restarts. A package upgrade without a restart, or a setting that changes the size asked, makes a backend ask a name at another size than the entry
# holds: PostgreSQL then raises "requested DSM segment size does not match size of existing segment", and the paths that join without a subtransaction
# (the rows of a joined table, the dimension maps, the write-buffer arena, the rollup state) passed it to the client. The registry name carries the size
# asked, so the same name is never asked at two sizes; entries of an older release (their own names, other sizes) stay unused.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 8
node_init ds 77
node_start ds || bail "start failed"
# the entries an older release left: its names, at sizes that are not the ones of this build
LEGACY="sift_agg_lookup sift_agg_dimmap sift_agg_verdicts_5 sift_col_dict_5 sift_col_dict_ring_5 sift_col_leaf_stats_5 sift_col_wbuf_reads_1 sift_col_wbuf_arena_4 sift_col_wbuf_arena_4_seg0 pg_sift_col_rollup"
sql ds "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

out=$(sql ds "SELECT sift._test_dsm_join('stale_probe', 4096)" "SELECT sift._test_dsm_join('stale_probe', 8192)" 2>&1)
check "one name asked at two sizes does not fail" test $? == 0
out=$(sql ds "SELECT sift._test_dsm_join('stale_probe', 4096)" "SELECT sift._test_dsm_join('stale_probe', 8192)" 2>&1)
check "and neither does a later backend" test $? == 0

REG=$(for n in $LEGACY; do echo "SELECT sift._test_dsm_register('$n', 4096)"; done)
mapfile -t REGQ <<<"$REG"
sql ds "${REGQ[@]}" >/dev/null
check "the registry holds the entries of an older release" test $? == 0

sql ds <<'SQL' >/dev/null || bail "setup failed"
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 4000, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);
CREATE TABLE dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3 FROM generate_series(1, 4000) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b);
CREATE TABLE ents (id int, label text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, 'e' || i % 50 FROM generate_series(1, 4000) i;
VACUUM (FREEZE, ANALYZE) ev, dim, ents;
SQL

# a new backend for each statement; the same statement with the engine on and off must give the same rows
same() {
	local q=$1 on off
	on=$(sql ds "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "SET sift.agg_force = on" "$q" 2>&1) || { out=$on; return 1; }
	off=$(sql ds "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = off" "$q") || { out=$off; return 1; }
	out=$on
	[[ -n $on && $on == "$off" ]]
}
check "the rows of a joined table are read after the entries of an older release" same "SELECT e.label, count(*), sum(v) FROM ev JOIN ents e ON e.id = ev.k GROUP BY 1 ORDER BY 1"
check "so are the dimension maps" same "SELECT dim.a, dim.b, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2"
out=$(sql ds "SET sift.col_wbuf_ref = on" "INSERT INTO ev SELECT i, 1 + i % 4000, i % 11 FROM generate_series(40001, 41000) i" "SELECT count(*) FROM ev" 2>&1)
check "a write that keeps its rows in the arena counts them" test "$out" == 41000
out=$(sql ds "SET sift.col_leaf_cache_kb = 2048" "SET sift.enable_agg = on" "SELECT count(*) FROM ev WHERE id <= 40000 AND k < 100" 2>&1)
check "a size of the leaf statistics that no table has yet is a table of its own" test "$out" == 990
check "no backend raised the error, and no table went unshared" bash -c '! grep -E "does not match size|are not shared with other backends" "$1"' _ "$(dir ds)/postgres.log"
finish
