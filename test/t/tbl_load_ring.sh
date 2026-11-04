#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The workers of a COPY into a sift_table stay for the command (table/load/ring.c): the backend writes the rows of a group into shared memory and goes on reading
# while a worker writes the group's segment, and publishes the segments as they come. The blocks of a group written and not yet published are in no directory: the orphan
# sweep of a VACUUM leaves them alone (SIFT_LOAD_LOCK_BLKNO). With a streaming standby under generic WAL consistency checking: the load equals the load without workers (the
# same rows at the same TIDs) and the heap twin, also into a table with a b-tree, and a table whose default calls nextval loads (it cannot run in the parallel mode of the ring); a VACUUM, a
# second COPY and an INSERT during the load change nothing for it; and a load that dies at each stage of the ring leaves nothing: power loss while a worker writes the first group,
# while groups are written and none is published, and while some are published and the last are not, a COPY that fails on a row after the workers started, a savepoint rolled back
# after the ring ran. After each the table is empty on the primary and the standby, VACUUM leaves the file as small as the empty table's and no orphan block, the store verifies,
# and the same load works. The worker of the ring keeps one segment writer over its groups (a load that fits the writer's memory is one segment); sift.test_load_segment_kb makes it end segments so that the stages that need some do. SIFT_LR_ROWS (300000, 20000 quick; quick sets sift.test_load_unit_bytes = 128 (SIFT_LR_UNIT) so that rows of 3 MB are enough for the ring).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_LR_ROWS 300000 20000)
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 32MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8
wal_consistency_checking = 'generic'"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE src AS SELECT g AS id, g % 500 AS grp, (g % 997)::numeric(10,2) AS amount, md5(g::text) || md5((g * 7)::text) AS pad FROM generate_series(1, $ROWS) g" \
	"COPY src TO '$D/load.csv' CSV" \
	"CREATE TABLE lt (id int, grp int, amount numeric(10,2), pad text) USING sift_table" \
	"CREATE INDEX lt_grp ON lt (grp)" >/dev/null || bail "setup failed"
# three times the rows: enough groups for the stages that need several (the ring publishes the segments taken between two groups)
cat "$D/load.csv" "$D/load.csv" "$D/load.csv" >"$D/big.csv"
# a row that fails near the end of the file
{ head -n $((ROWS - 100)) "$D/load.csv"; echo 'x,1,1,y'; tail -n 99 "$D/load.csv"; } >"$D/bad.csv"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
wal_consistency_checking = 'generic'"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

OPTS=("SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET sift.table_write_buffer_limit = 2048" "SET sift.test_load_unit_bytes = $(knob SIFT_LR_UNIT 0 128)" "SET sift.test_par_workers = 0" "SET sift.test_load_publish = 1" "SET sift.test_load_segment_kb = $(knob SIFT_LR_SEG 4096 256)")
COPYSQL="COPY lt FROM '$D/load.csv' CSV"
GATE=7000
digest() { sql "$1" "SELECT count(*) || ':' || md5(string_agg(id || ':' || ctid::text || ':' || amount, ',' ORDER BY id, ctid)) FROM lt" | tail -1; }
rows() { sql "$1" "SELECT count(*) || ':' || md5(string_agg(id || ':' || grp || ':' || amount || ':' || pad, ',' ORDER BY id, grp, amount, pad)) FROM lt" | tail -1; }
answers() { sql "$1" "SELECT count(*) || ':' || sum(amount) FROM lt WHERE grp = 7" "SELECT count(*) FROM lt WHERE id = 12345" | tr '\n' ' '; }
twin() { sql primary "SELECT count(*) || ':' || sum(amount) FROM src WHERE grp = 7" "SELECT count(*) FROM src WHERE id = 12345" | tr '\n' ' '; }
# the high-water mark: the workers extend the file by several blocks at a time, and the tail past the mark is no space the load holds
used() { sql primary "SELECT high_water - free_blocks FROM sift.table_space('lt')"; }
orphans() { sql primary "SELECT sift._test_check('lt')"; }
reset() { sql primary "TRUNCATE lt" >/dev/null && sql primary "VACUUM lt" >/dev/null; }
same_on_standby() { catchup primary standby && [[ $(digest standby) == "$(digest primary)" ]] && [[ $(sql standby "SELECT sift._test_check('lt') >= 0") == t ]]; }
workers() { sql primary "SELECT current_setting('sift.test_par_workers')"; }

# the first load with the ring, then without workers, then with the segments published at the default pace
used0=$(used)
out=$(sql primary "${OPTS[@]}" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')" | tail -1)
check "the load starts workers" test "${out:-0}" -gt 0
with=$(digest primary)
onerows=$(rows primary)
check "the load equals the heap twin: the rows" test "$(sql primary "SELECT count(*) FROM lt")" == "$ROWS"
check "... the answers, by the scan and by the b-tree" test "$(answers primary)" == "$(twin)"
check "... the store verifies and no block is left over" test "$(orphans)" == 0
check "the standby replays the same rows at the same TIDs under generic WAL consistency checking" same_on_standby
reset
out=$(sql primary "${OPTS[@]}" "SET max_parallel_maintenance_workers = 0" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')" | tail -1)
check "the load with no worker starts none" test "$out" == 0
check "... and gives the same rows at the same TIDs as the load with the ring" test "$(digest primary)" == "$with"
reset
sql primary "${OPTS[@]}" "SET sift.test_load_publish = 0" "$COPYSQL" >/dev/null
check "the segments published at the default pace: the same rows at the same TIDs" test "$(digest primary)" == "$with"
reset
# the worker keeps one writer over the groups: a load that fits the memory budget of the writer is one segment, which no merge has to rewrite afterwards
sql primary "${OPTS[@]}" "SET sift.test_load_segment_kb = 0" "$COPYSQL" >/dev/null
check "the load with the default budget is one segment" test "$(sql primary "SELECT count(*) FROM sift.table_segments('lt') WHERE kind = 'columns'")" == 1
check "... the same rows at the same TIDs" test "$(digest primary)" == "$with"
reset
sql primary "${OPTS[@]}" "COPY lt FROM '$D/big.csv' CSV" >/dev/null
bigrows=$(rows primary)
check "the load of three times the rows" test "$(sql primary "SELECT count(*) FROM lt")" == "$((3 * ROWS))"
reset

# a default that calls nextval cannot run in the parallel mode of the ring: the groups of that table are written without it
sql primary "CREATE TABLE ls (sid serial, id int, grp int, amount numeric(10,2), pad text) USING sift_table" >/dev/null
out=$(sql primary "${OPTS[@]}" "COPY ls (id, grp, amount, pad) FROM '$D/load.csv' CSV" "SELECT count(*) || ':' || count(DISTINCT sid) || ':' || max(sid) FROM ls" | tail -1)
check "a table with a serial column: the sequence runs in the load, one value per row" test "$out" == "$ROWS:$ROWS:$ROWS"
sql primary "DROP TABLE ls" >/dev/null

# what the backend runs for each row while the ring keeps it in parallel mode must not start a command or a subtransaction: a table with a sift index (its maintenance merges in a
# subtransaction, and failed here with the index half written) or a foreign key loads without the ring
sql primary "CREATE TABLE lk (id int PRIMARY KEY)" "INSERT INTO lk SELECT g FROM generate_series(1, $ROWS) g" \
	"CREATE TABLE lb (id int REFERENCES lk, grp int, amount numeric(10,2), pad text) USING sift_table" "CREATE INDEX lb_bm ON lb USING sift (pad)" >/dev/null
out=$(sql primary "${OPTS[@]}" "COPY lb FROM '$D/load.csv' CSV" 2>&1)
check "a table with a sift index and a foreign key loads without a warning or an error" test -z "$out"
check "... the rows" test "$(sql primary "SELECT count(*) || ':' || sum(amount) FROM lb")" == "$ROWS:$(sql primary "SELECT sum(amount) FROM src")"
check "... the sift index verifies" test -z "$(sql primary "SELECT sift.verify('lb_bm', true)" 2>&1)"
sql primary "DROP TABLE lb, lk" >/dev/null

# the stages: the load waits for the advisory lock of sift.test_phase_gate + the phase (4: a participant writing a group, 5: the backend about to publish the segments taken, 6: the backend about to publish the last)
HOLDER=
hold() { # PHASE
	rm -f "$D/held"
	{ echo "SELECT pg_advisory_lock($((GATE + $1)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	HOLDER=$!
	for _ in $(seq 1200); do [[ -e $D/held ]] && return 0; sleep 0.05; done
	bail "the holder of the gate did not start"
}
release() {
	sql primary "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null 2>&1
	wait "$HOLDER" 2>/dev/null
}
waits() { # PHASE: the pid of the backend (leader, phase 5 and 6) or of a process that writes a group (4: a worker, or the backend while the workers attach) waiting for the lock
	local v kind="backend_type = 'client backend' AND query LIKE 'COPY lt FROM%'"
	[[ $1 == 4 ]] && kind="(backend_type = 'parallel worker' OR (backend_type = 'client backend' AND query LIKE 'COPY lt FROM%'))"
	for _ in $(seq 600); do
		v=$(sql primary "SELECT pid FROM pg_stat_activity WHERE $kind AND wait_event_type = 'Lock' AND pid <> pg_backend_pid() LIMIT 1" 2>/dev/null)
		[[ -z $v ]] || { echo "$v"; return 0; }
		kill -0 "$LOADER" 2>/dev/null || return 1
		sleep 0.05
	done
	return 1
}
start_load() { # PHASE: the load of big.csv, waiting at the phase
	hold "$1"
	sql primary "${OPTS[@]}" "SET sift.test_phase_gate = $GATE" "COPY lt FROM '$D/big.csv' CSV" >"$D/load.out" 2>&1 &
	LOADER=$!
	victim=$(waits "$1") || victim=
}
# after the failure: nothing of the load on either node, the file as that of an empty table, no orphan block, the same load works and equals the twin
recovered() { # LABEL [FILE REFERENCE]
	check "$1: no row, on the primary and the standby" test "$(sql primary "SELECT count(*) FROM lt")/$(sql standby "SELECT count(*) FROM lt")" == 0/0
	sql primary "VACUUM lt" >/dev/null
	diag "$1: blocks in use $(used) (empty table $used0), orphans $(orphans)"
	check "$1: VACUUM leaves the file as small as the empty table's" test "$(used)" -le "$((used0 + 8))"
	check "$1: ... and no orphan block" test "$(orphans)" == 0
	sql primary "${OPTS[@]}" "COPY lt FROM '${2:-$D/big.csv}' CSV" >/dev/null || { not_ok "$1: the load run again failed"; return 1; }
	check "$1: the load run again gives the same rows" test "$(rows primary)" == "${3:-$bigrows}"
	check "$1: ... the standby counts as the primary" same_on_standby
	reset
}
power_loss() { # LABEL PHASE: the power goes while the load waits at the phase
	start_load "$2"
	check "$1: the load waits at the phase" test -n "$victim"
	crash primary
	wait "$LOADER" 2>/dev/null
	release
	if node_start primary && catchup primary standby; then ok "$1: the primary recovers and the standby streams again"; recovered "$1"; else not_ok "$1: the primary recovers"; fi
}
power_loss "power loss while a worker writes the first group" 4
power_loss "power loss with groups written and none published" 5
power_loss "power loss with some groups published and the last not" 6

# a VACUUM, a second COPY and an INSERT while groups are written and not published: the sweep leaves the blocks, the others go on
start_load 5
check "vacuum: the backend is about to publish" test -n "$victim"
check "vacuum: a VACUUM during the load returns" test "$(timeout 120 bash -c ". '$(dirname "$0")/lib.bash'; sql primary 'VACUUM lt' && echo done" 2>&1 | tail -1)" == done
check "an INSERT during the load" test "$(sql primary "INSERT INTO lt VALUES (-1, 0, 0, 'oltp')" "SELECT count(*) FROM lt WHERE id = -1" | tail -1)" == 1
check "a second COPY during the load" test "$(timeout 300 bash -c ". '$(dirname "$0")/lib.bash'; sql primary \"COPY lt FROM '$D/load.csv' CSV\" && echo done" 2>&1 | tail -1)" == done
release
wait "$LOADER"; rc=$?
check "the load that waited went on and ended" test "$rc" -eq 0
check "both loads and the row: the rows" test "$(sql primary "SELECT count(*) FROM lt")" == "$((4 * ROWS + 1))"
check "... no block left over" test "$(sql primary "VACUUM lt" "SELECT sift._test_check('lt')" | tail -1)" == 0
check "... the standby counts as the primary" same_on_standby
reset

# errors: a row that fails after the workers started, a savepoint rolled back after the ring ran
out=$(sql primary "${OPTS[@]}" "COPY lt FROM '$D/bad.csv' CSV" 2>&1)
check "a COPY that fails on a row after the workers started fails" test -n "$out"
recovered "failing row" "$D/load.csv" "$onerows"
out=$(sql primary "${OPTS[@]}" "BEGIN" "SAVEPOINT a" "$COPYSQL" "ROLLBACK TO a" "COPY lt FROM '$D/load.csv' CSV" "COMMIT" "SELECT count(*) FROM lt" | tail -1)
check "a savepoint rolled back after the ring ran: the second load only" test "$out" == "$ROWS"
check "... the rows at the TIDs of a single load" test "$(digest primary | cut -d: -f1)" == "$ROWS"
check "... no block left over after VACUUM" test "$(sql primary "VACUUM lt" "SELECT sift._test_check('lt')" | tail -1)" == 0
reset
echo "1..$NTEST"
finish
