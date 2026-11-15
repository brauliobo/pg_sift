#!/usr/bin/env bash
# A VACUUM whose AccessExclusiveLock stays refused for the wait (readers overlap on the table all the time) gives the free blocks of a sift_col file back the way a VACUUM
# with a standby does: it seals and promotes a write buffer that lies above the end the file can have, moves the segments above that end into the holes below, and cuts the
# free end under an ExclusiveLock (readers are never kept out; a writer waits for the cut of the free end only). The file is within 1.3 times the live bytes after a few
# VACUUMs with the readers running, a writer stalls for milliseconds, a kill while the pages are copied or when the move is published leaves an index that agrees with its
# table, and a standby that replays it verifies the index.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_OF_ROWS 200000 100000)
plan 8
node_init primary 8 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 512MB
autovacuum = off
max_worker_processes = 16"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 9 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
node_start standby || bail "standby start failed"
D=$(dir primary)
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
tight() { test "$(size)" -le "$(( $(live) * ${1:-130} / 100 ))"; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
# the VACUUM sees no standby (the lock path it takes without one) while one streams and replays
vacuum() { sql primary "SET sift.async_maintenance = off" "SET sift.test_cut_with_standby = on" "SET lock_timeout = '300ms'" "VACUUM t" >/dev/null; }  # timeout-ok: the lock_timeout ends the cut's wait; the cut falls back online
fragment() {
	sql primary "DROP INDEX IF EXISTS t_idx" >/dev/null
	sql primary "SET sift.async_maintenance = off" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp)" >/dev/null || bail "create failed"
	for k in 1 2 3 4; do
		sql primary "SET sift.async_maintenance = off" "INSERT INTO t SELECT id + $k * 10000000, grp, amount, pad FROM t WHERE id < 10000000 AND id % 5 = $k - 1" "SELECT sift.col_promote('t_idx')" >/dev/null
	done
	sql primary "SELECT sift.col_merge('t_idx', true)" >/dev/null
	sql primary "INSERT INTO t SELECT id + 90000000 + $RANDOM, grp, amount, pad FROM t WHERE id < 1000" "SELECT sift.col_promote('t_idx')" >/dev/null
	# a later transaction: what the merges released is then reusable by the horizon
	sql primary "SELECT pg_current_xact_id()" >/dev/null
}
# two sessions whose statements overlap, so that some reader holds the table at every instant until stop_readers
readers() {
	rm -f "$D/stop"
	for off in 0 0.4; do
		(
			sleep "$off"
			while [[ ! -e $D/stop ]]; do sql primary "SELECT count(*), pg_sleep(0.8) FROM t WHERE id < 10" >/dev/null 2>>"$D/readers.err" || echo failed >>"$D/readers.err"; done
		) &
		READERS+=($!)
	done
	sleep 1
}
stop_readers() { touch "$D/stop"; wait "${READERS[@]}" 2>/dev/null; READERS=(); }
READERS=()
vacuum_until() {
	for _ in $(seq 30); do
		vacuum
		tight && return 0
		sql primary "SELECT pg_current_xact_id()" >/dev/null
		sleep 0.3
	done
	return 1
}
verified() { test -z "$(sql "$1" "SELECT sift.col_verify('t_idx', true)" 2>&1)"; }
answer() { sql "$1" "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5"; }
agree() { test "$(answer primary)" == "$(sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_col_scan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5")"; }
catchup() {
	local lsn
	lsn=$(sql primary "SELECT pg_current_wal_insert_lsn()")
	for _ in $(seq 600); do
		[[ $(sql standby "SELECT pg_last_wal_replay_lsn() >= '$lsn'") == t ]] && return 0
		sleep 0.1
	done
	return 1
}

# 1. readers overlap all the time: the AccessExclusiveLock is never free, the online path cuts the file
fragment
catchup || bail "the standby did not catch up"
loose=$(size)
check "the merges leave the file over 1.6 times the live bytes" test "$loose" -gt "$(( $(live) * 16 / 10 ))"
readers
# a writer at the same time: one row a statement, the time of each (psql start included); the longest is the stall of the cut
writer() {
	rm -f "$D/writer.ms"
	while [[ ! -e $D/stop ]]; do
		t0=$(now_ms)
		sql primary "INSERT INTO t VALUES (-1 - $RANDOM, 1, 1, 'w')" >/dev/null 2>>"$D/writer.err" || echo failed >>"$D/writer.err"
		echo $(( $(now_ms) - t0 )) >>"$D/writer.ms"
	done
}
writer & WRITER=$!
vacuum_until
stop_readers
wait "$WRITER"
sort -n "$D/writer.ms" >"$D/writer.sorted"
diag "file $loose -> $(size), live $(live); $(wc -l <"$D/writer.sorted") inserts, median $(sed -n "$(( $(wc -l <"$D/writer.sorted") / 2 + 1 ))p" "$D/writer.sorted") ms, longest $(tail -1 "$D/writer.sorted") ms"
check "readers on the table the whole time: the file is within 1.3 times the live bytes" tight
check "... no reader and no writer failed, and a writer stalled for under a second" test ! -s "$D/readers.err" -a ! -s "$D/writer.err" -a "$(tail -1 "$D/writer.sorted")" -lt 1000
recovered() { verified primary && agree; }
check "... the index verifies and counts as the table" recovered
catchup
check "... the standby that replayed the shrink and the cut verifies the index" verified standby

# 2. a kill while the pages are copied (phase 5) and when the move is published (phase 6), readers running
for phase in 5 6; do
	fragment
	readers
	# the first VACUUM seals and promotes the write buffer that ends the file; the blocks that frees are reusable once the readers that began before it are done
	vacuum
	sql primary "SELECT pg_current_xact_id()" >/dev/null
	sleep 1.2
	SIFT_TMP=$(dir primary) leak_expect 1 "terminated by signal 9"
	# a VACUUM reaches the phase when it has a run to move: ask again, with a transaction between, until the kill
	for _ in $(seq 8); do
		sql primary "SET sift.async_maintenance = off" "SET sift.test_cut_with_standby = on" "SET lock_timeout = '300ms'" "SET sift.test_space_crash = $phase" "VACUUM t" >/dev/null 2>&1  # timeout-ok: the lock_timeout ends the cut's wait; the cut falls back online
		for _ in $(seq 20); do [[ -f $D/data/postmaster.pid ]] || break; sleep 0.1; done
		[[ -f $D/data/postmaster.pid ]] || break
		sql primary "SELECT pg_current_xact_id()" >/dev/null
		sleep 1.2
	done
	stop_readers
	node_start primary || bail "primary did not restart"
	check "a kill in phase $phase: the index verifies and counts as the table" recovered
done
fragment
readers
vacuum_until
stop_readers
check "after the kills the VACUUMs with readers bring the file within 1.3 times the live bytes" tight
finish
