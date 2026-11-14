#!/usr/bin/env bash
# A DELETE beside CREATE INDEX CONCURRENTLY of a second sift_col index of its table (what pg_repack --only-indexes and REINDEX CONCURRENTLY run): the trigger of the first index asks
# whether another index tracks the same key, and it used to read the metapage of the index being built. The build writes its file past shared buffers, data pages first and the metapage
# last, so the reader found the hole of block 0, kept it in shared buffers, and the build, its writers and its readers failed with "sift index ... is corrupted" (block 0 with lower, upper
# and special 0) although the file was complete. The build is held before its metapage by sift.test_build_gate (a testing build): it waits for advisory lock (0x51F7, 8), which the test
# holds, so no timing is involved.
. "$(dirname "$0")/lib.bash"

plan 7
node_init bu 15 "autovacuum = off"
node_start bu || bail "start failed"
D=$(dir bu)
sql bu "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (id int PRIMARY KEY, grp int, amount numeric(10,2), h text, doc jsonb) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 50, g % 997, md5(g::text), jsonb_build_object('score', g % 11) FROM generate_series(1, 20000) g" \
	"CREATE INDEX t_col ON t USING sift_col (id, grp, amount, h, doc) WITH (paths = 'doc.score float8')" "VACUUM t" >/dev/null || bail "setup failed"

rm -f "$D/locked" "$D/release"
{
	echo "SELECT pg_advisory_lock(20983, 8);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
} | sql bu >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done

sql bu "SET sift.test_build_gate = on" "CREATE INDEX CONCURRENTLY t_copy ON t USING sift_col (id, grp, amount, h, doc) WITH (paths = 'doc.score float8', track_changes = id)" >/dev/null 2>"$D/cic.err" &
cp=$!
for _ in $(seq 400); do
	[[ $(sql bu "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break
	sleep 0.05
done
check "the build is held before its metapage, with its data pages written" test "$(sql bu "SELECT pg_relation_size('t_copy') > 16 * 8192")" == t

sql bu "DELETE FROM t WHERE id % 100 = 1" "UPDATE t SET id = id + 100000 WHERE id % 100 = 2" >"$D/dml.out" 2>&1
check "the writers of the table do not read the index being built" test "$?" == 0
# the sub-select of a window (rank within a group) asks which columns the indexes of the table hold (column_source_reads), and reads the layout of every one of them
LATEST="SELECT count(*) FROM (SELECT id, row_number() OVER (PARTITION BY grp ORDER BY amount DESC, id) rn FROM t) s WHERE rn <= 3"
check "the plans of the readers of the table do not read the index being built" test "$(sql bu "SET sift.enable_agg = on" "$LATEST" 2>&1 | tail -1)" == 150
touch "$D/release"
wait "$hp"
wait "$cp"
check "the build finished: the index is valid and ready" test "$(sql bu "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid = 't_copy'::regclass")" == t
check "... and verifies against the heap" test -z "$(sql bu "SELECT sift.col_verify('t_copy', true)" 2>&1)"
check "the writers go on beside both indexes" test "$(sql bu "DELETE FROM t WHERE id % 100 = 3" "SELECT count(*) FROM t WHERE id % 100 = 3")" == "0"
check "no sift index was reported corrupted" test "$(grep -c 'is corrupted' "$D/postgres.log")" == 0
finish
