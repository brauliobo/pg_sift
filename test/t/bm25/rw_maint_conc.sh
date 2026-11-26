#!/usr/bin/env bash
# Readers, writers and maintenance of a BM25 index do not wait for one another: a SELECT of an open write buffer writes no WAL, the writers of a UNIQUE index
# never stall behind a promotion of their own, a promotion does not wait for a merge of the worker, and a VACUUM that is slow to merge holds back no promotion.
# The merge and the VACUUM are held by sift.test_merge_gate and sift.test_cleanup_gate (a testing build): they wait for an advisory lock the test holds, so no timing is involved.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

SECS=$(knob SIFT_RW_SECS 4 3)
plan 6
node_init rw 5 "autovacuum = off
sift.async_maintenance = on
deadlock_timeout = 5ms
log_lock_waits = on
shared_buffers = 64MB
max_wal_size = 512MB"
node_start rw || bail "start failed"
D=$(dir rw)
sql rw "CREATE EXTENSION pg_sift_test CASCADE" "CREATE EXTENSION pg_walinspect" "CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 4096)" \
	"INSERT INTO t (body) SELECT string_agg('w' || ((g * 7 + j * 13) % 300), ' ') FROM generate_series(1, 500) g, generate_series(1, 20) j GROUP BY g" \
	"VACUUM (INDEX_CLEANUP off, TRUNCATE off) t" >/dev/null || bail "setup failed"

# 1. a SELECT of 500 documents of an open write buffer: no record of the index is written, however many clients read
printf '%s\n' "SELECT count(*) FROM t WHERE body @@> 'w7';" >"$D/read.sql"
PGOPTIONS="-c enable_seqscan=off" pgbench -n -h "$D/sock" -p "$(port rw)" -U postgres -c 8 -j 4 -T 1 -f "$D/read.sql" postgres >/dev/null 2>&1
sql rw "CHECKPOINT" >/dev/null
l0=$(sql rw "SELECT pg_current_wal_insert_lsn()")
PGOPTIONS="-c enable_seqscan=off" pgbench_bg rw 32 "$SECS" -f "$D/read.sql"
wait "$PGBENCH_PID"
l1=$(sql rw "SELECT pg_current_wal_insert_lsn()")
records=0
[[ $l0 == "$l1" ]] || records=$(sql rw "SELECT coalesce(sum(count), 0) FROM pg_get_wal_stats('$l0', '$l1') WHERE \"resource_manager/record_type\" = 'Generic'")
diag "$(grep -E "^(number of transactions actually|tps)" "$D/pgbench.log"); the reads wrote $(sql rw "SELECT pg_wal_lsn_diff('$l1', '$l0')") bytes of WAL, $records generic records"
check "32 clients reading an open write buffer of 500 documents write no WAL" test "$records" == 0

# 2. 16 writers of a UNIQUE index of 4 KB documents: a writer waits at most for the lookup and the write of the row of its key, never for a promotion
sql rw "CREATE TABLE u (id bigserial, body text)" \
	"CREATE UNIQUE INDEX u_idx ON u USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "unique setup failed"
printf '%s\n' "\\set n random(1, 9223372036854775806)" \
	"INSERT INTO u (body) SELECT string_agg(md5(:n::text || g::text), ' ') FROM generate_series(1, 120) g;" >"$D/write.sql"
first=$(wc -l <"$D/postgres.log")
PGOPTIONS="-c sift.async_maintenance=off" pgbench_bg rw 16 "$SECS" -f "$D/write.sql"
wait "$PGBENCH_PID"
diag "$(grep -E "^(number of transactions actually|tps)" "$D/pgbench.log")"
stalls=$(tail -n +"$((first + 1))" "$D/postgres.log" | grep -c "still waiting for ExclusiveLock on tuple .* of relation $(sql rw "SELECT 'u_idx'::regclass::oid")")
check "no writer waited 5 ms for the key lock of another ($stalls waits)" test "$stalls" == 0
check "no write failed" grep -q "number of failed transactions: 0 " "$D/pgbench.log"

# 3. a promotion does not wait for the merge of the worker: it is held at its gate, and the buffer that is sealed meanwhile is promoted all the same
sql rw "CREATE TABLE m (id serial, body text)" "ALTER DATABASE postgres SET sift.test_merge_gate = on" \
	"CREATE INDEX m_idx ON m USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "merge setup failed"
DOCS="INSERT INTO m (body) SELECT string_agg(md5(g::text || j::text), ' ') FROM generate_series(1, %s) g, generate_series(1, 30) j GROUP BY g"
SEALED="SELECT count(*) FROM sift.segments('m_idx') WHERE kind = 'sealed buffer'"
rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 4);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql rw >/dev/null &
hp=$!
wait_for "[[ -e $D/locked ]]" 60
for _ in $(seq 120); do
	sql rw "$(printf "$DOCS" 100)" >/dev/null || bail "insert failed"
	[[ $(sql rw "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break
	sleep 0.25
done
sql rw "$(printf "$DOCS" 100)" >/dev/null || bail "insert failed"
check "the buffer sealed while the worker holds its merge is promoted" wait_for "[[ \$(sql rw \"$SEALED\") == 0 ]]" 20
touch "$D/release"
wait "$hp"
sql rw "ALTER DATABASE postgres RESET sift.test_merge_gate" "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'" >/dev/null

# 4. a VACUUM that waits after its merges (the gate of its cleanup) holds back no promotion: the writers promote the buffers they seal themselves
sql rw "DELETE FROM m WHERE id % 3 = 0" >/dev/null
rm -f "$D/locked2" "$D/release2"
{ echo "SELECT pg_advisory_lock(20983, 2);"; echo "\\! touch '$D/locked2'"; echo "\\! while [ ! -e '$D/release2' ]; do sleep 0.05; done"; } | sql rw >/dev/null &
hp=$!
wait_for "[[ -e $D/locked2 ]]" 60
sql rw "SET sift.test_cleanup_gate = on" "VACUUM m" >"$D/vacuum.out" 2>&1 &
vp=$!
wait_for "[[ \$(sql rw \"SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted\") -ge 1 ]]" 60
INSERTS=("SET sift.async_maintenance = off")
for _ in $(seq 12); do INSERTS+=("$(printf "$DOCS" 100)"); done
sql rw "${INSERTS[@]}" >/dev/null || bail "insert failed"
check "the buffers sealed during a VACUUM are promoted by the writers" test "$(sql rw "$SEALED")" -lt 4
touch "$D/release2"
wait "$hp" "$vp"
out=$(sql rw "VACUUM m" "SELECT sift.verify('m_idx', true), sift.verify('u_idx', true)" 2>&1); check "VACUUM ends clean, every row of both indexes is intact" test "$(grep -c -E "ERROR|corrupt" <<<"$out")" == 0
finish
