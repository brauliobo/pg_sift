#!/usr/bin/env bash
# The processes that write or vacuum without a client session run the guard of the quarantine in their own transaction states, and a healthy index must see no difference: autovacuum
# (VACUUM of a table whose sift_col and BM25 indexes are healthy, then damaged), the apply worker of a logical subscriber (multi-row transactions, with savepoints on the publisher side
# changing nothing for it), and a promoted standby (multi-row INSERT with a SAVEPOINT in its first transactions). None of them may end a process, and the heap gets every row.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 12
ROWS=$(knob SIFT_TXSTATE_ROWS 3000 1500)
CONF="wal_level = logical
max_wal_senders = 5
max_replication_slots = 5
max_logical_replication_workers = 4
max_worker_processes = 16
autovacuum = on
autovacuum_naptime = 1
synchronous_commit = on
sift.on_corruption = quarantine"
node_init p 80 "$CONF"
node_start p || bail "publisher start failed"
node_init s 81 "$CONF"
node_start s || bail "subscriber start failed"
export PGOPTIONS="-c client_min_messages=warning"

DDL="CREATE EXTENSION pg_sift
CREATE EXTENSION pg_sift_test
CREATE TABLE a (id int PRIMARY KEY, g int NOT NULL, v int, title text) WITH (autovacuum_vacuum_threshold = 10, autovacuum_vacuum_scale_factor = 0, autovacuum_analyze_threshold = 100000000)
CREATE TABLE d (id int PRIMARY KEY, g int NOT NULL, v int, title text)
CREATE INDEX a_c ON a USING sift_col (id, g, v)
CREATE INDEX a_b ON a USING sift (title)
CREATE INDEX d_c ON d USING sift_col (id, g, v) WITH (track_changes = ctid)"
while IFS= read -r q; do sql p "$q" >/dev/null || bail "ddl failed on the publisher: $q"; sql s "$q" >/dev/null || bail "ddl failed on the subscriber: $q"; done <<<"$DDL"
sql p "INSERT INTO a SELECT i, i % 7, i, 'alpha ' || (i % 13) FROM generate_series(1, $ROWS) i" \
	"INSERT INTO d SELECT i, i % 7, i, 'alpha ' || (i % 13) FROM generate_series(1, $ROWS) i" "CREATE PUBLICATION pb FOR TABLE a, d" >/dev/null || bail "publisher setup failed"
sql s "CREATE SUBSCRIPTION sb CONNECTION '$(conninfo p)' PUBLICATION pb" >/dev/null || bail "subscribe failed"

log_clean() { ! grep -qE 'unexpected state|FATAL:  BeginInternal|terminated by signal|server process .* was terminated|TRAP' "$(dir "$1")/postgres.log"; }
sums() { sql "$1" "SELECT (SELECT count(*) || ':' || coalesce(sum(id), 0) FROM a) || ' ' || (SELECT count(*) || ':' || coalesce(sum(id), 0) FROM d)"; }
synced() { [[ $(sums p) == "$(sums s)" ]]; }
check "the subscriber has the initial rows" wait_for synced 120

sql p "INSERT INTO a SELECT i, i % 7, i, 'beta ' || (i % 5) FROM generate_series($((ROWS + 1)), $((ROWS + 400))) i" \
	"BEGIN" "INSERT INTO d SELECT i, i % 7, i, 'beta' FROM generate_series($((ROWS + 1)), $((ROWS + 200))) i" "SAVEPOINT x" "INSERT INTO d VALUES ($((ROWS + 900)), 1, 1, 'y')" "ROLLBACK TO x" \
	"INSERT INTO d VALUES ($((ROWS + 901)), 1, 1, 'z')" "COMMIT" "UPDATE a SET v = v + 1 WHERE id <= 300" "DELETE FROM d WHERE id > $((ROWS - 200)) AND id <= $ROWS" >/dev/null || bail "publisher writes failed"
check "the apply worker writes the multi-row transactions and the rows reach the subscriber" wait_for synced 120
check "the subscriber's indexes are healthy and none is out of service" test "$(sql s "SELECT (SELECT count(*) FROM sift.check('a_c')) + (SELECT count(*) FROM sift.check('a_b')) + (SELECT count(*) FROM sift.check('d_c')) + (SELECT count(*) FROM sift.quarantine)")" == 0
check "the apply worker ended no process" log_clean s

# autovacuum on the subscriber's healthy indexes: a deletion on the publisher, a worker that vacuums the table
sql p "DELETE FROM a WHERE id <= 600" >/dev/null
vacuumed() { [[ $(sql s "SELECT coalesce(autovacuum_count, 0) > 0 FROM pg_stat_all_tables WHERE relid = 'a'::regclass") == t ]]; }
check "autovacuum vacuums a table with healthy sift_col and BM25 indexes" wait_for vacuumed 120
none_out() { [[ $(sql s "SELECT count(*) FROM sift.quarantine") == 0 ]]; }
check "no process of the subscriber ended" log_clean s
check "no index is out of service" none_out
check "the vacuumed indexes verify" test "$(sql s "SELECT (SELECT count(*) FROM sift.check('a_c')) + (SELECT count(*) FROM sift.check('a_b'))")" == 0

# the same with a damaged index on the subscriber: the apply worker keeps applying (the heap has every row), the index leaves service
sql s "SELECT sift._test_poke('d_c', 2, 8176, 9, 2)" "CHECKPOINT" >/dev/null || bail "poke failed"
sql p "INSERT INTO d SELECT i, i % 7, i, 'gamma' FROM generate_series($((ROWS + 1000)), $((ROWS + 1300))) i" "BEGIN" "INSERT INTO d VALUES ($((ROWS + 1400)), 1, 1, 'q'), ($((ROWS + 1401)), 1, 1, 'q')" "COMMIT" >/dev/null || bail "publisher writes failed"
check "the apply worker applies the rows into a table whose index is damaged" wait_for synced 120
check "the apply worker and the server are up and the damaged index is out of service or not yet met" log_clean s

# a promoted standby: the first transactions it runs write several rows after a SAVEPOINT
sql s "SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots" >/dev/null 2>&1 || true
node_clone p c 82 "sift.on_corruption = quarantine"
node_start c || bail "standby start failed"
catchup p c || bail "standby catchup failed"
node_promote c
promoted() { [[ $(sql c "SELECT pg_is_in_recovery()") == f ]]; }
check "the standby is promoted" wait_for promoted 60
promoted_writes() {
	sql c "BEGIN" "INSERT INTO a SELECT i, 1, 1, 'p' FROM generate_series(50000, 50100) i" "SAVEPOINT s" "INSERT INTO a VALUES (50200, 1, 1, 'p')" "ROLLBACK TO s" \
		"INSERT INTO d SELECT i, 1, 1, 'p' FROM generate_series(50000, 50100) i" "COMMIT" >/dev/null 2>&1 &&
		[[ $(sql c "SELECT count(*) FROM a WHERE id BETWEEN 50000 AND 50100") == 101 ]]
}
check "the promoted server commits multi-row transactions with a SAVEPOINT" promoted_writes
finish
