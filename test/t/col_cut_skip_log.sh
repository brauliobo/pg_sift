#!/usr/bin/env bash
# A VACUUM whose cut of the free end of a sift_col file waited for its lock in vain says so once, with the blocks the online cut moved and gave back instead: VACUUM VERBOSE reports it (INFO), and an
# autovacuum that logs reports it in the server log (LOG). The reason is the lock another session holds on the table for the whole wait, and what the online cut did in its place.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_CS_ROWS 200000 100000)
plan 5
node_init skip 5 "autovacuum = on
autovacuum_naptime = 1s
autovacuum_vacuum_insert_threshold = 500
log_autovacuum_min_duration = 0"
node_start skip || bail "start failed"
sql skip "CREATE EXTENSION pg_sift" >/dev/null || bail "setup failed"
# t: table with a loose file (a merge writes its output above its inputs), autovacuum off for it until the test flips it
loosen() {
	sql skip "CREATE TABLE $1 (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
		"INSERT INTO $1 SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
		"VACUUM (FREEZE, ANALYZE) $1" >/dev/null || bail "load failed"
	sql skip "SET sift.async_maintenance = off" "CREATE INDEX $1_idx ON $1 USING sift_col (id, grp, amount, pad) WITH (key_cut = grp)" >/dev/null || bail "create failed"
	for k in 1 2 3; do
		sql skip "SET sift.async_maintenance = off" "INSERT INTO $1 SELECT id + $k * 10000000, grp, amount, pad FROM $1 WHERE id < 10000000 AND id % 5 = $k - 1" \
			"SELECT sift.col_promote('$1_idx')" >/dev/null
	done
	sql skip "SELECT sift.col_merge('$1_idx', true)" >/dev/null
}
# a reader that holds the table for $2 seconds (the AccessShareLock on the index refuses the AccessExclusiveLock of the cut)
hold() { sql skip "BEGIN" "SELECT count(*) FROM $1 WHERE id < 10" "SELECT pg_sleep($2)" "COMMIT" >/dev/null 2>&1 & HOLDER=$!; sleep 0.5; }

loosen t
hold t 4
out=$(sql skip "SET sift.async_maintenance = off" "SET lock_timeout = '500ms'" "SET client_min_messages = info" "VACUUM (VERBOSE) t" 2>&1)  # timeout-ok: the lock_timeout ends the cut's wait that the skip log reports
wait "$HOLDER"
diag "$(grep 'not truncating' <<<"$out")"
check "VACUUM VERBOSE says the lock was held and what the online cut did" grep -q 'index "t_idx": a lock is held on it (waited [0-9]* ms): [0-9]* blocks moved, [0-9]* given back online' <<<"$out"
check "it says so once" test "$(grep -c 'a lock is held on it' <<<"$out")" -eq 1
out=$(sql skip "SET sift.async_maintenance = off" "SET client_min_messages = info" "VACUUM (VERBOSE) t" 2>&1)
check "without a reader nothing is reported and the file is cut" test -z "$(grep 'a lock is held on it' <<<"$out")" -a \
	"$(sql skip "SELECT pg_relation_size('t_idx') < 15 * bytes / 10 FROM sift.col_index_info('t_idx')")" == t

# an autovacuum that logs: the reader holds the table longer than the 5 s wait
loosen u
hold u 12
sql skip "INSERT INTO u SELECT id + 50000000, grp, amount, pad FROM u WHERE id < 2000" "ALTER TABLE u SET (autovacuum_enabled = on)" >/dev/null
for _ in $(seq 150); do
	grep -q 'index "u_idx": a lock is held on it' "$(dir skip)/postgres.log" && break
	sleep 0.2
done
wait "$HOLDER"
line=$(grep 'index "u_idx": a lock is held on it' "$(dir skip)/postgres.log" | head -1)
diag "$line"
check "the autovacuum logs it at LOG with the blocks moved and given back" grep -q 'LOG:.*index "u_idx": a lock is held on it (waited 5000 ms): [0-9]* blocks moved, [0-9]* given back online' <<<"$line"
check "intact" test -z "$(sql skip "SELECT sift.col_verify('u_idx', true)" 2>&1)"
finish
