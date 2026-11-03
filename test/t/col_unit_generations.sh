#!/usr/bin/env bash
. "$(dirname "$0")/lib.bash"
plan 16
node_init p 31 "wal_level = replica
max_wal_senders = 4
max_replication_slots = 4
synchronous_commit = on
max_prepared_transactions = 4"
node_start p || bail "primary start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
 "SET sift.eval_methods = 'unit_rewrite'" \
 "CREATE TABLE events(id int PRIMARY KEY, tenant_id int, counter_a int) WITH (autovacuum_enabled=false)" \
 "INSERT INTO events SELECT i,i%10,i FROM generate_series(1,8000) i" \
 "CREATE INDEX events_c ON events USING sift_col(id,tenant_id,counter_a) WITH(track_updates='id',key_cut='id',key_cut_rows=1024,vacuum_truncate=false)" \
 "VACUUM (FREEZE,ANALYZE) events" >/dev/null || bail "setup failed"
node_clone p s 32 "hot_standby_feedback = off
max_standby_streaming_delay = 0"
node_start s || bail "standby start failed"
catchup p s || bail "catchup failed"
A="SELECT tenant_id,count(*),sum(counter_a) FROM events GROUP BY tenant_id ORDER BY tenant_id"
answer() { sql "$1" "SET enable_seqscan=off" "$A"; }
oracle() { sql "$1" "SET sift.enable_agg=off; SET enable_indexscan=off; SET enable_indexonlyscan=off; SET enable_bitmapscan=off" "$A"; }
check "independently owned units" test "$(sql p "SELECT bool_and(units=1) FROM sift.col_segments('events_c') WHERE kind='immutable'")" == t
D=$(dir p)
DS=$(dir s)
sql p "UPDATE events SET counter_a=counter_a+1 WHERE id=7500" "SELECT pg_log_standby_snapshot()" >/dev/null
catchup p s || bail "pre-update horizon replay failed"
{ echo 'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_backend_pid();'; echo "\\! touch '$DS/death.held'; while [ ! -e '$DS/death.release' ]; do sleep 0.05; done"; echo "$A; COMMIT;"; } | sql s >"$DS/death.out" 2>"$DS/death.err" &
dp=$!
until [[ -e $DS/death.held ]]; do sleep 0.05; done
# The earlier dead version makes VACUUM call the index while the held snapshot still needs other tombstoned entries.
# An old primary snapshot prevents VACUUM from declaring its versions removable.
{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ; $A;"; echo "\\! touch '$D/held'; while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "$A; COMMIT;"; } | sql p >"$D/old.out" &
hp=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql p "CREATE TABLE before AS SELECT gen FROM sift.col_segments('events_c') WHERE kind='immutable'" \
 "CREATE TABLE prior_tids AS SELECT ctid AS tid FROM events WHERE id<=400" "UPDATE events SET counter_a=counter_a+1 WHERE id<=400" "SET sift.eval_methods='unit_rewrite'" "VACUUM (INDEX_CLEANUP ON) events" >/dev/null
check "held snapshot forbids early replacement" test "$(sql p "SELECT count(*)=(SELECT count(*) FROM before) FROM before JOIN sift.col_segments('events_c') USING(gen)")" == t
check "pre-update standby snapshot survives the unrelated older heap death" test "$(sql s "SELECT EXISTS(SELECT FROM pg_stat_activity WHERE pid=$(head -1 "$DS/death.out"))")" == t
touch "$D/release"; wait "$hp"
check "old snapshot sees its original rows" test "$(sed -n '1,10p' "$D/old.out")" == "$(sed -n '11,20p' "$D/old.out")"
# The standby snapshot has never opened this index. Directory publication must not bypass the heap conflict horizon.
# A reader beginning after the update still holds the old allocation, although VACUUM may remove the ended versions.
rm "$D/held" "$D/release"
{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;"; echo "\\! touch '$D/held'; while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql p >"$D/retire.out" &
hp=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql p "CREATE TABLE old_extents AS SELECT * FROM sift.col_extents('events_c') WHERE gen IN (SELECT gen FROM before)" >/dev/null
sql p "SELECT pg_log_standby_snapshot()" >/dev/null
catchup p s || bail "snapshot horizon replay failed"
DS=$(dir s)
{ echo 'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_backend_pid();'; echo "\\! touch '$DS/held'; while [ ! -e '$DS/release' ]; do sleep 0.05; done"; echo "$A; COMMIT;"; } | sql s >"$DS/old.out" 2>"$DS/old.err" &
sp=$!
until [[ -e $DS/held ]]; do sleep 0.05; done
{ echo "SELECT pg_advisory_lock(20983,5);"; echo "\\! touch '$D/unit.locked'; while [ ! -e '$D/unit.release' ]; do sleep 0.05; done"; } | sql p >/dev/null &
gp=$!
until [[ -e $D/unit.locked ]]; do sleep 0.05; done
sql p "SET sift.eval_methods='unit_rewrite'" "SET sift.test_col_unit_gate=on" "VACUUM (INDEX_CLEANUP ON) events" >"$D/vacuum.out" 2>&1 &
vp=$!
at_gate=false
for _ in $(seq 400); do
	if [[ $(sql p "SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND NOT granted") == 1 ]]; then at_gate=true; break; fi
	sleep 0.05
done
if ! $at_gate; then
	touch "$D/unit.release" "$D/release" "$DS/death.release" "$DS/release"
	wait "$gp" "$vp" "$hp" "$dp" "$sp" || true
	bail "unit publication did not reach the gate"
fi
check "VACUUM is still vacuuming indexes at unit publication" test "$(sql p "SELECT count(*)=1 FROM pg_stat_progress_vacuum WHERE phase='vacuuming indexes'")" == t
sql p "CHECKPOINT" >/dev/null
catchup p s || bail "partial VACUUM replay failed"
touch "$DS/death.release"; wait "$dp" || true
check "heap pruning cancels an old standby snapshot before index publication" grep -Eq "recovery|conflict|terminating connection" "$DS/death.err"
check "post-update standby snapshot survives index publication" test "$(sql s "SELECT EXISTS(SELECT FROM pg_stat_activity WHERE pid=$(head -1 "$DS/old.out"))")" == t
touch "$D/unit.release"; wait "$gp" "$vp" || bail "VACUUM failed"
catchup p s || bail "rewrite replay failed"
check "only the owning generation retired after the snapshot horizon" test "$(sql p "SELECT count(*)=(SELECT count(*)-1 FROM before) FROM before JOIN sift.col_segments('events_c') USING(gen)")" == t
check "every retired unit extent is pending while its reader lives" test "$(sql p "SELECT count(*)>0 AND bool_and(EXISTS(SELECT FROM sift.col_extents('events_c') f WHERE f.pending AND e.first_block>=f.first_block AND e.first_block+e.nblocks<=f.first_block+f.nblocks)) FROM old_extents e WHERE NOT EXISTS(SELECT FROM sift.col_segments('events_c') s WHERE s.gen=e.gen)")" == t
touch "$D/release"; wait "$hp"
# Force reuse after the primary reader ends; the standby has no feedback. The reuse WAL horizon must cancel that old snapshot.
sql p "SET sift.eval_methods='unit_rewrite'" "SELECT pg_current_xact_id()" "SELECT sift._test_alloc('events_c',1,(reusable_blocks+pending_blocks)::int,1) FROM sift.col_space('events_c')" >/dev/null
catchup p s || bail "reuse replay failed"
touch "$DS/release"; wait "$sp" || true
check "standby reuse horizon cancels the old snapshot" grep -Eq "recovery|conflict|terminating connection" "$DS/old.err"
check "primary rewritten result equals heap" test "$(answer p)" == "$(oracle p)"
check "standby rewritten result equals primary" test "$(answer s)" == "$(oracle p)"
sql p "DELETE FROM events WHERE id<=400" "VACUUM (INDEX_CLEANUP ON) events" "INSERT INTO events SELECT i,i%10,i FROM generate_series(9001,9400) i" >/dev/null
catchup p s || bail "reuse replay failed"
check "insertion actually reuses a retired indexed TID" test "$(sql p "SELECT EXISTS(SELECT FROM prior_tids d JOIN events e ON e.ctid=d.tid WHERE e.id>=9001)")" == t
check "TID reuse and generation retirement preserve answers" test "$(answer s)" == "$(oracle p)"
sql p "UPDATE events SET counter_a=counter_a+1 WHERE id=8000" "BEGIN; UPDATE events SET counter_a=-1 WHERE id>2000 AND id<=2400; PREPARE TRANSACTION 'unit_in_doubt'" "SET sift.eval_methods='unit_rewrite'" "VACUUM (INDEX_CLEANUP ON) events" "SELECT sift.col_promote('events_c')" >/dev/null
check "a generation containing prepared inserts inherits no committed stamp" test "$(sql p "SELECT count(*) FILTER (WHERE stamp IS NULL)>0 FROM sift.col_stamps('events_c')")" == t
check "prepared update is not an authority to drop a row" test "$(answer p)" == "$(oracle p)"
sql p "ROLLBACK PREPARED 'unit_in_doubt'" >/dev/null
finish
