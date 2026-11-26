#!/usr/bin/env bash
# tap: pooled
# What an operator sees of the background workers of sift: a promotion worker says what it is and for which index in pg_stat_activity and in its process title
# (backend_type "sift promote", query "sift promote of schema.index"), sift.workers keeps when the last step ended well, which job it was and how long it took,
# and a failure counter that stays zero while the steps end well.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 9
node_init wi 64 "sift.test_create_format1 = on
autovacuum = off
sift.test_col_net = off
sift.maintenance_naptime = 200"
node_start wi || bail "start failed"
D=$(dir wi)
sql wi "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text, h1 bigint DEFAULT (random() * 1e12)::bigint, h2 bigint DEFAULT (random() * 1e12)::bigint, h3 bigint DEFAULT (random() * 1e12)::bigint)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount, h1, h2, h3)" "VACUUM facts" >/dev/null || bail "setup failed"

churn() { sql wi "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $1" "DELETE FROM facts WHERE id % 100 = $1 + 50" \
	"INSERT INTO facts SELECT $1 * 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" >/dev/null; }
hold() { tap_tx_hold wi gate "SELECT pg_advisory_lock(20983, 12)"; }   # the advisory lock of the progress gate (sift.test_progress_gate)
let_go() { tap_tx_release gate; }
buffered() { sql wi "SELECT coalesce(sum(rows), 0) FROM sift.col_segments('facts_idx') WHERE kind <> 'immutable'"; }
await_promotion() { for _ in $(seq 600); do [[ $(buffered) == 0 ]] && return 0; sleep 0.05; done; return 1; }
worker_row() { sql wi "SELECT backend_type || '|' || state || '|' || query FROM pg_stat_activity WHERE datname = current_database() AND backend_type LIKE 'sift%' ORDER BY backend_start DESC LIMIT 1"; }
await_worker() { local i; for i in $(seq 600); do [[ -n $(worker_row) ]] && return 0; sleep 0.05; done; return 1; }
workers() { sql wi "SELECT $1 FROM sift.workers WHERE index = 'facts_idx'::regclass"; }

# 1. a maintenance worker held at the start of its promotion: its identity while it works
hold
sql wi "ALTER DATABASE $(node_db wi) SET sift.async_maintenance = on" "ALTER DATABASE $(node_db wi) SET sift.test_progress_gate = 1" >/dev/null
churn 1
sql wi "VACUUM facts" >/dev/null
check "a worker of sift is running" await_worker
out=$(worker_row)
check "backend_type names the job" test "${out%%|*}" == "sift promote"
check "its query names the job and the index" test "${out##*|}" == "sift promote of public.facts_idx"
pid=$(sql wi "SELECT pid FROM pg_stat_activity WHERE datname = current_database() AND backend_type LIKE 'sift%' LIMIT 1")
check "its process title names the index" bash -c "ps -o args= -p '$pid' | grep -q 'sift promote public.facts_idx'"
let_go
check "the promotion ends" await_promotion
sql wi "ALTER DATABASE $(node_db wi) RESET sift.test_progress_gate" >/dev/null

# 2. what the worker leaves behind
for _ in $(seq 100); do [[ -n $(workers last_ok) ]] && break; sleep 0.1; done
check "sift.workers has the end of the last step" test -n "$(workers last_ok)"
check "and its job" test "$(workers last_job)" == "promote"
check "and its time" test "$(workers "last_ms >= 0")" == "t"
check "no failure was counted" test "$(workers failures_total)" == "0"
finish
