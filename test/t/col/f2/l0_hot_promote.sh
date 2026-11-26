#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Writers that contend for the lock of one partition make it hot: 16 sessions that insert one row at a time into the single partition of a sift_col index and of a sift_table promote its
# micro-slab to a directory of shards, which they then append to without the lock (the flush of a transaction must not hold the lock of the partition around the append, or the append's
# probe of the lock always finds it free), and every committed row is there and checks clean. A backend that exits with FATAL while it holds the entry of a hot partition (a terminated
# flush of the directory) gives the entry back.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
SECS=$(knob SIFT_S107_SECS 3 2)
node_init hp 1 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
max_connections = 40"
node_start hp || bail "start failed"
D=$(dir hp)
sql hp "CREATE EXTENSION pg_sift" \
	"CREATE FUNCTION l0_hot_dirs(regclass) RETURNS int4 LANGUAGE C STRICT AS 'pg_sift', 'sift_test_f2_l0_hot_dirs'" \
	"CREATE FUNCTION l0_hot_hold_fatal(regclass) RETURNS void LANGUAGE C STRICT AS 'pg_sift', 'sift_test_f2_l0_hot_hold_fatal'" \
	"CREATE FUNCTION l0_hot_holders(regclass) RETURNS int4 LANGUAGE C STRICT AS 'pg_sift', 'sift_test_f2_l0_hot_holders'" \
	"CREATE TABLE ti (id bigserial, v int) USING sift_table WITH (autovacuum_enabled = false)" \
	"CREATE TABLE hc (id bigserial, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX hc_idx ON hc USING sift_col (id, v)" \
	"CREATE TABLE hf (id bigserial, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX hf_idx ON hf USING sift_col (id, v)" >/dev/null || bail "setup failed"

out=$(sql hp "SELECT l0_hot_hold_fatal('hf_idx')" 2>&1)
check "a backend that exits with FATAL holding the entry of a hot partition gives it back" wait_for "[[ \$(sql hp \"SELECT l0_hot_holders('hf_idx')\") == 0 ]]" 5

# hot_run TABLE REL: 16 writers of single-row INSERTs into TABLE; HOT the most partitions of REL that were hot at one look, ROWS the rows they committed
hot_run() {
	echo "INSERT INTO $1 (v) VALUES (1);" >"$D/ins_$1.sql"
	pgbench_bg hp 16 "$SECS" -f "$D/ins_$1.sql"
	HOT=0
	while kill -0 "$PGBENCH_PID" 2>/dev/null; do
		h=$(sql hp "SELECT l0_hot_dirs('$2')")
		((h > HOT)) && HOT=$h
	done
	wait "$PGBENCH_PID"
	ROWS=$(awk '/transactions actually processed/{print $NF}' "$D/pgbench.log")
}

hot_run hc hc_idx
diag "sift_col: $ROWS rows, $HOT hot partitions"
check "sift_col: 16 writers of one partition make it hot ($HOT)" test "$HOT" -ge 1
check "sift_col: every committed row is there ($ROWS)" test "$(sql hp "SELECT count(*) FROM hc")" == "$ROWS"
hot_run ti ti
diag "sift_table: $ROWS rows, $HOT hot partitions"
check "sift_table: 16 writers of one partition make it hot ($HOT)" test "$HOT" -ge 1
check "sift_table: every committed row is there ($ROWS)" test "$(sql hp "SELECT count(*) FROM ti")" == "$ROWS"
check "the column scan counts what the heap does" test "$(sql hp "SET sift.enable_agg = on" "SELECT count(*) FROM hc" | tail -1)" == "$(sql hp "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM hc" | tail -1)"
check "sift.check finds nothing in the table and the index" test "$(sql hp "SELECT count(*) FROM sift.check('ti'::regclass)" "SELECT count(*) FROM sift.check('hc_idx'::regclass, true)" | tr '\n' ' ')" == "0 0 "
finish
