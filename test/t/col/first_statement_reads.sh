#!/usr/bin/env bash
# The first statement after the index's pages left shared buffers (a native scan of a table larger than the pool evicted them) reads its blocks in as few requests as it can: the
# segment reader reads the rest of a run of pages in one request (sr_read_range), a unit that waited for the device has the chunks every unit before it read, and the head of the next
# unit, read as one plan (sift_cseg_unit_plan). Counts only, never time: blocks read (pg_stat_io), read
# requests (pg_stat_io reads), the share of the segment's blocks that a get waited for against those the plan or the read-ahead brought in first (sift._test_segment_reads), the blocks
# of the same statement run again. A scratch cluster on tmpfs has no device latency. Format 1: the segment reader, its units of sift.test_col_unit_rows and its counters are Format 1's.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}
# pg_stat_io counts bytes from PostgreSQL 18 (read_bytes); before it, reads of op_bytes (8 kB)
RB=read_bytes; ((PG_MAJOR >= 18)) || RB='reads * op_bytes'

plan 9
node_init fs 29 "sift.test_create_format1 = on
autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 16MB
track_io_timing = off"
node_start fs || bail "start failed"
sql fs "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null 2>&1 || { skip 9 "pg_sift_test is not built"; finish; exit 0; }
# events inserted in random order, a wide row (the measure columns are not read by the statement), units of 400 rows cut by the key column
sql fs "CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int NOT NULL, kind_id int NOT NULL, day date NOT NULL, amount numeric(10,2) NOT NULL,
		m1 float8, m2 float8, m3 float8, m4 float8, m5 float8, m6 float8, m7 float8, m8 float8)" \
	"INSERT INTO ev SELECT row_number() OVER (), e, 1 + (e + g) % 19, date '2024-01-01' + (random() * 900)::int, round((random() * 100)::numeric, 2),
		random(), random(), random(), random(), random(), random(), random(), random()
		FROM generate_series(1, 2000) e, generate_series(1, 50) g ORDER BY random()" \
	"SET sift.test_col_unit_rows = 400" \
	"CREATE INDEX ev_col ON ev USING sift_col (id, dim_a_id, kind_id, day, amount, m1, m2, m3, m4, m5, m6, m7, m8) WITH (key_cut = dim_a_id)" \
	"CREATE TABLE evict (id serial PRIMARY KEY, k int NOT NULL, pad text NOT NULL)" \
	"INSERT INTO evict (k, pad) SELECT (random() * 1000000)::int, repeat('x', 200) FROM generate_series(1, 300000)" \
	"CREATE INDEX evict_k ON evict (k)" \
	"SET synchronous_commit = on" "VACUUM (FREEZE, ANALYZE) ev, evict" >/dev/null || bail "setup failed"

keys() { seq 1 $((2000 / $1)) 2000 | head -n "$1" | paste -sd, -; }
Q() { echo "SELECT kind_id, count(*), sum(amount) FROM ev WHERE dim_a_id IN ($(keys "$1")) AND day BETWEEN '2024-03-01' AND '2025-03-01' GROUP BY 1 ORDER BY 1"; }

# cold N [off]: one fresh backend evicts the index's pages with a native bitmap heap scan of evict (about five pools of pages), then runs the statement for N keys;
# prints the md5 of the rows and then: blocks read, read requests, demand blocks, ahead blocks; then the blocks the same statement reads run again
cold() {
	{
		echo "SET sift.enable_agg = off; SET enable_seqscan = off; SET enable_indexscan = off;"
		echo "SELECT count(*) FROM evict WHERE k < 1000000 \\g /dev/null"
		echo "RESET enable_seqscan; RESET enable_indexscan; SET sift.enable_agg = ${2:-on};"
		echo "SELECT pg_stat_force_next_flush() \\g /dev/null"
		echo "SELECT sum($RB) / 8192 AS b0, sum(reads) AS r0 FROM pg_stat_io WHERE object = 'relation' \\gset"
		echo "SELECT sift._test_segment_reads(true) \\g /dev/null"
		echo "\\echo ROWS"
		echo "$(Q "$1");"
		echo "\\echo STATS"
		echo "SELECT pg_stat_force_next_flush() \\g /dev/null"
		echo "SELECT ((SELECT sum($RB) / 8192 FROM pg_stat_io WHERE object = 'relation') - :b0)::bigint, ((SELECT sum(reads) FROM pg_stat_io WHERE object = 'relation') - :r0)::bigint, demand_blocks, ahead_blocks FROM sift._test_segment_reads();"
		echo "\\echo STEADY"
		for _ in 1 2; do
			echo "SELECT pg_stat_force_next_flush() \\g /dev/null"
			echo "SELECT sum($RB) / 8192 AS b1 FROM pg_stat_io WHERE object = 'relation' \\gset"
			echo "$(Q "$1") \\g /dev/null"
			echo "SELECT pg_stat_force_next_flush() \\g /dev/null"
			echo "SELECT ((SELECT sum($RB) / 8192 FROM pg_stat_io WHERE object = 'relation') - :b1)::bigint;"
		done
	} | sql fs
}
rowsum() { sed -n '/^ROWS$/,/^STATS$/p' <<<"$1" | "$REPO/scripts/sha256.sh"; }
stats() { sed -n '/^STATS$/{n;p}' <<<"$1"; }
# the blocks the second run of the statement read (the first one reads the pages the statistics queries need, which the eviction took out too)
steady() { sed -n '/^STEADY$/,$p' <<<"$1" | tail -n 1; }

for n in 25 200; do
	r=$(cold "$n"); o=$(cold "$n" off)
	IFS='|' read -r blocks req demand ahead <<<"$(stats "$r")"
	diag "$n keys: blocks=$blocks requests=$req demand=$demand ahead=$ahead"
	check "$n keys: the rows are those of the normal plan" test "$(rowsum "$r")" == "$(rowsum "$o")"
	check "$n keys: the statement reads blocks from the OS" test "$blocks" -gt 100
	# the units that waited for the device are read as one plan each: what a get waits for is a small part of the blocks (about half of them before the plan)
	check "$n keys: at most two fifths of the segment's blocks are waited for" test $((demand * 5)) -le $(((demand + ahead) * 2))
	# the baseline of this table: 489 blocks and 368 requests at 200 keys; the plan reads no block that the statement does not ask for
	# (before PostgreSQL 18 pg_stat_io counts a read per block, so the requests equal the blocks and say nothing)
	if [[ $n -lt 200 ]]; then :
	elif ((PG_MAJOR >= 18)); then check "$n keys: no more than 465 blocks and 420 requests" test "$blocks" -le 465 -a "$req" -le 420
	else check "$n keys: no more than 465 blocks" test "$blocks" -le 465; fi
	check "$n keys: the same statement run again reads nothing" test "$(steady "$r")" == 0
done

finish
