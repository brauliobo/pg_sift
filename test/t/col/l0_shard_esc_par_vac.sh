#!/usr/bin/env bash
# The row locks of a prepared transaction, a fast default after VACUUM, the writers of one partition of a UNIQUE index, a parallel VACUUM and the rows a transaction drops from its own
# stage: a prepared transaction keeps the rows it locked from a session whose earlier statement found none prepared, a fast default that a part does not hold (NaN) is still read after
# VACUUM rewrote the part, a key that another transaction wrote into the slab is waited for, deletes and VACUUM under 16 writers lose no row, VACUUM (PARALLEL 2) of a heap with sift_col
# indexes of both formats completes, and the rows of the same transaction in a table of element columns stay right. The errors of the sharded directory are test/unit/col/f2/shard_dir.c.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init p 2 "autovacuum = off
shared_preload_libraries = 'pg_sift'
max_connections = 80
max_prepared_transactions = 8
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 4"
node_start p || bail "start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions"
D=$(dir p)
said() { grep -q "^$2\$" "$D/$1.out"; }
says() { wait_for "said $1 $2" 30; }

# a fast default whose cell is an exception of the numeric column: the part VACUUM rewrites keeps it
sql p "CREATE TABLE nan_t (id int, v int) USING sift_table" "INSERT INTO nan_t SELECT i, i FROM generate_series(1, 1000) i" \
	"ALTER TABLE nan_t ADD COLUMN x numeric(10,2) DEFAULT 'NaN', ADD COLUMN y numeric(10,2) DEFAULT 1.25" "DELETE FROM nan_t WHERE id <= 300" "VACUUM nan_t" >/dev/null
out=$(sql p "SELECT count(*) || ' ' || count(x) || ' ' || min(x::text) || ' ' || sum(y) FROM nan_t")
check "ADD COLUMN ... DEFAULT 'NaN': every row still reads NaN after VACUUM rewrote its part" test "$out" == "700 700 NaN 875.00"

# 2PC: B's first statement (a row lock its savepoint gives back) finds no prepared transaction; A then prepares with a row locked (PREPARE holds its partition); B's update of that row waits
sql p "CREATE TABLE tp (id int, v int) USING sift_table" "INSERT INTO tp SELECT i, 0 FROM generate_series(1, 100) i" >/dev/null
sess_open p sb
sess_say sb "BEGIN; SAVEPOINT s; SELECT id FROM tp WHERE id = 2 FOR UPDATE; ROLLBACK TO s; \\echo b1"
says sb b1
sql p "BEGIN" "SELECT id FROM tp WHERE id = 1 FOR UPDATE" "PREPARE TRANSACTION 'p1'" >/dev/null
sess_say sb "SET lock_timeout = '1s'; UPDATE tp SET v = v + 1 WHERE id = 1; \\echo b2" # timeout-ok: waits on a row lock held by a prepared transaction
says sb b2
sess_say sb "ROLLBACK;"
sess_close sb
out=$(cat "$D/sb.out")
check "a row of a prepared transaction is not updated by a session whose earlier statement found none prepared" grep -q "lock timeout" <<<"$out"
sql p "ROLLBACK PREPARED 'p1'" >/dev/null

# 16 writers of the one partition of a UNIQUE index, then a transaction whose stage flushes its rows to the slab while it is open
sql p "CREATE TABLE u (id bigint, pad text)" "CREATE SEQUENCE useq" "CREATE UNIQUE INDEX u_c ON u USING sift_col (id)" >/dev/null
echo "INSERT INTO u VALUES (nextval('useq'), 'x');" >"$D/ins.sql"
pgbench_bg p 16 3 -f "$D/ins.sql"
wait "$PGBENCH_PID" || bail "pgbench: $(tail -n 5 "$D/pgbench.log")"
sess_open p ua
sess_say ua "SET work_mem = '64kB'; BEGIN; INSERT INTO u SELECT -i, repeat('y', 40) FROM generate_series(1, 3000) i; \\echo a1"
says ua a1
out=$(sql p "SET lock_timeout = '1s'" "INSERT INTO u VALUES (-1, 'dup')" 2>&1) # timeout-ok: waits on an open transaction holding the unique key
sess_say ua "ROLLBACK;"
sess_close ua
check "UNIQUE: a key another transaction wrote into the slab is waited for, not taken again" grep -qE "lock timeout|duplicate key" <<<"$out"

# deletes and VACUUM while 16 writers go on: no row is lost
pgbench_bg p 16 4 -f "$D/ins.sql"
for _ in 1 2 3 4 5 6; do sql p "DELETE FROM u WHERE id % 7 = 0" "VACUUM u" >/dev/null || bail "delete and vacuum"; done
wait "$PGBENCH_PID" || bail "pgbench: $(tail -n 5 "$D/pgbench.log")"
sql p "DELETE FROM u WHERE id % 7 = 0" "VACUUM u" >/dev/null
heap=$(sql p "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) || ' ' || sum(id) FROM u WHERE id > 0")
idx=$(sql p "SET enable_seqscan = off" "SELECT count(*) || ' ' || sum(id) FROM u WHERE id > 0")
check "deletes and VACUUM under concurrent inserts: the index answers what the heap holds ($heap)" test "$idx" == "$heap"
check "and sift.col_verify finds every row" sql p "SELECT sift.col_verify('u_c', true)"

# VACUUM (PARALLEL 2) of heaps with B-trees and a sift_col index of format 1 and of format 2
for t in pv1 pv2; do
	sql p "CREATE TABLE $t (id int, v int, t text)" "INSERT INTO $t SELECT i, i % 100, 'x' || i FROM generate_series(1, 100000) i" \
		"CREATE INDEX ${t}_b ON $t (id)" "CREATE INDEX ${t}_b2 ON $t (v)" >/dev/null
done
sql p "SET sift.test_create_format1 = on" "CREATE INDEX pv1_c ON pv1 USING sift_col (id, v, t)" >/dev/null
sql p "CREATE INDEX pv2_c ON pv2 USING sift_col (id, v, t)" >/dev/null
sql p "DO \$\$ BEGIN FOR r IN 1..20 LOOP INSERT INTO pv2 SELECT i, 1, 'y' FROM generate_series(1000000 + r * 1000, 1000000 + r * 1000 + 999) i; END LOOP; END \$\$" \
	"DELETE FROM pv1 WHERE id % 3 = 0" "DELETE FROM pv2 WHERE id % 3 = 0" >/dev/null
out=$(sql p "SET min_parallel_index_scan_size = 0" "VACUUM (PARALLEL 2) pv1" "VACUUM (PARALLEL 2) pv2" "SELECT count(*) FROM pv1" "SELECT count(*) FROM pv2" 2>&1)
want=$(sql p "SELECT count(*) FROM generate_series(1, 100000) i WHERE i % 3 <> 0" "SELECT count(*) FROM (SELECT generate_series(1, 100000) UNION ALL SELECT generate_series(1001000, 1020999)) s(i) WHERE i % 3 <> 0")
check "VACUUM (PARALLEL 2) of format 1 and format 2 sift_col indexes beside B-trees: no error, every row kept" test "$out" == "$want"

# the rows a transaction drops from its own stage (UPDATE, DELETE, a savepoint rolled back) in a table of element columns
sql p "CREATE TABLE ta (id int, j jsonb) USING sift_table" "SELECT sift.table_set_paths('ta', 'j', 'j.k int4, j.a[]{x int4}')" "CREATE TABLE ms (ms float8)" >/dev/null
sql p "DO \$\$ DECLARE t0 timestamptz; BEGIN
	FOR i IN 1..10000 LOOP INSERT INTO ta VALUES (i, jsonb_build_object('k', i, 'a', jsonb_build_array(jsonb_build_object('x', i)))); END LOOP;
	t0 := clock_timestamp(); DELETE FROM ta; INSERT INTO ms VALUES (extract(epoch FROM clock_timestamp() - t0) * 1000); END \$\$" >/dev/null
ms=$(sql p "SELECT round(ms) FROM ms")
check "the DELETE of 10,000 rows the same transaction inserted is linear (${ms} ms)" test "$ms" -lt 2000
sql p "BEGIN; INSERT INTO ta SELECT i, jsonb_build_object('k', i, 'a', jsonb_build_array(jsonb_build_object('x', i), jsonb_build_object('x', 1))) FROM generate_series(1, 10000) i;
	DELETE FROM ta WHERE id % 2 = 0; UPDATE ta SET id = id + 1 WHERE id % 3 = 0; SAVEPOINT s; DELETE FROM ta WHERE id < 100; ROLLBACK TO s; COMMIT" >/dev/null
out=$(sql p "SELECT count(*) || ' ' || sum(id) || ' ' || sum((j->>'k')::int) || ' ' || sum((j->'a'->0->>'x')::int) FROM ta")
check "the rows the transaction kept are those of the heap of the same statements" test "$out" == "5000 25001667 25000000 25000000"

finish
