#!/usr/bin/env bash
# tap: pooled
# sift.progress: one row for each maintenance job that runs now. A maintenance worker held at the start of its promotion (sift.test_progress_gate, a testing build) is listed with its process,
# index, job and phase and the segments it will replace; the merges of a VACUUM held at its cleanup gate are listed with the process of the session; both are gone when the job ends.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
node_init wp 66 "autovacuum = off
sift.test_col_net = off
sift.maintenance_naptime = 200"
node_start wp || bail "start failed"
D=$(dir wp)
sql wp "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text, h1 bigint DEFAULT (random() * 1e12)::bigint, h2 bigint DEFAULT (random() * 1e12)::bigint, h3 bigint DEFAULT (random() * 1e12)::bigint) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount, h1, h2, h3)" "VACUUM facts" >/dev/null || bail "setup failed"
churn() { sql wp "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $1" "DELETE FROM facts WHERE id % 100 = $1 + 50" \
	"INSERT INTO facts SELECT $1 * 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" >/dev/null; }
rows() { sql wp "SELECT count(*) FROM sift.progress"; }
hold() { tap_tx_hold wp gate "SELECT pg_advisory_lock(20983, $1)"; }   # KEY: the advisory lock of the gate
let_go() { tap_tx_release gate; }

# 1. a maintenance worker held when it enters its promote phase (the first of its phases)
hold 12
sql wp "ALTER DATABASE $(node_db wp) SET sift.async_maintenance = on" "ALTER DATABASE $(node_db wp) SET sift.test_progress_gate = 1" >/dev/null
churn 1
sql wp "VACUUM facts" >/dev/null
waiting() { tap_tx_blocked wp; }
check "the maintenance worker waits at its promote phase" wait_for waiting 60
out=$(sql wp "SELECT p.job, p.phase, p.index = 'facts_idx'::regclass, p.segments_done, p.segments_total >= 1, p.started IS NOT NULL, a.backend_type FROM sift.progress p JOIN pg_stat_activity a ON a.pid = p.pid")
check "sift.progress lists it: job, phase, index, no segment promoted yet" test "$out" == "promote|promote|t|0|t|t|sift promote"
let_go
gone() { [[ $(rows) == 0 ]]; }
check "the row is gone when the promotion ends" wait_for gone 60
check "the promotion did its work: nothing is left in the write buffer" bash -c "[[ \$(psql -X -At -h '$(node_sock wp)' -p $(port wp) -U postgres -d '$(node_db wp)' -c \"SELECT coalesce(sum(rows), 0) FROM sift.col_segments('facts_idx') WHERE kind <> 'immutable'\") == 0 ]]"
sql wp "ALTER DATABASE $(node_db wp) RESET sift.test_progress_gate" "ALTER DATABASE $(node_db wp) SET sift.async_maintenance = off" >/dev/null

# 2. the merges of a VACUUM, held at its cleanup gate
churn 2
hold 2
sql wp "LOAD 'pg_sift'" "SET sift.test_cleanup_gate = on" "SET sift.async_maintenance = off" "VACUUM facts" >"$D/vacuum.out" 2>&1 &
VP=$!
check "the VACUUM waits at its cleanup gate" wait_for waiting 60
out=$(sql wp "SELECT p.job, p.index = 'facts_idx'::regclass, a.backend_type, a.query LIKE 'VACUUM%' FROM sift.progress p JOIN pg_stat_activity a ON a.pid = p.pid")
check "sift.progress lists the VACUUM of the session" test "$out" == "vacuum|t|client backend|t"
let_go
wait "$VP"
check "nothing is listed when the VACUUM ends" gone
finish
