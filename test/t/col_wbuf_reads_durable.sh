#!/usr/bin/env bash
# What the reads of the server paid for the open write buffer (rows decoded) and for the marks of the tombstone log is kept in the metapage of the index (col/wbuf/reads.c): a restart, a crash
# and a backend that ends keep it, a server with more indexes than the table of the shared memory had slots counts each of them, and a rule that is met does not make every read write.
. "$(dirname "$0")/lib.bash"
plan 8

node_init rd 58 "autovacuum = off
sift.col_auto_merge = off
sift.async_maintenance = off"
node_start rd || bail "start failed"
sql rd "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "SET client_min_messages = warning" \
	"CREATE TABLE wr (id int PRIMARY KEY, k int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO wr SELECT g, g % 50, g % 100 FROM generate_series(1, 3000) g" \
	"CREATE INDEX wr_idx ON wr USING sift_col (k, amount)" "VACUUM (FREEZE, ANALYZE) wr" \
	"INSERT INTO wr SELECT g, g % 50, g % 100 FROM generate_series(3001, 33000) g" \
	"CREATE TABLE mk (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off, fillfactor = 60)" \
	"INSERT INTO mk SELECT i, i % 10, i FROM generate_series(1, 20000) i" \
	"CREATE INDEX mk_idx ON mk USING sift_col (id, g, v) WITH (track_changes = 'id')" "VACUUM (FREEZE, ANALYZE) mk" \
	"DELETE FROM mk WHERE id % 50 = 7" >/dev/null || bail "setup failed"

# a session that reads the buffer decodes its rows and ends
WR="SELECT k, count(*), sum(amount) FROM wr GROUP BY 1"
MK="SELECT g, count(*), sum(v) FROM mk GROUP BY 1"
read_wr() { sql rd "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "$WR" >/dev/null; }
read_mk() { sql rd "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "$MK" >/dev/null; }
decoded() { sql rd "SELECT decoded FROM sift._test_col_wbuf_reads('${1:-wr_idx}')" | tail -n 1; }
marks() { sql rd "SELECT sift._test_col_marks_paid('mk_idx')" | tail -n 1; }
# the WAL writer has the record in the log within wal_writer_delay (200 ms): a crash after that keeps it
settle() { sleep 1; }

read_wr; read_mk
D1=$(decoded); M1=$(marks)
check "a session that read the buffer and ended: the rows it decoded are seen by another session" test "$D1" -gt 0
check "the same for the instructions of the marks of the log" test "$M1" -gt 0
settle
node_stop rd; node_start rd || bail "restart failed"
check "a clean restart keeps both sums" test "$(decoded)" -eq "$D1" -a "$(marks)" -eq "$M1"
settle
crash rd; node_start rd || bail "restart after the crash failed"
check "a crash keeps both sums" test "$(decoded)" -eq "$D1" -a "$(marks)" -eq "$M1"

# more indexes than the 64 slots of the table of the shared memory: each one counts
for i in $(seq 1 70); do
	sql rd "SET client_min_messages = warning" "CREATE TABLE q$i (id int PRIMARY KEY, k int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off)" \
		"INSERT INTO q$i SELECT g, g % 5, g FROM generate_series(1, 200) g" "CREATE INDEX q${i}_idx ON q$i USING sift_col (k, amount)" "VACUUM (FREEZE, ANALYZE) q$i" \
		"INSERT INTO q$i SELECT g, g % 5, g FROM generate_series(201, 2000) g" >/dev/null || bail "index $i failed"
done
sql rd "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "SET sift.agg_force = on" \
	"$(for i in $(seq 1 70); do echo "SELECT k, count(*) FROM q$i GROUP BY 1;"; done)" >/dev/null
out=$(sql rd "$(for i in $(seq 1 70); do echo "SELECT $i, decoded FROM sift._test_col_wbuf_reads('q${i}_idx');"; done)" | awk -F'|' '$2 == 0 {print $1}' | tr '\n' ' ')
check "70 indexes were read once each: every one has its rows" test -z "$out"
check "the first index still has its sum" test "$(decoded)" -eq "$D1"

# once the promotion is due the sum is not written again: the reads of 30 sessions that follow write no record
due() { sql rd "SELECT due FROM sift._test_col_wbuf_reads('wr_idx')" | tail -n 1; }
for i in 1 2 3; do read_wr; done
check "three more sessions decoded the buffer: its promotion is due" test "$(due)" == t
L0=$(sql rd "SELECT pg_current_wal_insert_lsn()" | tail -n 1)
for i in $(seq 1 30); do read_wr; done
W=$(sql rd "SELECT pg_current_wal_insert_lsn() - '$L0'::pg_lsn" | tail -n 1)
check "30 sessions that read a buffer whose promotion is due wrote under 4 KB of WAL" test "$W" -lt 4096
finish
