#!/usr/bin/env bash
# The row locks of a sift_table past the 128 stripes of 64 entries of shared memory: writers on disjoint rows hold more entries than the stripes take, the entries of a full stripe go to chunks of
# dynamic shared memory (sift._test_rowlock_stats), and no writer holds the relation to wait for another. Two writers in one transaction each hold 4,200 rows of two tables (8,400 entries, more than
# the stripes hold) and a gate (an advisory lock a test holds) makes the second one reach a full stripe while the first still has rows to lock: a writer that holds the relation for it waits for the
# intention lock of the other, and the two are a deadlock. Then the rows are exact (a row held is skipped by SKIP LOCKED, NOWAIT refuses it), and the chunks are given back when the transaction
# commits, rolls back to a savepoint, rolls back, prepares, and when its backend exits.
. "$(dirname "$0")/lib.bash"

plan 18
node_init lo 50 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_prepared_transactions = 4
deadlock_timeout = 200ms"
node_start lo || bail "start failed"
D=$(dir lo)
sql lo "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (k int, v int) USING sift_table" \
	"CREATE TABLE b (k int, v int) USING sift_table" \
	"CREATE TABLE c (k int, v int) USING sift_table" \
	"INSERT INTO a SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO b SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO c SELECT g, 0 FROM generate_series(1, 6000) g" >/dev/null || bail "setup failed"

stats() { sql lo "SELECT $1 FROM sift._test_rowlock_stats()"; }
sums() { sql lo "SELECT (SELECT sum(v) FROM a) || ' ' || (SELECT sum(v) FROM b) || ' ' || (SELECT sum(v) FROM c)"; }
skipped() { sql lo "SELECT count(*) FROM (SELECT 1 FROM $1 FOR UPDATE SKIP LOCKED) s"; }
bump() { echo "UPDATE $1 SET v = v + 1 WHERE k BETWEEN $2 AND $3"; }
at_gate() { [[ $(sql lo "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; }
start_holder() {
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(7001);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql lo >/dev/null &
	holder=$!
	wait_for "[[ -e $D/locked ]]" 60 || bail "the holder did not take the gate"
}
release_gate() { touch "$D/release"; wait "$holder"; }

start_holder
sql lo "BEGIN" "$(bump a 1 2100)" "$(bump b 1 2100)" "SELECT pg_advisory_lock(7001)" "$(bump a 2101 2400)" "$(bump b 2101 2400)" "COMMIT" >"$D/s1.out" 2>&1 &
s1=$!
wait_for at_gate 60 || bail "the first writer did not reach its gate"
sql lo "BEGIN" "$(bump a 3001 5100)" "$(bump b 3001 5100)" "COMMIT" >"$D/s2.out" 2>&1 &
s2=$!
second_done() { ! kill -0 "$s2" 2>/dev/null || [[ $(sql lo "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND NOT granted") -ge 1 ]]; }
wait_for second_done 60 || bail "the second writer neither finished nor waited"
chunk_entries=$(stats in_chunks)
skip_a=$(skipped a)
skip_b=$(skipped b)
sql lo "SELECT k FROM a WHERE k = 1 FOR UPDATE NOWAIT" >"$D/nowait.out" 2>&1
nowait=$?
release_gate
wait "$s1"; r1=$?
wait "$s2"; r2=$?
check "the first writer commits" test "$r1" -eq 0 -a -z "$(grep ERROR "$D/s1.out")"
check "the second writer commits" test "$r2" -eq 0 -a -z "$(grep ERROR "$D/s2.out")"
check "no deadlock" test "$(grep -c 'deadlock detected' "$D/postgres.log")" -eq 0
check "the entries of 8,400 rows overflow the stripes into chunks" test "$chunk_entries" -gt 0
check "the rows of the first writer are skipped, in the table and in the chunks (a)" test "$skip_a" -eq 3900
check "the rows of the first writer are skipped, in the table and in the chunks (b)" test "$skip_b" -eq 3900
refused() { [[ $nowait -ne 0 ]] && grep -q 'could not obtain lock' "$D/nowait.out"; }
check "NOWAIT refuses a row that is held" refused
out=$(sums)
check "both writers' rows are changed, nothing else" test "$out" == "4500 4500 0"
out=$(stats "in_chunks || ' ' || chunks")
check "the chunks are given back when the writers commit" test "$out" == "0 0"

out=$(sql lo "BEGIN" "$(bump a 1 2800)" "SAVEPOINT s" "$(bump b 1 2800)" "$(bump c 1 2800)" "SELECT chunks > 0 FROM sift._test_rowlock_stats()" \
	"ROLLBACK TO s" "SELECT chunks FROM sift._test_rowlock_stats()" "ROLLBACK" | paste -sd' ' -)
check "a transaction of 8,400 rows uses chunks, and rolling back to a savepoint gives them back" test "$out" == "t 0"
out=$(stats chunks)
check "the rollback gives back what is left" test "$out" -eq 0
out=$(sums)
check "the rollback changes no row" test "$out" == "4500 4500 0"

start_holder
sql lo "BEGIN" "$(bump a 1 2800)" "$(bump b 1 2800)" "$(bump c 1 2800)" "SELECT pg_advisory_lock(7001)" >"$D/exit.out" 2>&1 &
victim=$!
wait_for at_gate 60 || bail "the third writer did not reach its gate"
out=$(stats in_chunks)
check "a writer at its gate holds chunks" test "$out" -gt 0
sql lo "SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted" >/dev/null
wait "$victim" 2>/dev/null
release_gate
out=$(stats chunks)
check "its backend exits and the chunks are given back" test "$out" -eq 0
out=$(sums)
check "the exit changes no row" test "$out" == "4500 4500 0"

sql lo "BEGIN" "$(bump a 1 2800)" "$(bump b 1 2800)" "$(bump c 1 2800)" "PREPARE TRANSACTION 'lo1'" >"$D/prepare.out" 2>&1
out=$(stats "in_chunks || ' ' || chunks")
check "a prepared transaction holds no chunks" test "$out" == "0 0"
sql lo "COMMIT PREPARED 'lo1'" >>"$D/prepare.out" 2>&1
out=$(sums)
check "the prepared transaction changes its rows" test "$out" == "7300 7300 2800"
check "no backend died" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" -eq 0
finish
