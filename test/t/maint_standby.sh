#!/usr/bin/env bash
# A long query on a hot standby across a merge of the primary. The query counts a BM25 index and waits (sift.test_count_gate) after
# its plan opened the segments; the primary merges them and writes new ones over the freed blocks. Without hot_standby_feedback the
# primary logs the reuse as a recovery conflict (sift.standby_conflicts, on): the standby's replay waits for the query like it
# does for a heap query, and the count is the one of its snapshot. With the setting off the query fails with the retryable
# "segment recycled during scan"; with a short max_standby_streaming_delay the conflict cancels it like any standby query.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_STANDBY_ROWS 6000 3000)
plan 7
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
node_start primary || bail "primary start failed"
# a batch of documents of the given words, written to one segment
fill() {
	sql primary "SET sift.async_maintenance = off" \
		"INSERT INTO docs SELECT g, '$1 $2 w' || (g % 211) || ' x' || (g * 7919 % 2003) || ' y' || (g * 104729 % 1009) FROM generate_series(1, $ROWS) g" \
		"SELECT sift.promote('docs_idx')" >/dev/null || bail "fill failed"
}
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE docs (id int, body text) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX docs_idx ON docs USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup failed"
sql primary "CHECKPOINT" >/dev/null

node_clone primary standby 3 "hot_standby_feedback = off
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
node_start standby || bail "standby start failed"
D=$(dir standby)
COUNT="SELECT count(*) FROM docs WHERE body @@> 'alpha OR beta'"

# hold: the gate of the testing build (advisory lock 20983, 1) is held on the standby until $D/release exists
hold() {
	rm -f "$D/locked" "$D/release"
	{
		echo "SELECT pg_advisory_lock(20983, 1);"
		echo "\\! touch '$D/locked'"
		echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	} | sql standby >/dev/null &
	HOLDER=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
}
waiting() { sql standby "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"; }

# race SETTING: three segments of the words are written, the count waits at its gate on the standby, the primary (with SETTING in its session)
# merges everything and writes new segments over the blocks the merge freed. WANT: the count of the query's snapshot; REUSED: the index did
# not grow (a diagnostic: the replay that waits is the proof of a reuse); LSN: where the primary ends. The caller releases the gate.
race() {
	local hw
	for _ in 1 2 3; do fill alpha beta; done
	WANT=$(sql primary "$COUNT")
	catchup primary standby || bail "the standby did not catch up"
	hold
	sql standby "LOAD 'pg_sift'" "SET sift.test_count_gate = on" "SET enable_seqscan = off" "$COUNT" >"$D/count.out" 2>&1 &
	COUNTER=$!
	for _ in $(seq 400); do [[ $(waiting) -ge 1 ]] && break; sleep 0.05; done
	[[ $(waiting) -ge 1 ]] || bail "the count did not reach its gate"
	sql primary "SET sift.async_maintenance = off" "SELECT sift.merge('docs_idx', true) > 0" >/dev/null || bail "merge failed"
	hw=$(sql primary "SELECT high_water FROM sift.index_info('docs_idx')")
	sql primary "$1" "SET sift.async_maintenance = off" \
		"INSERT INTO docs SELECT g, 'gamma delta w' || (g % 97) || ' x' || (g * 7919 % 2003) FROM generate_series(1, $((ROWS * 2))) g" \
		"SELECT sift.promote('docs_idx')" >/dev/null || bail "rewrite failed"
	REUSED=$(sql primary "SELECT high_water <= $hw FROM sift.index_info('docs_idx')")
	LSN=$(sql primary "SELECT pg_current_wal_insert_lsn()")
	sleep 2
}
replayed() { sql standby "SELECT pg_last_wal_replay_lsn() >= '$LSN'"; }
release() { touch "$D/release"; wait "$HOLDER" "$COUNTER"; diag "count: $(head -1 "$D/count.out"); wanted $WANT, the index did not grow: $REUSED"; }

# the default: the replay waits for the query
race "SELECT 1"
check "the standby's replay waits for the long query" test "$(replayed)" == f
release
check "the count of the query's snapshot, no error" test "$(cat "$D/count.out")" == "$WANT"
catchup primary standby
check "the replay goes on once the query is done" test "$(replayed)" == t

# the setting off: nothing makes the replay wait, the query meets the recycled blocks
race "SET sift.standby_conflicts = off"
check "conflicts off: the replay does not wait" test "$(replayed)" == t
release
check "conflicts off: the query gets the retryable 40001" grep -q "segment recycled during scan, retry" "$D/count.out"

# a short delay: the conflict cancels the query like any standby query
sql standby "ALTER SYSTEM SET max_standby_streaming_delay = '1s'" "SELECT pg_reload_conf()" >/dev/null
sleep 1
race "SELECT 1"
release
check "delay 1s: the query is cancelled by the recovery conflict" grep -q "conflict with recovery" "$D/count.out"
catchup primary standby
check "the standby verifies the index and counts as the primary" test -z "$(sql standby "SELECT sift.verify('docs_idx', true)" 2>&1)" -a \
	"$(sql standby "$COUNT")" == "$(sql primary "$COUNT")"
finish
