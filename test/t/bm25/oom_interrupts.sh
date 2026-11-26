#!/usr/bin/env bash
# tap: pooled
# The memory of a row is let go with the row, a long loop over the parts of an index can be cancelled, and a scan asks for the maintenance of the partitions it reads only:
# (1) sift.verify(heapallindexed) and sift.promote of 10,000 toasted 4 kB docs hold no copy of each doc until they end (the detoasted text of a row was kept in the context of the
# whole pass, 40 MB here); (2) a cancel that comes while CREATE INDEX of a wide sift_col index writes its parts ends it within 500 ms (the loop over the granules of a part had no
# interrupt check: a second and more), and a statement_timeout of 50 ms ends a scan of every granule and sift.verify(heapallindexed); (3) a one-row lookup in an index of 256
# partitions that are all due for compaction asks for the compaction of the partition it reads, not of all 256 (each request was also looked for in a list of all of them).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 9
node_init bmo 3 "autovacuum = off
sift.test_create_format1 = off
sift.async_maintenance = on
max_parallel_workers_per_gather = 0
max_parallel_maintenance_workers = 0"
node_start bmo || bail "start failed"
sql bmo "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

sql bmo "CREATE TABLE d (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off)" "ALTER TABLE d ALTER body SET STORAGE EXTERNAL" \
	"CREATE INDEX d_i ON d USING sift (body)" \
	"INSERT INTO d SELECT i, repeat(md5(i::text) || ' ' || md5((i % 97)::text) || ' ', 62) FROM generate_series(1, 10000) i" >/dev/null || bail "docs failed"
check "the docs are toasted" test "$(sql bmo "SELECT pg_relation_size(reltoastrelid) > 40000000 FROM pg_class WHERE relname = 'd'")" == t

# peak STMT: the most anonymous memory the backend held while it ran STMT, in kB
peak() {
	sql bmo <<SQL | tail -n 1
SELECT sift._test_thp_disable();
SELECT count(*) FROM (SELECT sift._test_malloc_stats(true)) s;
SELECT sift._test_rss_peak_start() AS base \\gset
$1;
SELECT sift._test_rss_peak_stop() - :base;
SQL
}
kb=$(peak "SELECT sift.verify('d_i', true)")
out=$kb check "sift.verify(heapallindexed) of 40 MB of docs holds under 12 MB ($kb kB)" test "$kb" -lt 12288
kb=$(peak "SELECT sift.promote('d_i')")
out=$kb check "sift.promote of 40 MB of docs holds under 12 MB ($kb kB)" test "$kb" -lt 12288
sql bmo "SELECT sift.verify('d_i', true)" >/dev/null || bail "verify failed"
check "the promoted index finds the docs of a term" test "$(sql bmo "SELECT count(*) FROM d WHERE body @@> md5('7')")" == 104

COLS=$(for k in $(seq 1 16); do printf ', md5((i * %d)::text) c%d' "$k" "$k"; done)
WIDE="ON w USING sift_col (id$(for k in $(seq 1 16); do printf ', c%d' "$k"; done))"
sql bmo "CREATE TABLE w WITH (autovacuum_enabled = off) AS SELECT i AS id $COLS FROM generate_series(1, 100000) i" "VACUUM (FREEZE) w" >/dev/null || bail "wide table failed"
(sql bmo "SET application_name = 'wide_build'" "CREATE INDEX w_i $WIDE" >"$(dir bmo)/ci.out" 2>&1) &
wait_for "[[ \$(sql bmo \"SELECT count(*) FROM pg_stat_progress_create_index WHERE phase LIKE '%writing parts'\") == 1 ]]" 60 || bail "the build never wrote parts"
start=$(date +%s%N)
sql bmo "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name = 'wide_build'" >/dev/null
wait
ms=$(( ($(date +%s%N) - start) / 1000000 ))
out=$(cat "$(dir bmo)/ci.out")
check "a cancel ends CREATE INDEX while it writes parts within 500 ms ($ms ms)" test "$(grep -c 'canceling statement due to user request' <<<"$out") $((ms < 500))" == "1 1"
sql bmo "CREATE INDEX w_i $WIDE" "VACUUM (FREEZE) w" >/dev/null 2>&1 || bail "wide index failed"

# timeout NAME STMT: STMT under a statement_timeout of 50 ms is cancelled within 500 ms
timeout_in() {
	local start ms
	start=$(date +%s%N)
	out=$(sql bmo "SET statement_timeout = 50" "$2" 2>&1)  # timeout-ok: the timeout is the interrupt the loops must honour
	ms=$(( ($(date +%s%N) - start) / 1000000 ))
	check "$1 is cancelled by a timeout of 50 ms within 500 ms ($ms ms)" test "$(grep -c 'canceling statement due to statement timeout' <<<"$out") $((ms < 500))" == "1 1"
}
timeout_in "a scan of every granule" "SELECT sum(0$(for k in $(seq 1 16); do printf ' + length(c%d)' "$k"; done)) FROM w"
timeout_in "sift.verify(heapallindexed) of 10,000 toasted docs" "SELECT sift.verify('d_i', true)"

sql bmo "CREATE TABLE p (k int8, v int8) WITH (autovacuum_enabled = off)" "CREATE INDEX p_i ON p USING sift_col (k, v) WITH (key_order = 'k')" \
	"SELECT sift._test_f2_split_many('p_i', 1, NULL, 255, 100)" \
	"SELECT count(sift._test_f2_add_part('p_i', part_id, key_lo, 10, coalesce(key_lo, 0), 1000 * g + coalesce(key_lo, 0) * 10, 0, 0))
	   FROM sift._test_f2_partitions('p_i'), generate_series(1, 6) g" >/dev/null || bail "partitions failed"
out=$(sql bmo "SELECT count(*), min(l0_parts) FROM sift._test_f2_partitions('p_i')")
check "256 partitions of 6 level-0 parts each" test "$out" == "256|6"
# the requests a transaction has asked for are kept in their context until it commits: one lookup asks for one partition, a few hundred bytes and the set of the requests
out=$(sql bmo "SET enable_seqscan = off" "BEGIN" "SELECT count(*) FROM p WHERE k = 777" \
	"SELECT sum(c.total_bytes - c.free_bytes) FROM pg_backend_memory_contexts c, pg_backend_memory_contexts r
	   WHERE r.name = 'sift cache maintenance requests' AND c.path[r.level] = r.path[r.level]" "COMMIT" | sed -n 2p)
check "a one-row lookup asks for the partition it reads, not all 256 ($out bytes of requests)" test "$out" -lt 8192

finish
