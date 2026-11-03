#!/usr/bin/env bash
# The tombstones an UPDATE writes are held in the backend until the commit record, a savepoint or a read of the log (col/supersede.c). What must not follow from a backend or a transaction
# that ends with them held: an entry in the log for a transaction that did not commit (a later commit of the same backend would write what the rollback left), or a sift memory
# context that outlives the backend (the leak gate that finish runs scans the server log for it). Each case ends the transaction or the backend another way, and a later committed statement
# writes only its own entries: a mutant that does not drop the held tombstones at abort (sift_col_tombs_discard in am.c) writes the aborted ones as well.
. "$(dirname "$0")/lib.bash"

plan 9
node_init p 37 "autovacuum = off"
node_start p || bail "start failed"
D=$(dir p)
LOG=$D/postgres.log

sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE th (id int PRIMARY KEY, g int NOT NULL, w int) WITH (autovacuum_enabled = false, fillfactor = 100)" \
	"INSERT INTO th SELECT i, i % 10, 0 FROM generate_series(1, 3000) i" \
	"CREATE INDEX th_c ON th USING sift_col (id, g, w) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) th" >/dev/null || bail "setup failed"

# the same as sql, but a failing statement does not stop the session
sqlc() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -h "$D/sock" -p "$(port p)" -U postgres -d postgres "${@/#/--command=}"; }
aborted() { sql p "SELECT count(*) FROM sift._test_col_log_entries('th_c') WHERE txid_status(xid::text::bigint) = 'aborted'"; }
entries() { sql p "SELECT count(*) FROM sift._test_col_log_entries('th_c')"; }
upd() { echo "UPDATE th SET w = w + 1 WHERE id BETWEEN $1 AND $(($1 + 99))"; }
no_backends() { for _ in $(seq 600); do [[ $(sql p "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()") == 0 ]] && return 0; sleep 0.1; done; bail "the backends did not end (or the server is gone)"; }

# 1: a rollback, then a committed statement of the same backend (100 rows: 101-200)
sqlc "BEGIN" "$(upd 1)" "ROLLBACK" "$(upd 101)"
check "a rolled back UPDATE leaves nothing for the next commit of its backend" [ "$(aborted)" == 0 ]

# 2: an error in the transaction, then the ROLLBACK and a committed statement (501-600)
sqlc "BEGIN" "$(upd 201)" "SELECT 1/0" "ROLLBACK" "BEGIN" "$(upd 301)" "SELECT 1/0" "COMMIT" "$(upd 501)" 2>/dev/null
check "an UPDATE of a transaction that failed leaves nothing in the log" [ "$(aborted)" == 0 ]

# 3: a savepoint rolled back, a statement after it kept (701-800)
sqlc "BEGIN" "SAVEPOINT s" "$(upd 601)" "ROLLBACK TO s" "$(upd 701)" "COMMIT"
check "an UPDATE of a savepoint rolled back leaves nothing in the log" [ "$(aborted)" == 0 ]

# 4: a backend terminated in the transaction with the tombstones held, and a statement of the next backend (901-1000)
PGAPPNAME=victim sqlc "BEGIN" "$(upd 801)" "SELECT pg_sleep(60)" >/dev/null 2>&1 &
victim=$!
for _ in $(seq 100); do [[ $(sql p "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'victim' AND query LIKE 'SELECT pg_sleep%'") == 1 ]] && break; sleep 0.1; done
sql p "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'victim'" >/dev/null
wait "$victim" 2>/dev/null
no_backends
sql p "$(upd 901)" >/dev/null
check "a backend terminated with tombstones held writes none, and the next backend writes only its own" [ "$(aborted)" == 0 ]

# 5: a client that disconnects in the transaction (psql ends with the transaction open), then a statement (1101-1200)
sqlc "BEGIN" "$(upd 1001)" >/dev/null
no_backends
sql p "$(upd 1101)" >/dev/null
check "a client that disconnects in the transaction leaves nothing in the log" [ "$(aborted)" == 0 ]

# 6: one transaction that holds the tombstones of two indexes (1201-1300 and 50 rows of another table)
sql p "CREATE TABLE th2 (id int PRIMARY KEY, g int NOT NULL, w int)" "INSERT INTO th2 SELECT i, i % 10, 0 FROM generate_series(1, 300) i" \
	"CREATE INDEX th2_c ON th2 USING sift_col (id, g, w) WITH (track_changes = 'id')" >/dev/null || bail "second table failed"
sql p "BEGIN" "$(upd 1201)" "UPDATE th2 SET w = w + 1 WHERE id <= 50" "COMMIT" >/dev/null
check "the tombstones of two indexes held by one transaction both reach their logs" [ "$(sql p "SELECT count(*) FROM sift._test_col_log_entries('th2_c')")" == 50 ]

# 7: a backend reads the tombstones it holds (1301-1400)
check "a backend reads the tombstones it holds" [ "$(sql p "BEGIN" "$(upd 1301)" "SELECT count(*) FROM sift._test_col_log_entries('th_c') WHERE xid::text::bigint = txid_current()" "COMMIT")" == 100 ]

# 8: the log holds the entries of the committed statements: 101, 501, 701, 901, 1101, 1201 and 1301, 100 rows each
check "the log holds the entries of the 7 committed statements and no other" [ "$(entries)" == 700 ]

# 9: no sift context of those backends is left at their exit
no_backends
check "no sift memory context outlived a backend" bash -c '! grep -q "sift leak" "$1"' _ "$LOG"

node_stop p
finish
