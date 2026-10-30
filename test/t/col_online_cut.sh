#!/usr/bin/env bash
# While a physical standby streams, VACUUM gives the free end of a sift_col file back under an ExclusiveLock, which a standby does not replay: the replay does not stall
# behind a long query of the standby on the table. Only the blocks the horizon says no snapshot can read go (P7a in core/store.h): a build after a standby snapshot keeps
# its freed blocks in the file until the snapshot ends. A standby without feedback gets the recovery conflict of the reuse. A churn of scans, inserts and VACUUMs, and a kill
# between the free list and the truncation, leave an index that agrees with its table on both servers.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_OC_ROWS 400000 200000)
plan 16
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 512MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
D=$(dir primary)
OPTS="SET max_parallel_maintenance_workers = 4"
SMALL="SET min_parallel_table_scan_size = 0"
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
tight() { echo "# file $(size) live $(live)" >&2; test "$(size)" -le "$(( $(live) * ${1:-110} / 100 ))"; }
loose() { test "$(size)" -gt "$(( $(live) * ${1:-19} / 10 ))"; }
vacuum() { sql primary "SET sift.async_maintenance = off" "VACUUM t" >/dev/null; }
# VACUUM until the file is cut: the feedback of the standby reaches the primary a second after a snapshot ends, longer on a busy host
vacuum_until_cut() {
	for _ in $(seq 15); do
		vacuum
		[[ $(size) -le $(( $(live) * 110 / 100 )) ]] && return 0
		sleep 2
	done
}
# the free list of the index: the run that ends the file, the holes before it, the records still holding a safexid
lay() {
	sql primary "WITH f AS (SELECT start, nblocks, pending FROM sift._test_freelist('t_idx')), i AS (SELECT high_water hw FROM sift.col_index_info('t_idx'))
		SELECT 'tail_mb ' || round(coalesce(sum(nblocks) FILTER (WHERE start + nblocks = hw), 0) * 8 / 1024.0, 1) || ' holes_mb ' || round(coalesce(sum(nblocks) FILTER (WHERE start + nblocks < hw), 0) * 8 / 1024.0, 1) || ' pending_mb ' || round(coalesce(sum(nblocks) FILTER (WHERE pending), 0) * 8 / 1024.0, 1) FROM f, i"
}
# the layout of a build whose merge went into its gap and whose free end was not cut (the 3.6.1 concurrent build): live in front, free blocks behind
drop_idx() { sql primary "DROP INDEX IF EXISTS t_idx" >/dev/null; catchup primary standby; }
# a parallel build can leave a small object of the index (a page of the free list, a lane) after the free run, and no cut passes it: build again until the free run ends the file
ends_file() { # the free records that run into the end of the file hold at least 1000 blocks
	[[ $(sql primary "WITH RECURSIVE f AS (SELECT start, nblocks FROM sift._test_freelist('t_idx')), r AS (SELECT start, nblocks FROM f WHERE start + nblocks = (SELECT relation_blocks FROM sift.col_index_info('t_idx'))
		UNION ALL SELECT f.start, f.nblocks FROM f JOIN r ON f.start + f.nblocks = r.start) SELECT coalesce(sum(nblocks), 0) >= 1000 FROM r") == t ]]
}
build_idx() {
	for _ in $(seq 20); do
		sql primary "$OPTS" "$SMALL" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp, vacuum_truncate = off)" >/dev/null || bail "create failed"
		sql primary "ALTER INDEX t_idx SET (vacuum_truncate = on)" >/dev/null
		catchup primary standby
		ends_file && return 0
		sql primary "DROP INDEX t_idx" >/dev/null
		catchup primary standby
	done
	bail "no build left its free run at the end of the file"
}
# a transaction after the build moves the horizon past the blocks it released (a server in use has them all the time), and the feedback of the standby tells the primary
past() { sql primary "SELECT pg_current_xact_id()" >/dev/null; catchup primary standby; sleep 3; }
bloat() { drop_idx; build_idx; past; }
# hold SECONDS: a transaction of the standby that reads the table, then waits, then reads the index
hold() {
	rm -f "$D/held"
	{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t WHERE id < 10;"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep($1);"
	  echo "SET enable_seqscan = off; SELECT 'after', count(*), sum(amount) FROM t WHERE grp = 5;"; echo "COMMIT;"; } | sql standby >"$D/hold.out" 2>&1 &
	HOLDER=$!
	until [[ -e $D/held ]]; do sleep 0.05; done
}
answer() { sql "$1" "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5"; }
table_answer() { sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_col_scan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5"; }
replayed_within() { # the standby has replayed what the primary wrote, $1 seconds later
	local lsn
	lsn=$(sql primary "SELECT pg_current_wal_insert_lsn()")
	sleep "$1"
	[[ $(sql standby "SELECT pg_last_wal_replay_lsn() >= '$lsn'") == t ]]
}
held_answer_ok() { test "$(grep after "$D/hold.out" | tr -d ' ')" == "after|$(answer primary)"; }
no_failure() { ! grep -q -E 'ERROR|pgbench: error|failed transactions: [1-9]' "$1"; }

# 1. a standby query on the table: the free end is cut, the replay does not wait, the query reads the same
bloat
check "the free end stays behind the live segments (the file is over 1.9 times the live bytes)" loose
hold 20
vacuum_until_cut
check "a standby query holds the table: the free end is cut (at most 1.1 times the live bytes)" tight
check "... and the replay does not stall behind the query" replayed_within 3
wait "$HOLDER"
check "... the query reads the same rows after the cut as the primary" held_answer_ok

# 2. the gate: blocks released after the standby snapshot stay held (not reusable, so no cut takes them) until the snapshot ends
pending() { sql primary "SELECT coalesce(sum(nblocks), 0) FROM sift._test_freelist('t_idx') WHERE pending"; }
hold 16
sql primary "UPDATE t SET amount = amount + 1 WHERE id % 3 = 0" >/dev/null
vacuum
echo "# gate: during the snapshot file $(size) live $(live) $(lay)" >&2
check "blocks released after a standby snapshot are held by the horizon" test "$(pending)" -gt 0
check "... and the replay does not stall behind the query" replayed_within 3
wait "$HOLDER"
past
vacuum
past
vacuum
echo "# gate: after the snapshot ended file $(size) live $(live) $(lay)" >&2
check "... and reusable once the snapshot ends" test "$(pending)" -eq 0
check "... the index counts as the table" test "$(answer primary)" == "$(table_answer)"

# 3. a standby without feedback: the reuse record is the recovery conflict that waits for or cancels its snapshot
sql standby "ALTER SYSTEM SET hot_standby_feedback = off" "ALTER SYSTEM SET max_standby_streaming_delay = '2s'" "SELECT pg_reload_conf()" >/dev/null
sleep 2
drop_idx
hold 15
build_idx
past
vacuum
wait "$HOLDER"
echo "# no feedback: $(cat "$D/hold.out" | tr '\n' ' ' | cut -c1-200)" >&2
check "a standby without feedback: its query is cancelled by the recovery conflict or reads what the primary has" bash -c "grep -q -E 'conflict with recovery' '$D/hold.out' || grep -q after '$D/hold.out'"
check "... and never reads a block past the end of the file" bash -c "! grep -q -i -E 'could not read block|read only 0 of' '$D/hold.out'"
sql standby "ALTER SYSTEM SET hot_standby_feedback = on" "ALTER SYSTEM SET max_standby_streaming_delay = -1" "SELECT pg_reload_conf()" >/dev/null
sleep 2

# 4. churn: scans, inserts and VACUUMs on the primary, a long query on the standby, a cut now and then
bloat
hold 24
printf 'INSERT INTO t SELECT -g, g %% 500, 1, $$w$$ FROM generate_series(1, 20) g;\n' >"$D/ins.sql"
printf '\\set g random(0, 499)\nSELECT count(*), sum(amount) FROM t WHERE grp = :g;\n' >"$D/rd.sql"
pgbench_bg primary 4 16 -f "$D/ins.sql@1" -f "$D/rd.sql@3"
for _ in 1 2 3 4; do sleep 3; sql primary "SET sift.async_maintenance = off" "SELECT sift.col_maintain('t_idx')" "VACUUM t" >/dev/null; done
wait "$PGBENCH_PID"
wait "$HOLDER"
check "churn: no statement failed" no_failure "$D/pgbench.log"
catchup primary standby
check "... the index counts as the table on the primary" test "$(answer primary)" == "$(table_answer)"
check "... the standby verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"

# 5. a kill between the free list and the truncation
bloat
SIFT_TMP=$(dir primary) leak_expect 1 "terminated by signal 9"
sql primary "SET sift.async_maintenance = off" "SET sift.test_space_crash = 3" "VACUUM t" >/dev/null 2>&1
for _ in $(seq 100); do [[ -f $D/data/postmaster.pid ]] || break; sleep 0.1; done
node_start primary || bail "primary did not restart"
check "a kill before the truncation: the primary verifies the index" test -z "$(sql primary "SELECT sift.col_verify('t_idx', true)" 2>&1)"
vacuum
check "... the next VACUUM cuts the file" tight
catchup primary standby
check "... the standby verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
