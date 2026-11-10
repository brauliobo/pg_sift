#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup) of a primary with scope copies, one plain and one partitioned: transactions that change facts and pairs (and a data-modifying CTE that
# changes both) replay to copies that are exact on the hot standby (sift.scope_verify finds nothing, every key answers by the sift plan as the join does), in snapshots that readers
# of the standby take while it replays; a read of one key on the standby is the primary's; a copy made after the standby replays too. The standby is promoted: the triggers
# fire there, a change keeps the copies exact and sift.scope_drop works.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_scope.bash"

SECS=$(knob SIFT_SCOPE_REPLICA_SECS 8 4)
plan 10
node_init primary 49 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
node_start primary || bail "primary start failed"
scope_setup primary || bail "setup failed"
node_clone primary standby 50 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

# the check inside one snapshot of the standby: a commit that replays between the two reads of the check would show the copy and the tables of two moments
snapshot_exact() { [[ $(sql standby "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT scope_same('cflat') AND scope_same('cpart')" "COMMIT" | tail -n 1) == t ]]; }

sql primary "SELECT sift.scope_create('cthird', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['kind', 'amount'])" >/dev/null
FILES=($(scope_files primary) --max-tries=20)
bad=0 reads=0
pgbench_bg primary 4 "$SECS" "${FILES[@]}"
while kill -0 "$PGBENCH_PID" 2>/dev/null; do
	reads=$((reads + 1))
	snapshot_exact || bad=$((bad + 1))
done
wait "$PGBENCH_PID"
check "the copies are exact in $reads snapshots of the standby while it replays" test "$bad" == 0 -a "$reads" -ge 3
catchup primary standby || bail "the standby did not catch up"
check "the copies are exact on the primary after the churn" scope_exact primary
check "... and on the standby" scope_exact standby
check "a read of every key on the standby is the primary's (plain copy)" test "$(scope_answers standby cflat)" == "$(scope_answers primary cflat)"
check "... and the partitioned copy's" test "$(scope_answers standby cpart)" == "$(scope_answers primary cpart)"
check "the sift index of the copy verifies on the standby" test -z "$(sql standby "SELECT sift.col_verify('cflat_col', true)" 2>&1)"

catchup primary standby || bail "the standby did not catch up"
check "a copy made after the standby is exact on it" test "$(sql standby "SELECT count(*) FROM sift.scope_verify('cthird')")" == 0 -a "$(sql standby "SELECT count(*) FROM cthird")" == "$(sql primary "SELECT count(*) FROM cthird")"

node_promote standby
promoted_edits() {
	sql standby "INSERT INTO facts VALUES (900002, 42, now(), 2, 2, 'p')" "INSERT INTO pairs VALUES (42, 8), (42, 3) ON CONFLICT DO NOTHING" "UPDATE facts SET amount = amount + 1 WHERE id % 7 = 0" \
		"DELETE FROM pairs WHERE dim_a_id = 41" >/dev/null && scope_exact standby
}
check "the promoted standby keeps the copies exact under its own changes" promoted_edits
check "and sift.scope_drop works there" test "$(sql standby "SELECT sift.scope_drop('cthird')" "SELECT count(*) FROM sift.scope_copies" | tail -n 1)" == 2
finish
