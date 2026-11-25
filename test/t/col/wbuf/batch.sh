#!/usr/bin/env bash
# The rows a backend buffers before it writes them to the write buffer of a sift_col index (a statement of many rows, COPY, the rows of a transaction):
# they go in as one WAL record a page, not one a row, before the commit record; a hot standby replays them to the same index; a transaction that
# rolls back, an error inside it, a prepared transaction and kill -9 with rows still buffered leave only the committed rows; writers in parallel (inserts,
# savepoints that roll back, whole rollbacks) leave an index that verifies and answers as the heap does, on the primary and on the standby.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_BATCH_ROWS 20000 5000)
SECS=$(knob SIFT_BATCH_SECS 15 4)
plan 14
node_init primary 2 "wal_level = replica
max_wal_senders = 4
max_prepared_transactions = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
full_page_writes = off
autovacuum = off"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE plain (id int, k int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"CREATE TABLE t (LIKE plain) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX t_idx ON t USING sift_col (id, k, amount)" \
	"CREATE TABLE src AS SELECT g AS id, g % 50 AS k, g % 997 AS amount, md5(g::text) AS pad FROM generate_series(1, $ROWS) g" \
	"COPY src TO '$D/src.csv' WITH (FORMAT csv)" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"

wal() { sql primary "SELECT pg_current_wal_insert_lsn() - '$1'::pg_lsn"; }
lsn() { sql primary "SELECT pg_current_wal_insert_lsn()"; }
count() { sql primary "SELECT count(*) FROM t"; }
# the pushdown and the heap answer the same, and the index verifies
same() {
	local node=$1 a b
	a=$(sql "$node" "SET sift.enable_agg = on" "SET max_parallel_workers_per_gather = 0" "SELECT count(*) || '/' || coalesce(sum(amount), 0) || '/' || coalesce(sum(id), 0) FROM t WHERE k < 60" | tail -1)
	b=$(sql "$node" "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 0" "SET enable_indexscan = off" "SELECT count(*) || '/' || coalesce(sum(amount), 0) || '/' || coalesce(sum(id), 0) FROM t WHERE k < 60" | tail -1)
	[[ $a == "$b" ]] || { diag "$node: pushdown $a, heap $b"; return 1; }
	[[ -z $(sql "$node" "SELECT sift.col_verify('t_idx', true)") ]]
}

same_rows() { [[ $(sql primary "SELECT count(*), sum(id) FROM t") == "$(sql standby "SELECT count(*), sum(id) FROM t")" ]] && same standby; }

# COPY of ROWS rows into a table with the index and into the same table without it: what the index adds, per row
sql primary "CHECKPOINT" >/dev/null
l0=$(lsn)
sql primary "COPY plain FROM '$D/src.csv' WITH (FORMAT csv)" >/dev/null
plain=$(wal "$l0")
sql primary "CHECKPOINT" >/dev/null
l0=$(lsn)
sql primary "COPY t FROM '$D/src.csv' WITH (FORMAT csv)" >/dev/null
withidx=$(wal "$l0")
per=$(((withidx - plain) / ROWS))
diag "COPY of $ROWS rows: $((plain / ROWS)) bytes of WAL a row without the index, $((withidx / ROWS)) with it: $per for the index"
check "COPY: the index adds under 45 bytes of WAL a row (a record a row: 88)" test "$per" -lt 45
sql primary "TRUNCATE plain" >/dev/null
l0=$(lsn)
sql primary "INSERT INTO plain SELECT * FROM src LIMIT 2000" >/dev/null
plain=$(wal "$l0")
l0=$(lsn)
sql primary "INSERT INTO t SELECT * FROM src LIMIT 2000" >/dev/null
withidx=$(wal "$l0")
per=$(((withidx - plain) / 2000))
diag "a statement of 2000 rows: $per bytes of WAL a row for the index"
check "a statement of 2000 rows: the index adds under 45 bytes of WAL a row (a record a row: 73)" test "$per" -lt 45
# an ON CONFLICT statement writes its rows at once: a conflict found after the index entry went in kills the heap row with no abort to drop the entry
sql primary "TRUNCATE plain" >/dev/null
l0=$(lsn)
sql primary "INSERT INTO plain SELECT id + 900000, k, amount, pad FROM src LIMIT 2000 ON CONFLICT DO NOTHING" >/dev/null
plain=$(wal "$l0")
l0=$(lsn)
sql primary "INSERT INTO t SELECT id + 900000, k, amount, pad FROM src LIMIT 2000 ON CONFLICT DO NOTHING" >/dev/null
withidx=$(wal "$l0")
per=$(((withidx - plain) / 2000))
diag "ON CONFLICT: $per bytes of WAL a row for the index"
check "ON CONFLICT: the index adds a record a row (over 60 bytes of WAL, against 25 batched)" test "$per" -gt 60
check "the primary: the pushdown, the heap and the index agree" same primary
catchup primary standby || bail "the standby did not catch up"
check "the standby replays the records to the same index" same standby

# one transaction: rows of every kind, a read in the middle, a savepoint that rolls back, then ROLLBACK and COMMIT
cat >"$D/txn.sql" <<EOF
BEGIN;
INSERT INTO t SELECT id + 100000, k, amount, pad FROM src LIMIT 500;
INSERT INTO t VALUES (200001, 1, 1, 'a'), (200002, 2, 2, 'b');
SELECT count(*) FROM t WHERE k = 1;
SAVEPOINT s;
INSERT INTO t SELECT id + 300000, k, amount, pad FROM src LIMIT 500;
ROLLBACK TO s;
COPY t FROM '$D/src.csv' WITH (FORMAT csv);
INSERT INTO t SELECT id + 400000, k, amount, pad FROM src LIMIT 100;
EOF
before=$(count)
sql primary "$(cat "$D/txn.sql")" "ROLLBACK" >/dev/null
check "ROLLBACK: nothing of the transaction is left" test "$(count)" == "$before"
sql primary "$(cat "$D/txn.sql")" "COMMIT" >/dev/null
check "COMMIT: the committed rows, not the savepoint's" test "$(count)" == "$((before + 500 + 2 + ROWS + 100))"
catchup primary standby
check "the standby holds the same rows and the index verifies" same standby

# kill -9 with rows buffered (an open transaction that has not read, one that has) and two prepared transactions, one committed and one rolled back after the restart
hold() { { echo "$1"; echo "\\! touch '$D/$2'"; echo "\\! sleep 600"; } | sql primary >/dev/null 2>&1 & }
before=$(count)
sql primary "BEGIN" "INSERT INTO t SELECT id + 500000, k, amount, pad FROM src LIMIT 300" "PREPARE TRANSACTION 'to_commit'" >/dev/null
sql primary "BEGIN" "INSERT INTO t SELECT id + 600000, k, amount, pad FROM src LIMIT 300" "PREPARE TRANSACTION 'to_roll_back'" >/dev/null
rm -f "$D/h1" "$D/h2"
hold "BEGIN; INSERT INTO t SELECT id + 700000, k, amount, pad FROM src LIMIT 300; INSERT INTO t SELECT id + 710000, k, amount, pad FROM src LIMIT 300;" h1
hold "BEGIN; INSERT INTO t SELECT id + 800000, k, amount, pad FROM src LIMIT 300; SELECT count(*) FROM t WHERE k = 3; INSERT INTO t SELECT id + 810000, k, amount, pad FROM src LIMIT 300;" h2
until [[ -e $D/h1 && -e $D/h2 ]]; do sleep 0.1; done
crash primary
node_start primary || bail "restart after the crash failed"
check "kill -9 with rows buffered: only the committed rows are there" test "$(count)" == "$before"
sql primary "COMMIT PREPARED 'to_commit'" "ROLLBACK PREPARED 'to_roll_back'" >/dev/null
check "the prepared transactions: the committed one's rows only" test "$(count)" == "$((before + 300))"
check "after recovery the pushdown, the heap and the index agree" same primary

# writers in parallel: inserts of many rows, one by one, a savepoint that rolls back, a whole rollback; a standby reads all the while
cat >"$D/mix.sql" <<'EOF'
\set base random(1000000, 900000000)
\set pick random(1, 100)
BEGIN;
INSERT INTO t SELECT :base + g, g % 50, g % 997, 'w' FROM generate_series(1, 30) g;
\if :pick <= 40
INSERT INTO t VALUES (:base + 1000, 7, 7, 'single');
INSERT INTO t VALUES (:base + 1001, 8, 8, 'single');
COMMIT;
\elif :pick <= 70
SELECT count(*) FROM t WHERE k = 7;
SAVEPOINT s;
INSERT INTO t SELECT :base + 2000 + g, g % 50, g % 997, 'w' FROM generate_series(1, 50) g;
ROLLBACK TO s;
INSERT INTO t SELECT :base + 3000 + g, g % 50, g % 997, 'w' FROM generate_series(1, 20) g;
COMMIT;
\else
INSERT INTO t SELECT :base + 4000 + g, g % 50, g % 997, 'w' FROM generate_series(1, 40) g;
ROLLBACK;
\endif
EOF
echo "SELECT count(*), sum(amount) FROM t WHERE k < 60;" >"$D/read.sql"
pgbench_bg primary 6 "$SECS" -f "$D/mix.sql"
WRITERS=$PGBENCH_PID
pgbench -n -h "$(dir standby)/sock" -p "$(port standby)" -U postgres -c 2 -j 2 -T "$SECS" -f "$D/read.sql" postgres >"$D/reader.log" 2>&1 &
READER=$!
wait "$WRITERS"
wait "$READER"
check "writers and readers ran with no failed transaction" test "$(grep -h -c 'number of failed transactions: 0 ' "$D/pgbench.log" "$D/reader.log" | paste -sd+ - | bc)" == 2
check "the primary after the writers: the pushdown, the heap and the index agree" same primary
catchup primary standby
check "the standby after the writers: the same rows as the primary, and the index agrees" same_rows
finish
