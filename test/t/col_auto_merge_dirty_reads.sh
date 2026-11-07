#!/usr/bin/env bash
# The rewrite the reads of an index ask for (sift.col_auto_merge). Rows deleted from one segment leave its units dirty after the VACUUM, under the 30% of the units from which
# the rule rewrites, and a dirty unit is not served from the point directory: every read of a key in it reads the unit (60 keys: 0.90 M instructions before the VACUUM,
# 1.75 M after it). The rewrite costs 886 M instructions and 3 MB of WAL, so a backend that reads the dirty units asks for it only when what its reads owe comes to half of that.
. "$(dirname "$0")/lib.bash"

plan 6
node_init rd 25 "autovacuum = off
sift.maintenance_naptime = 200"
node_start rd || bail "start failed"
sql rd "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, k int, v int, pad text)" \
	"INSERT INTO t SELECT g, g % 3000, g % 1000, md5(g::text) FROM generate_series(1, 300000) g" \
	"CREATE INDEX t_c ON t USING sift_col (id, k, v)" "VACUUM (ANALYZE, FREEZE) t" >/dev/null || bail "setup failed"
sql rd "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null

dirty() { sql rd "SELECT coalesce(sum(units) FILTER (WHERE dirty), 0) FROM sift.col_segments('t_c')"; }
# the rewrite is done: 30 s at most
clean() { for _ in $(seq 400); do [[ $(dirty) == 0 ]] && return 0; sleep 0.3; done; return 1; }
ids=$(seq 1 333 20000 | head -60 | paste -sd, -)
# $1 batches of 100 reads of 60 keys in the dirty units in one session, a naptime apart: the look of the tick is once per naptime
reads() { sql rd "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 0" \
	"DO \$\$ BEGIN FOR b IN 1..$1 LOOP FOR i IN 1..100 LOOP PERFORM id, k, v FROM t WHERE id IN ($ids); END LOOP; PERFORM pg_sleep(0.3); END LOOP; END \$\$" >/dev/null; }
churn() { sql rd "DELETE FROM t WHERE id BETWEEN $1 AND $2 AND id % 5 = 0" "VACUUM t" >/dev/null; }

churn 1 20000
check "the VACUUM left units with dead rows and the rule does not rewrite them" test "$(dirty)" -gt 0 -a -z "$(sql rd "SELECT rewrite_due FROM sift.col_maintenance_status('t_c')")"
reads 5
sleep 2
check "a few reads do not ask for the rewrite" test "$(dirty)" -gt 0
reads 40
check "the reads that came to half of it did" clean
check "the answers are the table's" test "$(sql rd "SELECT sum(v), count(*) FROM t WHERE id IN ($ids)")" == "$(sql rd "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET enable_indexonlyscan = off" "SELECT sum(v), count(*) FROM t WHERE id IN ($ids)" | tail -1)"

sql rd "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
churn 20001 40000
reads 40
sleep 2
check "with the setting off the same reads ask for nothing" test "$(dirty)" -gt 0
sql rd "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null
reads 40
check "and with it on again they do" clean
finish
