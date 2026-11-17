#!/usr/bin/env bash
# sift.col_value_store_compact takes its locks as VACUUM does, without waiting: when a session holds a lock that conflicts with the table's, a member's or the store's lock it needs, it
# fails at once and names the relation, and it never queues behind that session (a queued request blocks every statement that comes after it). A session that holds the store in
# RowExclusiveLock (a writer between its codes and its segment) is one of them. Once the locks are free the compaction runs.
. "$(dirname "$0")/lib.bash"

plan 5
node_init vc 35 "autovacuum = off
sift.async_maintenance = off
sift.col_auto_merge = off"
node_start vc || bail "start failed"
D=$(dir vc)
STORE=vr_t_url_sift_values
sql vc "CREATE EXTENSION pg_sift_test CASCADE" "SET sift.test_vstore_columns = 'url'" \
	"CREATE TABLE vr_t (id int PRIMARY KEY, url text)" "CREATE INDEX vr_a ON vr_t USING sift_col (id, url)" "CREATE INDEX vr_b ON vr_t USING sift_col (url, id)" \
	"INSERT INTO vr_t SELECT i, 'u' || i FROM generate_series(1, 200) i" >/dev/null || bail "setup failed"
compact() { sql vc "LOAD 'pg_sift'" "SELECT sift.col_value_store_compact('vr_t', 'url')" >"$D/compact.out" 2>&1; }
hold() { # SQL: a session that runs it in a transaction and keeps the transaction open until release
	rm -f "$D/locked" "$D/release"
	{ echo "BEGIN;"; echo "$1;"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql vc >/dev/null &
	holder=$!
	wait_for "[[ -e $D/locked ]]" 30
}
drop() { touch "$D/release"; wait "$holder"; }

compact_while() { # SQL held by another session: the compaction ends, in an error that names $2
	hold "$1"
	compact &
	local c=$!
	wait_for "! kill -0 $c 2>/dev/null" 30
	local rc=$?
	drop
	wait "$c" 2>/dev/null
	return $rc
}

compact_while "SELECT sift._test_vstore_intern('$STORE', array['held'])"
check "a writer of the store in its transaction: the compaction does not wait for it" test $? -eq 0
check "it names the store" grep -q "$STORE" "$D/compact.out"
compact_while "LOCK TABLE vr_t IN SHARE MODE"
check "a lock on the table that conflicts with ShareUpdateExclusiveLock: the compaction does not wait" test $? -eq 0
check "it names the table" grep -q "vr_t" "$D/compact.out"
compact
check "with the locks free the compaction runs" test "$(cat "$D/compact.out")" -ge 0
finish
