#!/usr/bin/env bash
# Searches while inserters, deleters and VACUUM run side by side: every search of rows that never change returns exactly them. A lazy
# VACUUM is left out of snapshots and of the xid horizon, its own xid included, so the extents it released with that xid must wait
# for the next xid (store_update, P4): a reader that began after the xid was assigned holds the directory as it was before the
# release and must find its segments as it left them. Runs on the production build too (no test functions).
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_CHURN_SECS 30 10)
STABLE=3000
plan 4
node_init rd 12 "autovacuum = off
max_worker_processes = 16"
node_start rd || bail "start failed"
sql rd "CREATE EXTENSION pg_sift" "CREATE TABLE t (id serial, body text)" \
	"INSERT INTO t (body) SELECT 'zzstable s' || i || ' ' || string_agg('w' || (random() * 2000)::int, ' ') FROM generate_series(1, $STABLE) i, generate_series(1, 20) GROUP BY i" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64, build_segments = 8)" \
	"CREATE FUNCTION chk(k int) RETURNS void LANGUAGE plpgsql AS \$\$ BEGIN
	IF (SELECT count(*) FROM t WHERE id <= $STABLE AND body @@> ('s' || k)) <> 1 THEN RAISE EXCEPTION 'search for s% wrong', k; END IF;
	IF (SELECT count(*) FROM t WHERE id <= $STABLE AND body @@> 'zzstable') <> $STABLE THEN RAISE EXCEPTION 'zzstable wrong'; END IF;
END \$\$" >/dev/null || bail "setup failed"
check "the stable rows are found" test "$(sql rd "SET enable_seqscan = off" "SELECT count(*) FROM t WHERE body @@> 'zzstable'" | tail -1)" == "$STABLE"

D=$(dir rd)
printf '%s\n' "INSERT INTO t (body) SELECT 'zzvolatile ' || string_agg('w' || (random() * 2000)::int, ' ') FROM generate_series(1, 40);" >"$D/insert.sql"
printf '%s\n' "\\set did random($((STABLE + 1)), 200000)" 'DELETE FROM t WHERE id = :did;' >"$D/delete.sql"
printf '%s\n' "\\set k random(1, $STABLE)" 'SELECT chk(:k);' >"$D/read.sql"
PGOPTIONS="-c enable_seqscan=off" pgbench_bg rd 24 "$SECS" -f "$D/insert.sql@8" -f "$D/delete.sql@2" -f "$D/read.sql@40"

: >"$D/maint.out"
while kill -0 "$PGBENCH_PID" 2>/dev/null; do
	sql rd "VACUUM t" >>"$D/maint.out" 2>&1 || echo "FAILED" >>"$D/maint.out"
done
wait "$PGBENCH_PID"
diag "$(grep -E "^(number of transactions actually|tps)" "$D/pgbench.log")"
check "no search returned a wrong result" test "$(grep -c -E "wrong|aborted|corrupt" "$D/pgbench.log")" == 0
check "VACUUM ran without error" test "$(grep -c -E "FAILED|ERROR" "$D/maint.out")" == 0

sql rd "VACUUM t" >/dev/null
check "verify: intact, every row indexed" test "$(sql rd "SELECT sift.verify('t_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
finish
