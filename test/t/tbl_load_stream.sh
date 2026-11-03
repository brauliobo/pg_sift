#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A COPY into a sift_table without workers keeps one segment writer between its rows (table/load/load.c): a unit is encoded when it fills, the segment ends at a memory budget
# (sift.test_load_segment_kb) or when the next superblocks are not those after it, and the segments are published when the command ends, so a load leaves as few segments as
# its dictionaries allow and no merge has to rewrite them. With a streaming standby under generic WAL consistency checking: the load has the rows, at the TIDs, of the load with
# workers and the heap twin; a load of more units than one reservation holds (eight) is one segment, and one with a small budget several; a writer that takes the superblocks
# after the first reservation while the load waits ends the segment there, a VACUUM leaves the blocks of the load alone, and a load that fails on its last row leaves nothing.
# SIFT_LS_ROWS (60000, 20000 quick); the units are 128 kB (sift.test_load_unit_bytes) so that these rows fill more than eight of them.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_LS_ROWS 60000 20000)
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
	"CREATE TABLE lt (id int, grp int, amount numeric(10,2), pad text) USING sift_table" >/dev/null || bail "setup failed"
{ head -n $((ROWS - 1)) "$D/load.csv"; echo 'x,1,1,y'; } >"$D/bad.csv"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
wal_consistency_checking = 'generic'"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

SERIAL=("SET max_parallel_maintenance_workers = 0" "SET sift.table_write_buffer_limit = 2048" "SET sift.test_load_unit_bytes = 128" "SET sift.test_par_workers = 0")
RING=("SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET sift.table_write_buffer_limit = 2048" "SET sift.test_load_unit_bytes = 128" "SET sift.test_par_workers = 0")
COPYSQL="COPY lt FROM '$D/load.csv' CSV"
digest() { sql "$1" "SELECT count(*) || ':' || md5(string_agg(id || ':' || ctid::text || ':' || amount, ',' ORDER BY id, ctid)) FROM lt" | tail -1; }
rows() { sql "$1" "SELECT count(*) || ':' || md5(string_agg(id || ':' || grp || ':' || amount || ':' || pad, ',' ORDER BY id, grp, amount, pad)) FROM lt" | tail -1; }
segments() { sql primary "SELECT count(*) || '/' || sum(units) FROM sift.table_segments('lt') WHERE kind = 'columns'"; }
used() { sql primary "SELECT high_water - free_blocks FROM sift.table_space('lt')"; }
orphans() { sql primary "SELECT sift._test_check('lt')"; }
reset() { sql primary "TRUNCATE lt" >/dev/null && sql primary "VACUUM lt" >/dev/null; }
same_on_standby() { catchup primary standby && [[ $(digest standby) == "$(digest primary)" ]] && [[ $(sql standby "SELECT sift._test_check('lt') >= 0") == t ]]; }

used0=$(used)
out=$(sql primary "${RING[@]}" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')" | tail -1)
ring=$(digest primary)
check "the load with workers starts them (the reference for the TIDs)" test "${out:-0}" -gt 0
reset

out=$(sql primary "${SERIAL[@]}" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')" | tail -1)
check "the load with no worker starts none" test "$out" == 0
onerows=$(rows primary)
check "... and gives the rows of the load with workers at the same TIDs" test "$(digest primary)" == "$ring"
check "... the rows of the heap twin" test "$(sql primary "SELECT count(*) || ':' || sum(amount) FROM lt")" == "$(sql primary "SELECT count(*) || ':' || sum(amount) FROM src")"
check "... more units than one reservation holds, in one segment" test "$(segments)" == "1/$(sql primary "SELECT units FROM sift.table_segments('lt') WHERE kind = 'columns'")"
check "... of more than eight units" test "$(sql primary "SELECT units > 8 FROM sift.table_segments('lt') WHERE kind = 'columns'")" == t
check "... the store verifies and no block is left over" test "$(orphans)" == 0
check "the standby replays the same rows at the same TIDs under generic WAL consistency checking" same_on_standby
reset

sql primary "${SERIAL[@]}" "SET sift.test_load_segment_kb = 64" "$COPYSQL" >/dev/null
check "a small budget ends segments: several of them" test "$(sql primary "SELECT count(*) > 1 FROM sift.table_segments('lt') WHERE kind = 'columns'")" == t
check "... the same rows at the same TIDs" test "$(digest primary)" == "$ring"
check "... the store verifies and no block is left over" test "$(orphans)" == 0
check "... the standby counts as the primary" same_on_standby
reset

# the load waits after a quarter of its rows; a writer takes the superblocks after the first reservation, the second one does not follow the segment, and a VACUUM leaves the blocks alone
GATEAT=$((ROWS / 4))
rm -f "$D/reached" "$D/go"
PROG="head -n $GATEAT $D/load.csv; touch $D/reached; while [ ! -e $D/go ]; do sleep 0.05; done; tail -n +$((GATEAT + 1)) $D/load.csv"
sql primary "${SERIAL[@]}" "COPY lt FROM PROGRAM '$PROG' CSV" >"$D/load.out" 2>&1 &
LOADER=$!
for _ in $(seq 1200); do [[ -e $D/reached ]] && break; sleep 0.05; done
check "the load waits for its rows" test -e "$D/reached"
sql primary "INSERT INTO lt VALUES (-1, -1, 0, 'marker')" >/dev/null
sql primary "VACUUM lt" >/dev/null
touch "$D/go"
wait "$LOADER"
check "the load ends without an error" test ! -s "$D/load.out"
check "the rows of the load and the marker are there" test "$(sql primary "SELECT count(*) FROM lt")" == "$((ROWS + 1))"
check "... the rows of the load are those of the heap twin" test "$(sql primary "SELECT count(*) || ':' || sum(amount) FROM lt WHERE id > 0")" == "$(sql primary "SELECT count(*) || ':' || sum(amount) FROM src")"
check "... the marker is found by its key" test "$(sql primary "SELECT count(*) FROM lt WHERE id = -1 AND pad = 'marker'")" == 1
check "... the segment ended where the writer took the superblocks: more than one" test "$(sql primary "SELECT count(*) > 1 FROM sift.table_segments('lt') WHERE kind = 'columns'")" == t
check "... the store verifies and no block is left over" test "$(orphans)" == 0
check "... the standby counts as the primary" same_on_standby
reset

# a row that fails at the end: the segments written are no part of the table
out=$(sql primary "${SERIAL[@]}" "SET sift.test_load_segment_kb = 64" "COPY lt FROM '$D/bad.csv' CSV" 2>&1)
check "a load that fails on its last row reports it" grep -q "invalid input syntax" <<<"$out"
check "... leaves no row, on the primary and the standby" test "$(sql primary "SELECT count(*) FROM lt")/$(sql standby "SELECT count(*) FROM lt")" == 0/0
sql primary "VACUUM lt" >/dev/null
diag "blocks in use $(used) (empty table $used0), orphans $(orphans)"
check "... VACUUM leaves the file as small as the empty table's" test "$(used)" -le "$((used0 + 8))"
check "... and no orphan block" test "$(orphans)" == 0
sql primary "${SERIAL[@]}" "$COPYSQL" >/dev/null
check "... the same load works afterwards, with the same rows (the TIDs of the failed load are not reused)" test "$(rows primary)" == "$onerows"
check "... the standby counts as the primary" same_on_standby

echo "1..$NTEST"
finish
