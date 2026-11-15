#!/usr/bin/env bash
# What an operator sees of the background workers of sift: a rewrite worker says what it is and for which index in pg_stat_activity and in its process title
# (backend_type "sift rewrite", query "sift rewrite of schema.index"), sift.workers keeps when the last step ended well, which job it was and how long it took,
# a worker that failed (the VACUUM worker, which had no failure path) leaves the error where sift.workers shows it, and the error stays visible after the next success.
. "$(dirname "$0")/lib.bash"

plan 11
node_init wi 64 "autovacuum = off
sift.test_col_net = off
sift.maintenance_naptime = 200"
node_start wi || bail "start failed"
sql wi "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text, h1 bigint DEFAULT (random() * 1e12)::bigint, h2 bigint DEFAULT (random() * 1e12)::bigint, h3 bigint DEFAULT (random() * 1e12)::bigint)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount, h1, h2, h3)" "VACUUM facts" >/dev/null || bail "setup failed"

churn() { sql wi "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $1" "DELETE FROM facts WHERE id % 100 = $1 + 50" \
	"INSERT INTO facts SELECT $1 * 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" >/dev/null; }
scattered() { [[ $(sql wi "SELECT count(*) FILTER (WHERE kind = 'immutable') || ',' || count(*) FILTER (WHERE dirty) || ',' || coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) FROM sift.col_segments('facts_idx')") != "1,0,0" ]]; }
await_rewrite() { for _ in $(seq 600); do scattered || return 0; sleep 0.05; done; return 1; }
worker_row() { sql wi "SELECT backend_type || '|' || state || '|' || query FROM pg_stat_activity WHERE backend_type LIKE 'sift%' ORDER BY backend_start DESC LIMIT 1"; }
await_worker() { local i; for i in $(seq 600); do [[ -n $(worker_row) ]] && return 0; sleep 0.05; done; return 1; }
workers() { sql wi "SELECT $1 FROM sift.workers WHERE index = 'facts_idx'::regclass"; }

# 1. a rewrite worker held by the cost delay of its writes: its identity while it works
sql wi "ALTER DATABASE postgres SET sift.col_auto_merge = on" "ALTER DATABASE postgres SET sift.maintenance_cost_delay = 100" >/dev/null
churn 1
sql wi "VACUUM facts" >/dev/null
check "a worker of sift is running" await_worker
out=$(worker_row)
check "backend_type names the job" test "${out%%|*}" == "sift rewrite"
check "its query names the job and the index" test "${out##*|}" == "sift rewrite of public.facts_idx"
pid=$(sql wi "SELECT pid FROM pg_stat_activity WHERE backend_type LIKE 'sift%' LIMIT 1")
check "its process title names the index" bash -c "ps -o args= -p '$pid' | grep -q 'sift rewrite public.facts_idx'"
check "the rewrite ends" await_rewrite
sql wi "ALTER DATABASE postgres RESET sift.maintenance_cost_delay" >/dev/null

# 2. what the worker leaves behind
for _ in $(seq 100); do [[ -n $(workers last_ok) ]] && break; sleep 0.1; done
check "sift.workers has the end of the last step" test -n "$(workers last_ok)"
check "and its job" test "$(workers last_job)" == "rewrite"
check "and its time" test "$(workers "last_ms >= 0")" == "t"
check "no failure was counted" test "$(workers failures_total)" == "0"

# 3. a VACUUM worker that fails: the error reaches sift.workers
sql wi "ALTER DATABASE postgres SET sift.test_worker_error = 'vacuum'" >/dev/null
sql wi "UPDATE facts SET amount = amount + 1 WHERE id % 5 = 0" >/dev/null
for _ in $(seq 20); do
	sleep 0.5
	sql wi "UPDATE facts SET amount = amount + 1 WHERE id % 5 = 1" >/dev/null
	[[ -n $(workers last_error) ]] && break
done
for _ in $(seq 200); do [[ -n $(workers last_error) ]] && break; sleep 0.1; done
check "the error of the VACUUM worker is in sift.workers" bash -c "[[ '$(workers last_error)' == *'test: the vacuum worker fails'* ]]"

# 4. the next success leaves the error visible
sql wi "ALTER DATABASE postgres RESET sift.test_worker_error" >/dev/null
sleep 1
churn 2
sql wi "VACUUM facts" >/dev/null
await_rewrite
for _ in $(seq 100); do [[ $(workers "last_ok > failed_at") == t ]] && break; sleep 0.1; done
check "the error stays after a later success, which is newer" test "$(workers "last_error IS NOT NULL AND last_ok > failed_at AND failures_total >= 1")" == "t"
finish
