#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The row locks of a sift_table past the 256 stripes of 64 entries of shared memory: writers on disjoint rows hold more entries than the stripes take, the entries of a full stripe go to chunks of
# dynamic shared memory (sift._test_rowlock_stats), and no writer holds the relation to wait for another. Two writers in one transaction each hold 8,400 rows of two tables (16,800 entries, more than
# the stripes hold) and a gate (an advisory lock a test holds) makes the second one reach a full stripe while the first still has rows to lock: a writer that holds the relation for it waits for the
# intention lock of the other, and the two are a deadlock. Then the rows are exact (a row held is skipped by SKIP LOCKED, NOWAIT refuses it), and the chunks are given back when the transaction
# commits, rolls back to a savepoint, rolls back, prepares, and when its backend exits.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 20
node_init lo 50 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_prepared_transactions = 4
deadlock_timeout = 200ms"
node_start lo || bail "start failed"
D=$(dir lo)
sql lo "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE b (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE c (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO a SELECT g, 0 FROM generate_series(1, 12000) g" \
	"INSERT INTO b SELECT g, 0 FROM generate_series(1, 12000) g" \
	"INSERT INTO c SELECT g, 0 FROM generate_series(1, 12000) g" >/dev/null || bail "setup failed"

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
sql lo "BEGIN" "$(bump a 1 4200)" "$(bump b 1 4200)" "SELECT pg_advisory_lock(7001)" "$(bump a 4201 4800)" "$(bump b 4201 4800)" "COMMIT" >"$D/s1.out" 2>&1 &
s1=$!
wait_for at_gate 60 || bail "the first writer did not reach its gate"
sql lo "BEGIN" "$(bump a 6001 10200)" "$(bump b 6001 10200)" "COMMIT" >"$D/s2.out" 2>&1 &
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
check "the entries of 16,800 rows overflow the stripes into chunks" test "$chunk_entries" -gt 0
check "the rows of the first writer are skipped, in the table and in the chunks (a)" test "$skip_a" -eq 7800
check "the rows of the first writer are skipped, in the table and in the chunks (b)" test "$skip_b" -eq 7800
refused() { [[ $nowait -ne 0 ]] && grep -q 'could not obtain lock' "$D/nowait.out"; }
check "NOWAIT refuses a row that is held" refused
out=$(sums)
check "both writers' rows are changed, nothing else" test "$out" == "9000 9000 0"
out=$(stats "in_chunks || ' ' || chunks")
check "the chunks are given back when the writers commit" test "$out" == "0 0"

out=$(sql lo "BEGIN" "$(bump a 1 5600)" "SAVEPOINT s" "$(bump b 1 5600)" "$(bump c 1 5600)" "SELECT chunks > 0 FROM sift._test_rowlock_stats()" \
	"ROLLBACK TO s" "SELECT chunks FROM sift._test_rowlock_stats()" "ROLLBACK" | paste -sd' ' -)
check "a transaction of 16,800 rows uses chunks, and rolling back to a savepoint gives them back" test "$out" == "t 0"
out=$(stats chunks)
check "the rollback gives back what is left" test "$out" -eq 0
out=$(sums)
check "the rollback changes no row" test "$out" == "9000 9000 0"

start_holder
sql lo "BEGIN" "$(bump a 1 5600)" "$(bump b 1 5600)" "$(bump c 1 5600)" "SELECT pg_advisory_lock(7001)" >"$D/exit.out" 2>&1 &
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
check "the exit changes no row" test "$out" == "9000 9000 0"

sql lo "BEGIN" "$(bump a 1 5600)" "$(bump b 1 5600)" "$(bump c 1 5600)" "PREPARE TRANSACTION 'lo1'" >"$D/prepare.out" 2>&1
out=$(stats "in_chunks || ' ' || chunks")
check "a prepared transaction holds no chunks" test "$out" == "0 0"
sql lo "COMMIT PREPARED 'lo1'" >>"$D/prepare.out" 2>&1
out=$(sums)
check "the prepared transaction changes its rows" test "$out" == "14600 14600 5600"

# A writer holds 18,000 rows, the stripes overflow into chunks: a backend that locks rows of those stripes maps the area once, not once a transaction, and 32 writers of disjoint rows keep the rate
# they have while a writer of 100 rows holds them. The rates are the medians of three windows of each, taken in turns on the same rows (a rollback to the savepoint gives the chunks back)
printf '\\set k 6001 + :client_id * 187 + random(0, 186)\nUPDATE c SET v = v + 1 WHERE k = :k;\n' >"$D/disjoint.pgb"
rate() { pgbench -n -c 32 -j 4 -T 3 -f "$D/disjoint.pgb" -h "$D/sock" -p "$(port lo)" -U postgres -d postgres 2>&1 | sed -n 's/^tps = \([0-9.]*\) .*/\1/p'; }
median() { printf '%s\n' "$@" | sort -g | sed -n 2p; }
sess_open lo ho
sess_say ho "BEGIN; $(bump a 1 100); \\echo small"
wait_for "grep -q '^small\$' $D/ho.out" 60 || bail "the holder of 100 rows did not take them"
base=() ovf=()
for i in 1 2 3; do
	base+=("$(rate)")
	sess_say ho "SAVEPOINT big; $(bump a 101 12000); $(bump b 1 6000); \\echo held$i"
	wait_for "grep -q '^held$i\$' $D/ho.out" 60 || bail "the holder of 18,000 rows did not take them"
	ovf+=("$(rate)")
	sess_say ho "ROLLBACK TO SAVEPOINT big; \\echo small$i"
	wait_for "grep -q '^small$i\$' $D/ho.out" 60 || bail "the holder did not give back the 17,900 rows"
done
sess_say ho "SAVEPOINT big; $(bump a 101 12000); $(bump b 1 6000); \\echo held"
wait_for "grep -q '^held\$' $D/ho.out" 60 || bail "the holder of 18,000 rows did not take them"
sess_open lo wr
sess_say wr "SELECT pg_backend_pid(); BEGIN; $(bump c 1 200); \\echo in"
wait_for "grep -q '^in\$' $D/wr.out" 60 || bail "the second writer did not lock its rows"
pid=$(head -n 1 "$D/wr.out")
maps() { grep -c '/dev/shm/PostgreSQL' "/proc/$pid/maps"; }
mapped_in=$(maps)
sess_say wr "COMMIT; \\echo out"
wait_for "grep -q '^out\$' $D/wr.out" 60 || bail "the second writer did not commit"
out="$mapped_in segments mapped in the transaction, $(maps) after it"
check "a backend keeps the area of the chunks mapped from one transaction to the next" test "$(maps)" -ge "$mapped_in"
base_tps=$(median "${base[@]}") ovf_tps=$(median "${ovf[@]}")
out="32 writers: $base_tps tps (of ${base[*]}) while a writer holds 100 rows, $ovf_tps tps (of ${ovf[*]}) while it holds 18,000 in chunks"
diag "$out"
check "32 writers of disjoint rows keep 60% of their rate while the stripes have chunks" awk -v a="$base_tps" -v b="$ovf_tps" 'BEGIN { exit !(b >= 0.6 * a) }'
sess_say ho "ROLLBACK;"
sess_close ho; sess_close wr
check "no backend died" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" -eq 0
finish
