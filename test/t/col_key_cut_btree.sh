#!/usr/bin/env bash
# A key_cut build that reads the heap through a btree on the key (sift_col build btree.c) keeps what the key ranges give under the conditions the regress test cannot make:
#  - a repeatable-read session older than the build: the btree holds entries of row versions the build's snapshot no longer sees, so the index is not offered to older snapshots
#    (indcheckxmin) and that session answers from the heap as it did before the build
#  - the build by workers, each scanning a range of the keys: every row once, no temporary file, as many workers as the cluster let go
#  - CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY under writers that update, insert and delete the whole time, serial and by workers
# Each build is checked against the heap (col_verify) and the grouped aggregate from the index against the plain plan. SIFT_BB_ROWS (120000, 40000 quick).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_BB_ROWS 120000 40000)
node_init n 2 "autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8
shared_buffers = 64MB"
node_start n || bail "start failed"
sql n "CREATE EXTENSION pg_sift" \
	"CREATE TABLE orders (id int, tenant_id int, created_at int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO orders SELECT g, g % 30, g, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX orders_created_at ON orders (created_at)" \
	"VACUUM (FREEZE, ANALYZE) orders" >/dev/null || bail "setup failed"

SETTINGS=("SET sift.async_maintenance = off" "SET max_parallel_workers_per_gather = 0" "SET sift.test_col_segment_kb = 1500" "SET sift.col_unit_rows = 1200"
	"SET maintenance_work_mem = '512MB'" "SET min_parallel_table_scan_size = 0")
GROUPS_Q="SELECT coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), '') FROM (SELECT tenant_id, count(*) AS n, sum(amount) AS s FROM orders GROUP BY tenant_id) x"
BUILD="CREATE INDEX orders_idx ON orders USING sift_col (id, tenant_id, created_at, amount) WITH (key_cut = 'created_at')"
RESULT="SELECT current_setting('sift.test_col_btree_entries')::int > 0, current_setting('sift.test_col_spill_bytes'), current_setting('sift.test_col_workers')"

answer() { sql n "LOAD 'pg_sift'" "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "$GROUPS_Q" | tail -1; }
verified() { test -z "$(sql n "SELECT sift.col_verify('orders_idx', true)" 2>&1)"; }
valid() { [[ $(sql n "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid = 'orders_idx'::regclass") == t ]]; }
# build WORKERS STATEMENT: the statement under the btree path forced, and what the build reports: "t|SPILL|WORKERS" (t: read through a btree)
build() {
	sql n "${SETTINGS[@]}" "SET max_parallel_maintenance_workers = $1" "SET sift.test_col_build_btree = 1" "$2" "$RESULT" | tail -1
}

# the older snapshot
cat >"$(dir n)/dml.sql" <<SQL
\set id random(1, $ROWS)
\set id2 random(1, $ROWS)
BEGIN;
UPDATE orders SET amount = amount + 1, tenant_id = (tenant_id + 1) % 30 WHERE id = :id;
INSERT INTO orders VALUES ($ROWS + :id, :id % 30, $ROWS + :id, :id % 997, 'w');
DELETE FROM orders WHERE id = :id2 AND id % 11 = 0;
END;
SQL

old_snapshot() { # MODE
	local fifo=$(dir n)/fifo outa=$(dir n)/outa pa
	sql n "DROP INDEX IF EXISTS orders_idx" >/dev/null
	rm -f "$fifo" "$outa"
	mkfifo "$fifo"
	psql -X -At -q -h "$(dir n)/sock" -p "$(port n)" -U postgres -d postgres <"$fifo" >"$outa" 2>&1 &
	pa=$!
	exec 3>"$fifo"
	echo "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 'before ' || count(*) FROM orders;" >&3
	wait_for "grep -q '^before' '$outa'" 30 || bail "the older session did not start"
	sql n "UPDATE orders SET tenant_id = (tenant_id + 3) % 30 WHERE id % 5 = 0" "DELETE FROM orders WHERE id % 7 = 0" >/dev/null
	BUILT=$(sql n "${SETTINGS[@]}" "SET max_parallel_maintenance_workers = 0" "SET sift.test_col_build_btree = $1" "$BUILD" "$RESULT" | tail -1)
	echo "SET sift.enable_agg = on; SELECT 'agg ' || ($GROUPS_Q);" >&3
	echo "SET sift.enable_agg = off; SELECT 'heap ' || ($GROUPS_Q);" >&3
	echo "COMMIT; \\q" >&3
	exec 3>&-
	wait "$pa"
	OLD_AGG=$(grep '^agg ' "$outa" | cut -c5-)
	OLD_HEAP=$(grep '^heap ' "$outa" | cut -c6-)
	rm -f "$fifo" "$outa"
}
old_snapshot 1
check "older snapshot: the build read the heap through the btree" test "${BUILT%%|*}" = t
check "older snapshot: the index is not offered to older snapshots (indcheckxmin)" test "$(sql n "SELECT indcheckxmin FROM pg_index WHERE indexrelid = 'orders_idx'::regclass")" = t
check "older snapshot: the session answers from the index as it does from the heap" test -n "$OLD_HEAP" -a "$OLD_AGG" = "$OLD_HEAP"
check "older snapshot: ... which is not what the rows are now" test "$OLD_HEAP" != "$(answer off)"
check "older snapshot: a new session answers the same from the index and the heap" test "$(answer on)" = "$(answer off)"
check "older snapshot: the index verifies" verified
old_snapshot -1
check "older snapshot (key ranges): the same answers as through the btree" test "$OLD_AGG" = "$OLD_HEAP"

# the build by workers
sql n "DROP INDEX IF EXISTS orders_idx" >/dev/null
BUILT=$(build 4 "$BUILD")
IFS='|' read -r via spill workers <<<"$BUILT"
check "by workers: read through the btree" test "$via" = t
check "by workers: no temporary file" test "$spill" = 0
check "by workers: more than the leader built" test "$workers" -ge 2
check "by workers: the index verifies" verified
check "by workers: the grouped aggregate from the index is the plain plan's" test "$(answer on)" = "$(answer off)"

# concurrent builds under writers
concurrent() { # LABEL WORKERS STATEMENT
	local writers before during
	pgbench_bg n 4 600 -f "$(dir n)/dml.sql"
	writers=$PGBENCH_PID
	wait_for "test \$(sql n \"SELECT count(*) FROM orders WHERE pad = 'w'\") -gt 0" 60 || bail "the writers did not start"
	before=$(sql n "SELECT count(*) FROM orders WHERE pad = 'w'")
	BUILT=$(build "$2" "$3")
	during=$(($(sql n "SELECT count(*) FROM orders WHERE pad = 'w'") - before))
	kill "$writers" 2>/dev/null
	wait "$writers" 2>/dev/null
	check "$1: finishes with the writers inserting while it ran" test "$during" -gt 0
	check "$1: read through the btree" test "${BUILT%%|*}" = t
	check "$1: the index is valid and ready" valid
	check "$1: it verifies against the heap" verified
	check "$1: the grouped aggregate from the index is the plain plan's" test "$(answer on)" = "$(answer off)"
}
sql n "DROP INDEX IF EXISTS orders_idx" >/dev/null
concurrent "CREATE INDEX CONCURRENTLY" 0 "${BUILD/CREATE INDEX/CREATE INDEX CONCURRENTLY}"
concurrent "REINDEX CONCURRENTLY" 0 "REINDEX INDEX CONCURRENTLY orders_idx"
sql n "DROP INDEX orders_idx" >/dev/null
concurrent "CREATE INDEX CONCURRENTLY by workers" 4 "${BUILD/CREATE INDEX/CREATE INDEX CONCURRENTLY}"
echo "1..$NTEST"
finish
