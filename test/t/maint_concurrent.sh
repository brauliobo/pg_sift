#!/usr/bin/env bash
# Promotion and merge steps of the worker and of inserters side by side (they claim the segments they replace),
# with deletes, VACUUM and the exclusive SQL functions in between: no error, every row indexed, nothing leaked.
# Runs on the production build too (no test functions).
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_CHURN_SECS 20 4)
plan 4
node_init conc2 10 "autovacuum = off
max_worker_processes = 16"
node_start conc2 || bail "start failed"
sql conc2 "CREATE EXTENSION pg_sift" "CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (mutable_limit = 64)" >/dev/null || bail "setup failed"

D=$(dir conc2)
printf '%s\n' "INSERT INTO t (body) SELECT 'zzcommon ' || string_agg('w' || (random() * 3000)::int, ' ') FROM generate_series(1, 40);" >"$D/insert.sql"
printf '%s\n' '\set did random(1, 200000)' 'DELETE FROM t WHERE id = :did;' >"$D/delete.sql"
pgbench_bg conc2 12 "$SECS" -f "$D/insert.sql@20" -f "$D/delete.sql@1"

# VACUUM and the exclusive SQL functions meanwhile
: >"$D/maint.out"
while kill -0 "$PGBENCH_PID" 2>/dev/null; do
	sql conc2 "VACUUM t" "SELECT sift.promote('t_idx')" "SELECT sift.merge('t_idx')" >>"$D/maint.out" 2>&1 || echo "FAILED" >>"$D/maint.out"
done
wait "$PGBENCH_PID"
check "pgbench: no failed transaction" grep -q "number of failed transactions: 0 " "$D/pgbench.log"
diag "$(grep -E "^(number of transactions actually|tps)" "$D/pgbench.log")"
check "VACUUM, promote and merge ran without error" test "$(grep -c -E "FAILED|ERROR" "$D/maint.out")" == 0

sql conc2 "VACUUM t" >/dev/null
check "verify: intact, every row indexed" test "$(sql conc2 "SELECT sift.verify('t_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
check "the index counts the rows of the table" \
	test "$(sql conc2 "SELECT count(*) FROM t")" == "$(sql conc2 "SET enable_seqscan = off" "SELECT count(*) FROM t WHERE body @@> 'zzcommon'" | tail -1)"
finish
