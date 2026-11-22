#!/usr/bin/env bash
# The open units of a CREATE INDEX of a key_cut index with no room for the temporary files of the key ranges (col/v1/build/open.c: every range of the key is an open unit, written as units as the table is read,
# with no file) on a physical hot standby and under writers: the index verifies and answers as the heap on the standby; a CREATE INDEX CONCURRENTLY under inserts, updates and deletes gives an index that
# matches the heap; a build that is cancelled, terminated or whose process dies leaves no temporary file and no index that reads wrong.
. "$(dirname "$0")/lib.bash"
plan 10

ROWS=$(knob SIFT_BOS_ROWS 300000 150000)
node_init primary 4 "sift.col_build_source = auto
wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
shared_buffers = 64MB
temp_file_limit = -1
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE ev (id int PRIMARY KEY, g int, k int, note text, pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO ev SELECT i, i % 7, CASE WHEN h % 100 < 60 THEN h % 4 ELSE 4 + (h / 100) % 3000 END, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text)
		FROM (SELECT i, (hashint4(i)::bigint & 2147483647) AS h FROM generate_series(1, $ROWS) i) s" \
	"VACUUM (FREEZE, ANALYZE) ev" >/dev/null || bail "setup failed"
node_clone primary standby 5 "hot_standby_feedback = on"
node_start standby || bail "standby start failed"

SETTINGS=("SET sift.async_maintenance = off" "SET max_parallel_workers_per_gather = 0" "SET max_parallel_maintenance_workers = 0" "SET sift.test_col_segment_kb = 4000" "SET sift.test_col_unit_rows = 500")
Q="SELECT coalesce(string_agg(k || ':' || n || ':' || s, ',' ORDER BY k), '') FROM (SELECT k, count(*) AS n, sum(id) AS s FROM ev WHERE k < 40 GROUP BY k) x"
TEMP="SELECT 'temp=' || temp_bytes FROM pg_stat_database WHERE datname = current_database()"
FLUSH="SELECT pg_stat_force_next_flush()"; CLEAR="SELECT pg_stat_clear_snapshot()"
answer() { sql "$1" "SET sift.enable_agg = $2" "SET max_parallel_workers_per_gather = 0" "$Q" | tail -1; }
verified() { test -z "$(sql "$1" "SELECT sift.col_verify('ev_idx', true)" 2>&1)"; }
same_everywhere() {
	catchup primary standby || return 1
	local heap
	heap=$(answer primary off)
	[[ -n $heap && $(answer primary on) == "$heap" && $(answer standby on) == "$heap" && $(answer standby off) == "$heap" ]] && verified primary && verified standby
}
# build STATEMENT...: the statements in one session; TEMPDELTA = the temp_bytes it added to the database, STATS = what the open units did
build() {
	local out
	out=$(sql primary "${SETTINGS[@]}" "$FLUSH" "$CLEAR" "$TEMP" "$@" "$FLUSH" "$CLEAR" "$TEMP" "SELECT 'stats=' || current_setting('sift.test_col_open_stats')" | grep -E '^(temp|stats)=')
	STATS=$(grep '^stats=' <<<"$out" | cut -c7-)
	TEMPDELTA=$(($(grep '^temp=' <<<"$out" | tail -n 1 | cut -c6-) - $(grep '^temp=' <<<"$out" | head -n 1 | cut -c6-)))
}
CREATE="CREATE INDEX ev_idx ON ev USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k', point_tid_columns = 'off', order_array_columns = 'off')"

build "SET sift.col_build_source = heap" "$CREATE"
HEAPTEMP=$TEMPDELTA
out="temp bytes $TEMPDELTA, stats [$STATS]"
check "the key ranges' files take the rows with sift.col_build_source = heap" test "$HEAPTEMP" -gt 0 -a -z "$STATS"
sql primary "DROP INDEX ev_idx" >/dev/null

build "SET temp_file_limit = '64kB'" "$CREATE"
out="temp bytes $TEMPDELTA, stats [$STATS]"
check "with no room for the files every range is an open unit and none is written" test "$TEMPDELTA" = 0 -a -n "$STATS"
check "... the index verifies on the standby and answers as the primary and the heap" same_everywhere
sql primary "DROP INDEX ev_idx" >/dev/null

# writers: a CREATE INDEX CONCURRENTLY while rows are inserted, updated and deleted
sql primary "ALTER TABLE ev ADD COLUMN touched int DEFAULT 0" >/dev/null
d=$(dir primary)
printf '\\set i random(1, %s)\nUPDATE ev SET touched = touched + 1, note = note || %s WHERE id = :i;\n' "$ROWS" "'x'" >"$d/upd.sql"
printf '\\set i random(%s, %s)\nINSERT INTO ev (id, g, k, note, pad) VALUES (:i + 1000000, :i %% 7, :i %% 4, %s, %s) ON CONFLICT DO NOTHING;\n' "$ROWS" "$((ROWS * 2))" "'new'" "'p'" >"$d/ins.sql"
printf '\\set i random(1, %s)\nDELETE FROM ev WHERE id = :i AND id %% 13 = 0;\n' "$ROWS" >"$d/del.sql"
pgbench_bg primary 4 600 -f "$d/upd.sql@6" -f "$d/ins.sql@3" -f "$d/del.sql@1"
PGOPTIONS="-c maintenance_work_mem=64MB -c sift.test_col_segment_kb=4000 -c sift.test_col_unit_rows=500 -c temp_file_limit=64kB" psql -X -q -h "$d/sock" -p "$(port primary)" -U postgres -d postgres \
	-c "CREATE INDEX CONCURRENTLY ev_idx ON ev USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k', point_tid_columns = 'off', order_array_columns = 'off')" >/dev/null 2>&1
cic_rc=$?
kill "$PGBENCH_PID" 2>/dev/null; wait "$PGBENCH_PID" 2>/dev/null
check "a CREATE INDEX CONCURRENTLY under writers ends and the index is valid" test "$cic_rc" = 0 -a "$(sql primary "SELECT indisvalid FROM pg_index WHERE indexrelid = 'ev_idx'::regclass")" = t
check "... it verifies and answers as the heap, on the primary and on the standby" same_everywhere
sql primary "DROP INDEX ev_idx" >/dev/null

# a cancelled and a terminated build leave no temporary file and no index
tmpfiles() { sql primary "SELECT count(*) FROM pg_ls_tmpdir()" | tail -n 1; }
bg_build() { PGOPTIONS="-c maintenance_work_mem=64MB -c sift.test_col_segment_kb=4000 -c sift.test_col_unit_rows=500 -c temp_file_limit=64kB" psql -X -q -h "$d/sock" -p "$(port primary)" -U postgres -d postgres \
	-c "CREATE INDEX ev_idx ON ev USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k', point_tid_columns = 'off', order_array_columns = 'off')" >/dev/null 2>&1 & }
backend() { sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX ev_idx%' AND pid <> pg_backend_pid()" | tail -n 1; }
building() { [[ -n $(backend) ]]; }
settled() { [[ -z $(backend) ]]; }
bg_build
wait_for building 60 || bail "the build never ran"
sql primary "SELECT pg_cancel_backend($(backend))" >/dev/null
wait_for settled 120
check "a cancelled build leaves no temporary file and no index" test "$(tmpfiles)" = 0 -a "$(sql primary "SELECT count(*) FROM pg_class WHERE relname = 'ev_idx'")" = 0
bg_build
wait_for building 60 || bail "the build never ran"
sql primary "SELECT pg_terminate_backend($(backend))" >/dev/null
wait_for settled 120
check "a terminated build leaves no temporary file and no index" test "$(tmpfiles)" = 0 -a "$(sql primary "SELECT count(*) FROM pg_class WHERE relname = 'ev_idx'")" = 0

# the build's process dies before the index is published (sift.test_crash_dir: before its first directory update): crash recovery
PM=$(head -1 "$(dir primary)/data/postmaster.pid")
SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 9'
sql primary "${SETTINGS[@]}" "SET temp_file_limit = '64kB'" "SET sift.test_crash_dir = 1" "$CREATE" >/dev/null 2>&1
dead() { ! kill -0 "$PM" 2>/dev/null; }
wait_for dead 120 || bail "the postmaster did not stop"
node_start primary || bail "restart failed"
check "after a crash in the middle of a build the cluster is up and there is no index" test "$(sql primary "SELECT count(*) FROM pg_class WHERE relname = 'ev_idx' AND relkind = 'i'")" = 0
build "$CREATE"
check "the next build is sound: it verifies and answers as the heap" test -z "$(sql primary "SELECT sift.col_verify('ev_idx', true)" 2>&1)" -a "$(answer primary on)" = "$(answer primary off)"
check "no temporary file is left" test "$(tmpfiles)" = 0

finish
