#!/usr/bin/env bash
# tap: pooled
# A lazy VACUUM runs with PROC_IN_VACUUM: every snapshot leaves its transaction id out, so another session that reads a catalog row the VACUUM updated judges the
# VACUUM's xid aborted and writes HEAP_XMAX_INVALID on the old version (and XMIN_INVALID on a new one in another page) while the VACUUM is still to commit. The row is then
# visible twice, or its update is lost, and pruning the page raises "dead heap-only tuple (N, M) is not linked to from any HOT chain". So a VACUUM of a table with a sift_col
# index must not take a transaction id: the choices of the index that the VACUUM's merge makes (key_cut, key_columns, point_tid_columns, order columns: 'auto') are not written
# by it, nor by a worker: the explicit rewrite chooses again and writes them. A VACUUM stopped at its cleanup gate (sift.test_cleanup_gate, a testing build, right after its merge) while a second session reads the index's
# pg_class row: the VACUUM has no xid, no tuple of pg_class has a committed updater and XMAX_INVALID, the row is there once, VACUUM pg_class and a scan of pg_class end
# without an error, and the choice the VACUUM saw is written by the explicit rewrite.
. "$(dirname "$0")/lib.bash"

plan 9
node_init vc 48 "autovacuum = off
sift.maintenance_naptime = 200
sift.log_min_duration = 0"
node_start vc || bail "start failed"
D=$(dir vc)
sql vc "CREATE EXTENSION pg_sift" "CREATE EXTENSION pageinspect" "CREATE TABLE bump (a int)" \
	"CREATE TABLE rc (id int PRIMARY KEY, tenant_id int, foreign_id int, counter_a int, updated_at timestamp, pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO rc SELECT i, i / 150, 1 + i % 12, hashint4(i) & 1048575, TIMESTAMP '2026-01-01' + (hashint4(i + 7) & 16777215) * INTERVAL '1 second', repeat('x', 100) FROM generate_series(1, 60000) i" \
	"VACUUM ANALYZE rc" \
	"CREATE INDEX rc_ix ON rc USING sift_col (id, tenant_id, foreign_id, counter_a, updated_at) WITH (dead_rewrite_pct = 50)" \
	"INSERT INTO rc SELECT i, (hashint4(i) & 2147483647) % 800, 1 + i % 12, hashint4(i) & 1048575, TIMESTAMP '2026-01-01', repeat('x', 100) FROM generate_series(60001, 120000) i" >/dev/null || bail "setup failed"

options() { sql vc "SELECT reloptions::text FROM pg_class WHERE relname = 'rc_ix'"; }
# the tuples of pg_class with an updater that committed and a header that says nobody updated them
damaged_rows() {
	sql vc "SELECT i.lp, i.t_xmin, i.t_xmax, to_hex(i.t_infomask) AS infomask, to_hex(i.t_infomask2) AS infomask2, i.t_ctid FROM generate_series(0, pg_relation_size('pg_class') / 8192 - 1) n, LATERAL heap_page_items(get_raw_page('pg_class', n)) i
		WHERE i.t_xmax <> 0 AND (i.t_infomask & 2048) <> 0 AND (i.t_infomask & 128) = 0 AND (i.t_infomask & 4096) = 0 AND pg_xact_status(i.t_xmax::text::xid8) = 'committed'"
}
damaged() { damaged_rows | awk 'END { print NR }'; }
rows_of_index() { sql vc "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM pg_class WHERE relname = 'rc_ix'" | tail -1; }
vacuum_xids() { sql vc "SELECT count(*) FROM pg_stat_activity WHERE query = 'VACUUM rc' AND backend_xid IS NOT NULL"; }
errors() { grep -cE 'ERROR|TRAP|dead heap-only' "$D/postgres.log"; }

# one VACUUM stopped at its cleanup gate: a second session commits something (its xid passes the VACUUM's) and reads the row of the index by its two paths, then the VACUUM goes on
XIDS=0
gated_vacuum() {
	rm -f "$D/locked" "$D/release"
	{
		echo "SELECT pg_advisory_lock(20983, 2);"
		echo "\\! touch '$D/locked'"
		echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	} | sql vc >/dev/null &
	local hp=$! vp
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql vc "LOAD 'pg_sift'" "SET sift.test_cleanup_gate = on" "VACUUM rc" >"$D/vacuum" 2>&1 &
	vp=$!
	for _ in $(seq 400); do [[ $(sql vc "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break; sleep 0.05; done
	XIDS=$((XIDS + $(vacuum_xids)))
	sql vc "INSERT INTO bump VALUES (1)" >/dev/null
	sql vc "SELECT relpages FROM pg_class WHERE oid = 'rc_ix'::regclass" >/dev/null
	sql vc "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT relpages FROM pg_class WHERE relname = 'rc_ix'" >/dev/null
	touch "$D/release"
	wait "$vp" "$hp"
}

# rechoose N: the choices of the index are 'auto' again, rows are inserted and a seventh of them deleted
rechoose() {
	sql vc "ALTER INDEX rc_ix SET (key_cut = 'auto', key_columns = 'auto', point_tid_columns = 'auto', dead_rewrite_pct = 50)" \
		"INSERT INTO rc SELECT 200000 + g + $1 * 100000, (hashint4(g) & 2147483647) % 800, 1 + g % 12, hashint4(g) & 1048575, TIMESTAMP '2026-01-01', repeat('x', 100) FROM generate_series(1, 20000) g" \
		"DELETE FROM rc WHERE id % 7 = $(($1 % 7))" >/dev/null
}

# the maintenance worker is off: the VACUUM's merge is where the choice was made (the round in which it does depends on the data, 6 or 7 here)
ROUNDS=0 BAD=0
for it in $(seq 9); do
	rechoose "$it"
	gated_vacuum
	ROUNDS=$it
	BAD=$(damaged)
	[[ $BAD != 0 || $XIDS != 0 ]] && break
done

check "the VACUUM stopped after its merge holds no transaction id (it assigned none in $ROUNDS rounds)" test "$XIDS" == 0
out=$(damaged_rows | sed 's/^/lp, xmin, xmax, infomask, infomask2, ctid: /')
check "no tuple of pg_class has a committed updater and HEAP_XMAX_INVALID" test "$BAD" == 0
out=
check "the index has one row in pg_class" test "$(rows_of_index)" == 1
check "a scan of pg_class ends without an error" test "$(sql vc "SELECT count(*) > 0 FROM pg_class" 2>&1)" == t
check "VACUUM pg_class ends without an error" test "$(sql vc "VACUUM pg_class" 2>&1 | grep -c ERROR)" == 0
check "the log has no error" test "$(errors)" == 0
# no worker writes the choices either, when sift.async_maintenance is on: the explicit rewrite does, in a transaction of its own
sql vc "ALTER DATABASE postgres SET sift.async_maintenance = on" >/dev/null
idle() { for _ in $(seq 200); do [[ $(sql vc "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift%'") == 0 ]] && return 0; sleep 0.1; done; return 1; }
chosen() {
	local it start
	for it in $(seq 10 14); do
		rechoose "$it"
		start=$(options)
		sql vc "VACUUM rc" >/dev/null
		idle || return 1
		[[ $(options) != "$start" ]] && return 1
		sql vc "SELECT sift.col_merge('rc_ix', true)" >/dev/null
		[[ $(options) != "$start" ]] && return 0
	done
	return 1
}
check "a VACUUM and its workers write no choice of the index, the explicit rewrite does" chosen
# a choice the data repeats is not written again
version() { sql vc "SELECT xmin FROM pg_class WHERE relname = 'rc_ix'"; }
repeated() {
	local before
	idle || return 1
	sql vc "SELECT sift.col_merge('rc_ix', true)" >/dev/null
	before=$(version)
	sql vc "SELECT sift.col_merge('rc_ix', true)" >/dev/null
	[[ $(version) == "$before" ]]
}
check "a rewrite that chooses what the index holds writes no catalog row" repeated
check "and the index verifies" test "$(sql vc "SELECT sift.col_verify('rc_ix', true)" 2>&1 | grep -c ERROR)" == 0

finish
