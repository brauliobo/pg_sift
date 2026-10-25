#!/usr/bin/env bash
# The heap check the planner records for the paths it prices (agg/refuse.c) lives in one context a backend keeps for its life: with the visibility map
# empty (a rewrite leaves it so) and sift.explain_refusals on, a refused CTE plan records it. The leak gate (finish) must find nothing left over after a
# session that ends normally, and after one that is terminated in the middle of a statement.
. "$(dirname "$0")/lib.bash"

plan 3
node_init pv 29 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start pv || bail "start failed"
sql pv "CREATE EXTENSION pg_sift" \
	"CREATE TABLE ordered (id int, k int, v int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO ordered SELECT i, i / 300, i % 7, repeat('x', 1000) FROM generate_series(0, 59999) i" \
	"CREATE INDEX ordered_k ON ordered (k)" \
	"CREATE INDEX ordered_col ON ordered USING sift_col (id, k, v)" \
	"CLUSTER ordered USING ordered_k" "ANALYZE ordered" >/dev/null || bail "setup failed"

CTE="WITH r AS (SELECT * FROM ordered WHERE k < 12) SELECT a.v, a.n, b.s FROM (SELECT v, count(*) AS n FROM r GROUP BY v) a JOIN (SELECT v, sum(k) AS s FROM r GROUP BY v) b ON a.v = b.v"
WHY="SELECT count(*) FROM sift.explain_why('$CTE') WHERE reason LIKE '%all-visible%'"

check "the refused CTE plan records the heap check of an empty visibility map" test "$(sql pv "$WHY")" -ge 1
# a session that plans it in a transaction with another statement, and ends
sql pv "SET sift.explain_refusals = on" "SET client_min_messages = warning" "EXPLAIN $CTE" "EXPLAIN SELECT v, count(*) FROM ordered WHERE k < 200 GROUP BY v" >/dev/null
check "a session that recorded it ends normally" test "$(sql pv "SELECT 1")" == 1

# and one that is terminated while it runs a statement
PGAPPNAME=vetter sql pv "SET sift.explain_refusals = on" "SET client_min_messages = warning" "EXPLAIN $CTE" "SELECT pg_sleep(60)" >/dev/null 2>&1 &
vetter=$!
for _ in $(seq 100); do [[ $(sql pv "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'vetter' AND query LIKE 'SELECT pg_sleep%'") == 1 ]] && break; sleep 0.1; done
check "a session terminated in a statement is gone" test "$(sql pv "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'vetter'")" == t
wait "$vetter" 2>/dev/null
# the exit of the backend writes the report: finish scans the log after the stop
for _ in $(seq 100); do [[ $(sql pv "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'vetter'") == 0 ]] && break; sleep 0.1; done
finish
