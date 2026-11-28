#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The participants of a parallel CREATE INDEX, of a VACUUM (PARALLEL n) and of a parallel scan of a sift_table scale with their number: the build writes its parts without holding the extension
# lock while it formats them and commits them in one batch, the compaction of a VACUUM splits the granules of its inputs over the participants, and the units of a scan are shared as
# granules, so that no worker of eight is left with none. The times are the best of the runs; the gates are what the probes of bench/steps/scaling_io.py (A5, A6 and the parallel scan) reproduce on a shared host, where participants do not add up to their number.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init pbv 1 "shared_preload_libraries = 'pg_sift'
autovacuum = off
sift.async_maintenance = off
max_worker_processes = 24
max_parallel_workers = 16
max_parallel_maintenance_workers = 7
max_parallel_workers_per_gather = 8
maintenance_work_mem = 1GB
shared_buffers = 256MB
max_wal_size = 1GB
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start pbv || bail "start failed"
sql pbv "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

now_ms() { echo $((${EPOCHREALTIME/./} / 1000)); }
# ms NODE QUERY...: the milliseconds the statements take in one session
ms() { local t0; t0=$(now_ms); sql "$@" >/dev/null || return 1; echo $(($(now_ms) - t0)); }
# speedup BASE PARALLEL: how many hundredths of a time the other is
speedup() { echo $(($1 * 100 / ($2 > 0 ? $2 : 1))); }
rows() {
	echo "SELECT i, (i * 7919) % 2000, DATE '2024-01-01' + (i % 730)::int, 1 + (i * 131) % 50000, 'tag-' || (i * 97) % 50000, (i % 100)::smallint,
		((i * 104729) % 100000) / 100.0, ((i * 31) % 1000) / 10.0, jsonb_build_object('c', i % 4, 's', md5(i::text)) FROM generate_series(1::bigint, $1) i"
}
table_ddl="(id bigint PRIMARY KEY, entity_id int, event_date date, group_key int, tag text, status smallint, amount numeric(12,2), score float8, payload jsonb) WITH (autovacuum_enabled = off)"
index_ddl="USING sift_col (id, entity_id, event_date, group_key, tag, status, amount, score, payload) WITH (key_order = 'entity_id, event_date')"

# A6: a VACUUM of 250,000 rows, a fifth of them dead, with one participant and with four
v1=; v4=; verify=
for _ in 1 2; do
	for t in v1 v4; do
		sql pbv "CREATE TABLE $t $table_ddl" "INSERT INTO $t $(rows 250000)" "CREATE INDEX ${t}_idx ON $t $index_ddl" "DELETE FROM $t WHERE id % 5 = 0" >/dev/null || bail "A6 setup failed"
	done
	t1=$(ms pbv "VACUUM (PARALLEL 1) v1")
	t4=$(ms pbv "VACUUM (PARALLEL 4) v4")
	[[ -n $v1 && $v1 -le $t1 ]] || v1=$t1
	[[ -n $v4 && $v4 -le $t4 ]] || v4=$t4
	verify+=$(sql pbv "SELECT sift.col_verify('v1_idx', true)" "SELECT sift.col_verify('v4_idx', true)" 2>&1)
	sql pbv "DROP TABLE v1, v4" >/dev/null
done
out="VACUUM (PARALLEL 1): $v1 ms, (PARALLEL 4): $v4 ms"
diag "$out"
scales 4 "A6: VACUUM (PARALLEL 4) takes under 1/2.2 of VACUUM (PARALLEL 1)" test "$(speedup "$v1" "$v4")" -ge 220
check "A6: both indexes verify against the heap" test -z "$verify"

# A5: a CREATE INDEX of 500,000 rows with one participant (the leader) and with four (the leader and three workers)
sql pbv "CREATE TABLE b $table_ddl" "INSERT INTO b $(rows 500000)" "ALTER TABLE b SET (parallel_workers = 3)" "VACUUM (FREEZE, ANALYZE) b" >/dev/null || bail "A5 setup failed"
b1=; b4=; wal=
for round in 1 2; do
	t1=$(ms pbv "SET max_parallel_maintenance_workers = 0" "CREATE INDEX b1_idx ON b $index_ddl")
	sql pbv "DROP INDEX b1_idx" >/dev/null
	lsn0=$(sql pbv "SELECT pg_current_wal_lsn()")
	t4=$(ms pbv "SET max_parallel_maintenance_workers = 3" "CREATE INDEX b4_idx ON b $index_ddl")
	wal=$(sql pbv "SELECT (pg_current_wal_lsn() - '$lsn0'::pg_lsn)::bigint")
	[[ -n $b1 && $b1 -le $t1 ]] || b1=$t1
	[[ -n $b4 && $b4 -le $t4 ]] || b4=$t4
	[[ $round == 2 ]] || sql pbv "DROP INDEX b4_idx" >/dev/null
done
out="CREATE INDEX with 1 participant: $b1 ms, with 4: $b4 ms, WAL $wal bytes"
diag "$out"
scales 4 "A5: the build of 4 participants takes under 1/2.2 of the build of one" test "$(speedup "$b1" "$b4")" -ge 220
check "A5: the build writes at most 42 bytes of WAL for a row" test "$((wal / 500000))" -le 42
check "A5: the index verifies" test -z "$(sql pbv "SELECT sift.col_verify('b4_idx', true)" 2>&1)"
sql pbv "DROP TABLE b" >/dev/null

# the parallel scan of a sift_table of 500,000 rows (some 61 granules): eight workers, and the leader out of it
sql pbv "CREATE TABLE s (id bigint, a int, t text) USING sift_table" "INSERT INTO s SELECT i, (i % 1000)::int, 'row-' || (i % 5000) FROM generate_series(1::bigint, 500000) i" \
	"ALTER TABLE s SET (parallel_workers = 8)" "VACUUM (ANALYZE) s" >/dev/null || bail "scan setup failed"
scan="SET sift.enable_agg = off; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET parallel_leader_participation = off"
probe="SELECT sum(length(md5(md5(t) || id::text))) FROM s"
best() { local m= t; for _ in 1 2 3; do t=$(ms pbv "$scan" "SET max_parallel_workers_per_gather = $1" "$probe") || return 1; [[ -n $m && $m -le $t ]] || m=$t; done; echo "$m"; }
s1=$(best 1)
s8=$(best 8)
out=$(sql pbv "$scan" "SET max_parallel_workers_per_gather = 8" "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF) $probe")
granule_rows=8192
busy=$(awk '/Parallel Seq Scan/ { f = 1 } f && /Worker [0-9]+:/ { match($0, /rows=[0-9]+/); if (substr($0, RSTART + 5, RLENGTH - 5) + 0 >= 4 * g) n++ } END { print n + 0 }' g="$granule_rows" <<<"$out")
diag "parallel scan with 1 worker: $s1 ms, with 8: $s8 ms; workers that scanned 4 granules or more: $busy"
check "the parallel scan gives each of eight workers at least four granules" test "$busy" -eq 8
scales 8 "the parallel scan of eight workers takes under 1/5 of the scan of one" test "$(speedup "$s1" "$s8")" -ge 500
check "eight workers and one add up the same rows" test "$(sql pbv "$scan" "SET max_parallel_workers_per_gather = 1" "$probe")" == "$(sql pbv "$scan" "SET max_parallel_workers_per_gather = 8" "$probe")"

finish
