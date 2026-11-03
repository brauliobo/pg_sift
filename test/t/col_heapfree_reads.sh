#!/usr/bin/env bash
# The tombstone log of a heap sift_col index with track_updates in the reads of an index scan, an index-only scan and a bitmap scan. The log only ever hides an
# entry that a transaction visible to the snapshot ended; every entry returned is still checked by the heap. Three tables get the same statements: a (track_updates), b (track_updates and
# track_deletes) and c (track_updates = off, the control: the default is auto). Every read is compared with a sequential scan of the same table and with the other tables; the hidden-entry counter says the log
# was used (a, b), not used (c), and not used for what is not committed before the snapshot: an older snapshot, a transaction in progress (prepared), a rolled back transaction
# and a subtransaction. Also on a hot standby that replays the log, after VACUUM gave the addresses to new rows, with the triggers of track_deletes disabled, enabled again and
# recreated, and on the subscriber of a real logical replication, which is no place for track_deletes: the apply worker fires no statement trigger.
. "$(dirname "$0")/lib.bash"

plan 35
ROWS=$(knob SIFT_HF_ROWS 3000 1500)
CONF="wal_level = logical
max_prepared_transactions = 5
max_wal_senders = 5
max_replication_slots = 5
autovacuum = off
synchronous_commit = on
max_logical_replication_workers = 4
max_worker_processes = 16"
node_init p 26 "$CONF"
node_start p || bail "start failed"
D=$(dir p)

for t in a b c; do
	sql p "CREATE TABLE $t (id int PRIMARY KEY, g int NOT NULL, v int, w int) WITH (autovacuum_enabled = false, fillfactor = 70)" \
		"INSERT INTO $t SELECT i, i % 10, i, 0 FROM generate_series(1, $ROWS) i" >/dev/null || bail "setup $t failed"
done
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE INDEX a_c ON a USING sift_col (id, g, v) WITH (track_updates = 'id')" \
	"CREATE INDEX b_c ON b USING sift_col (id, g, v) WITH (track_updates = 'id', track_deletes = on)" \
	"CREATE INDEX c_c ON c USING sift_col (id, g, v) WITH (track_updates = 'off')" "VACUUM (FREEZE, ANALYZE) a, b, c" >/dev/null || bail "index failed"
node_clone p s 27 "max_prepared_transactions = 5
max_worker_processes = 16"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

# the subquery keeps the aggregate out of the column source (SiftAgg): the rows come from the index scan, the index-only scan or the bitmap scan
QF() { echo "SELECT md5(string_agg(id || ':' || g || ':' || v || ':' || w, ',' ORDER BY id)) FROM (SELECT * FROM $1 WHERE g IN (3, 4) AND v BETWEEN 1 AND 1500 OFFSET 0) s"; }
QI() { echo "SELECT md5(string_agg(id || ':' || g || ':' || v, ',' ORDER BY id)) FROM (SELECT id, g, v FROM $1 WHERE g IN (3, 4) AND v BETWEEN 1 AND 1500 OFFSET 0) s"; }
MODE_idx="SET enable_seqscan = off; SET enable_bitmapscan = off"
MODE_bmp="SET enable_seqscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off"
MODE_heap="SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_bitmapscan = off"
# rd NODE TABLE MODE QUERYFN: the answer; hid: the answer and the entries the log hid for the read (the counter of the session, reset first)
rd() { local m=MODE_$3; sql "$1" "${!m}" "$($4 "$2")"; }
hid() { local m=MODE_$3; sql "$1" "SELECT sift._test_col_log_hidden(true)" "${!m}" "$($4 "$2")" "SELECT sift._test_col_log_hidden()" | tail -2 | tr '\n' ' ' | sed 's/ $//'; }
# same NODE TABLE: the three scans, the full row and the covered columns, all equal the sequential scan of the table
same() {
	local ref m q
	for q in QF QI; do
		ref=$(rd "$1" "$2" heap $q)
		for m in idx bmp; do [[ $(rd "$1" "$2" $m $q) == "$ref" ]] || { diag "$1 $2 $m $q differs from the heap"; return 1; }; done
	done
}
sameall() { local t; for t in a b c; do same "$1" $t || return 1; done; }
agree() { [[ $(rd p a heap QF) == $(rd p b heap QF) && $(rd p b heap QF) == $(rd p c heap QF) && $(rd p a heap QI) == $(rd p c heap QI) ]]; }

# the plans are the ones that read the index: a test of the log that planned a b-tree or a sequential scan would prove nothing
planhas() { local m=MODE_$3; sql "$1" "${!m}" "EXPLAIN (COSTS OFF) $($4 "$2")" | grep -q "$5"; }
plans_ok() { planhas p a idx QF "Index Scan using a_c" && planhas p a idx QI "Index Only Scan using a_c" && planhas p a bmp QF "Bitmap Index Scan on a_c"; }
check "the plans: index scan, index-only scan and bitmap scan of the column index" plans_ok
check "no tombstone yet: every scan of every table is the heap's" sameall p
BASE_A=$(rd p a heap QF); BASE_AI=$(rd p a heap QI)
check "the three tables hold the same rows" agree

# a snapshot older than the statements below, kept open until released
{ cat <<EOF
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM a;
\\! touch '$D/locked'; while [ ! -e '$D/release' ]; do sleep 0.05; done
\\o $D/held.out
$MODE_idx;
SELECT sift._test_col_log_hidden(true);
$(QF a);
$(QI a);
SELECT sift._test_col_log_hidden();
$MODE_bmp;
$(QF b);
SELECT sift._test_col_log_hidden();
$MODE_heap;
$(QF a);
\\o
COMMIT;
EOF
} | sql p >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done

for t in a b c; do
	sql p "UPDATE $t SET v = v + 1 WHERE id % 7 = 0" \
		"UPDATE $t SET w = w + 1 WHERE id % 11 = 0" \
		"DELETE FROM $t WHERE id % 13 = 0" \
		"UPDATE $t SET id = id + 100000 WHERE id % 17 = 0" \
		"MERGE INTO $t USING (SELECT id FROM $t WHERE id % 19 = 0) s ON $t.id = s.id WHEN MATCHED AND $t.id % 2 = 0 THEN DELETE WHEN MATCHED THEN UPDATE SET v = $t.v + 5" \
		"BEGIN; UPDATE $t SET v = v + 100 WHERE id % 23 = 0; DELETE FROM $t WHERE id % 29 = 0; ROLLBACK" \
		"BEGIN; SAVEPOINT s; UPDATE $t SET v = v + 200 WHERE id % 31 = 0; ROLLBACK TO s; UPDATE $t SET v = v + 300 WHERE id % 37 = 0; COMMIT" >/dev/null || bail "dml $t failed"
done
touch "$D/release"
wait "$hp"
check "a snapshot older than the commits: the log hides nothing (no entry of it) and the rows are the old ones" \
	test "$(grep -c "^0$" "$D/held.out")" == 3 -a "$(grep -c "$BASE_A" "$D/held.out")" == 3
check "the same snapshot, covered columns, are the old ones too" test "$(grep -c "$BASE_AI" "$D/held.out")" == 1

check "after the statements every scan of every table is the heap's (a: updates in the log only; b: deletes and key changes too; c: none)" sameall p
check "the three tables hold the same rows" agree
HA=$(hid p a idx QF); HB=$(hid p b idx QF); HC=$(hid p c idx QF)
check "the control table hides nothing" test "${HC##* }" == 0
check "a hides the entries its updates replaced" test "${HA##* }" -gt 0
check "b hides more: the deletes it heard (a hears the key changes too)" test "${HB##* }" -gt "${HA##* }"
check "the index-only and the bitmap scans hide too" test "$(hid p a bmp QF | awk '{print $NF}')" -gt 0 -a "$(hid p a idx QI | awk '{print $NF}')" -gt 0
check "the hiding is the log's: switched off, nothing is hidden and the answer is the same" \
	test "$(sql p "SET sift.test_no_log_hide = on" "SELECT sift._test_col_log_hidden(true)" "$MODE_idx" "$(QF a)" "SELECT sift._test_col_log_hidden()" | tail -2 | tr '\n' ' ' | sed 's/ $//')" == "$(rd p a heap QF) 0"

# the channel of b: a trigger disabled, the deletes unheard, the answers still the heap's, then a new baseline
sql p "ALTER TABLE b DISABLE TRIGGER ALL" "DELETE FROM b WHERE id % 41 = 0" "UPDATE b SET id = id + 200000 WHERE id % 43 = 0" "ALTER TABLE b ENABLE TRIGGER ALL" "DO \$\$ DECLARE n text; BEGIN FOR n IN SELECT tgname FROM pg_trigger WHERE tgrelid = 'b'::regclass AND tgname LIKE 'sift_track%' LOOP EXECUTE format('ALTER TABLE b ENABLE ALWAYS TRIGGER %I', n); END LOOP; END \$\$" >/dev/null
for t in a c; do sql p "DELETE FROM $t WHERE id % 41 = 0" "UPDATE $t SET id = id + 200000 WHERE id % 43 = 0" >/dev/null; done
check "the triggers of b were disabled for deletes: not complete any more, and the reads are the heap's" test "$(sql p "SELECT heap_free FROM sift.col_tracking('b_c')")" == f
check "b reads as the control table" test "$(rd p b idx QF)" == "$(rd p c heap QF)"
sql p "ALTER INDEX b_c RESET (track_deletes)" "ALTER INDEX b_c SET (track_deletes = on)" >/dev/null
check "recreated triggers, new baseline: complete again, and the reads are the heap's" test "$(sql p "SELECT heap_free FROM sift.col_tracking('b_c')")" == t -a "$(rd p b bmp QF)" == "$(rd p c heap QF)"

# a transaction in progress hides nothing; committed, it does
HA=$(hid p a idx QF)   # the key changes above reached a too (its UPDATE trigger), so the count to hold is the one now
sql p "SELECT sift._test_col_log_hidden(true)" >/dev/null
for t in a b; do sql p "BEGIN; UPDATE $t SET v = v + 1000 WHERE id % 53 = 0; DELETE FROM $t WHERE id % 59 = 0; PREPARE TRANSACTION 'hf_$t'" >/dev/null; done
sql p "UPDATE c SET v = v + 1000 WHERE id % 53 = 0" "DELETE FROM c WHERE id % 59 = 0" >/dev/null
H0=$(hid p a idx QF)
check "prepared, not committed: nothing of it is hidden, the reads are the old rows" test "${H0##* }" == "${HA##* }" -a "$(rd p a idx QF)" == "$(rd p a heap QF)"
sql p "COMMIT PREPARED 'hf_a'" "COMMIT PREPARED 'hf_b'" >/dev/null
H1=$(hid p a idx QF)
check "committed: its entries are hidden, the reads are the heap's" test "${H1##* }" -gt "${H0##* }" -a "$(rd p a idx QF)" == "$(rd p a heap QF)"
check "all tables again alike" sameall p

# the standby replays the log and the heap
catchup p s || bail "standby catchup failed"
check "standby: every scan is the heap's, and the log is used on it" sameall s
check "standby answers as the primary" test "$(rd s a idx QF)" == "$(rd p a heap QF)" -a "$(rd s b bmp QI)" == "$(rd p b heap QI)"
check "standby hides entries" test "$(hid s a idx QF | awk '{print $NF}')" -gt 0

# VACUUM drops the tombstones with the entries and the heap gives the addresses to new rows, which the log must not hide
E0=$(sql p "SELECT count(*) FROM sift._test_col_log_entries('a_c')")
sql p "VACUUM a, b, c" >/dev/null
E1=$(sql p "SELECT count(*) FROM sift._test_col_log_entries('a_c')")
check "VACUUM dropped the tombstones of the entries it removed" test "$E1" -lt "$E0"
for t in a b c; do sql p "INSERT INTO $t SELECT i, i % 10, i, 7 FROM generate_series($((ROWS + 1000)), $((ROWS + 1000 + ROWS / 2))) i" "UPDATE $t SET v = v + 1 WHERE id % 61 = 0" "DELETE FROM $t WHERE id % 67 = 0" >/dev/null; done
E2=$(sql p "SELECT count(*) FROM sift._test_col_log_entries('a_c')")
diag "log entries of a: $E0 before VACUUM, $E1 after, $E2 after the new statements"
check "the new statements wrote tombstones again" test "$E1" -lt "$E2"
check "the index verifies" test "$(sql p "SELECT sift.col_verify('a_c')" 2>&1)" == ""
check "new rows at freed addresses are returned" sameall p
check "same count by index and heap on a" test "$(rd p a idx QF)" == "$(rd p a heap QF)"
agree && ok "a, b, c alike after VACUUM and new rows" || not_ok "a, b, c alike after VACUUM and new rows"
catchup p s || bail "standby catchup failed"
check "standby after VACUUM and new rows: the heap's" sameall s

# a table without track_updates: the setting does nothing and no log exists
check "c has no log" test "$(sql p "SELECT count(*) FROM sift.col_segments('c_c') WHERE kind = 'tombstones'")" == 0

# logical replication: the subscriber table is written by the apply worker, which fires no statement trigger
node_init pub 28 "wal_level = logical
max_replication_slots = 5
max_wal_senders = 5"
node_start pub || bail "pub start failed"
node_init sub 29 "max_logical_replication_workers = 4
max_worker_processes = 16"
node_start sub || bail "sub start failed"
sql pub "CREATE TABLE r (id int PRIMARY KEY, g int NOT NULL, v int, w int)" "CREATE TABLE r2 (id int PRIMARY KEY, g int NOT NULL, v int, w int)" \
	"INSERT INTO r SELECT i, i % 10, i, 0 FROM generate_series(1, $ROWS) i" "INSERT INTO r2 SELECT i, i % 10, i, 0 FROM generate_series(1, $ROWS) i" \
	"CREATE PUBLICATION pb FOR TABLE r, r2" >/dev/null || bail "pub setup failed"
sql sub "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE r (id int PRIMARY KEY, g int NOT NULL, v int, w int)" "CREATE TABLE r2 (id int PRIMARY KEY, g int NOT NULL, v int, w int)" \
	"CREATE INDEX r_c ON r USING sift_col (id, g, v) WITH (track_updates = 'id')" \
	"CREATE INDEX r2_c ON r2 USING sift_col (id, g, v) WITH (track_updates = 'id', track_deletes = on)" >/dev/null || bail "sub setup failed"
check "before the subscription b-like table with track_deletes is complete" test "$(sql sub "SELECT heap_free FROM sift.col_tracking('r2_c')")" == t
sql sub "CREATE SUBSCRIPTION sb CONNECTION '$(conninfo pub)' PUBLICATION pb" >/dev/null || bail "subscription failed"
synced() { for _ in $(seq 300); do [[ $(sql sub "SELECT count(*) FROM pg_subscription_rel WHERE srsubstate <> 'r'") == 0 && $(sql sub "SELECT count(*) FROM r") == $(sql pub "SELECT count(*) FROM r") && $(sql sub "SELECT count(*) FROM r2") == $(sql pub "SELECT count(*) FROM r2") ]] && return 0; sleep 0.1; done; return 1; }
synced || bail "initial sync failed"
check "once subscribed, track_deletes is not trusted: heap_free is false" test "$(sql sub "SELECT heap_free FROM sift.col_tracking('r2_c')")" == f
check "and CREATE INDEX .. track_deletes on a subscribed table is refused" \
	test "$(sql sub "CREATE INDEX r_c2 ON r USING sift_col (id, g, v) WITH (track_updates = 'id', track_deletes = on)" 2>&1 | grep -c 'target of a logical replication subscription')" == 1
sql pub "UPDATE r SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM r WHERE id % 13 = 0" "UPDATE r SET id = id + 100000 WHERE id % 17 = 0" \
	"UPDATE r2 SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM r2 WHERE id % 13 = 0" "UPDATE r2 SET id = id + 100000 WHERE id % 17 = 0" >/dev/null
synced || bail "replication of the changes failed"
check "subscriber: the apply worker's updates, deletes and key changes read as the heap" same sub r
check "subscriber with track_deletes configured: the unheard deletes read as the heap" same sub r2
check "subscriber answers as the publisher" test "$(rd sub r idx QF | tr -d ' ')" == "$(sql pub "$(QF r)")"

finish
