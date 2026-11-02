#!/usr/bin/env bash
# The VACUUM an index-only scan asks for (sift.col_auto_merge). A table that only updates in place (HOT) leaves no dead tuple for the statistics once a read prunes
# it, and an index-only scan fetches the heap for every row of a page the visibility map does not hold: nothing but the scans themselves can say so. A scan that
# returns many rows of such pages (a sample of its rows is looked up in the map) starts the worker and the map is set again; a scan that returns few rows starts
# none, and with the setting off nothing does.
. "$(dirname "$0")/lib.bash"

plan 5
node_init rd 25 "autovacuum = off
sift.maintenance_naptime = 200"
node_start rd || bail "start failed"
sql rd "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_visibility" "CREATE TABLE facts (id int PRIMARY KEY, key int, v int, pad text) WITH (fillfactor = 70)" \
	"INSERT INTO facts SELECT g, g % 300, g % 1000, md5(g::text) FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (key, v)" "VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null || bail "setup failed"

# the pages that are not all-visible
unclean() { sql rd "SELECT relpages - all_visible FROM pg_visibility_map_summary('facts'), (SELECT relpages FROM pg_class WHERE relname = 'facts') c"; }
# an index-only scan of the access method over key between 0 and $1 (the rows of ($1 + 1) / 300 of the table)
ioss() { sql rd "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SET enable_indexscan = off" "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 0" \
	"SELECT key, v FROM facts WHERE key BETWEEN 0 AND $1" >/dev/null; }
# scans of $1 for 4 s: the map is as it was
stays() { local u; u=$(unclean); for _ in $(seq 20); do ioss "$1"; sleep 0.2; done; [[ $(unclean) == "$u" ]]; }
# scans of $1 until the pages are all-visible again, 60 s at most
returns() { for _ in $(seq 300); do ioss "$1"; [[ $(unclean) == 0 ]] && return 0; sleep 0.2; done; return 1; }
hot_churn() { sql rd "UPDATE facts SET pad = pad || 'x' WHERE id % 50 = $1" >/dev/null; }

sql rd "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null
hot_churn 1
check "the churn left pages out of the map and every update was HOT" test "$(unclean)" -gt 100 -a "$(sql rd "SELECT n_tup_upd = n_tup_hot_upd FROM pg_stat_user_tables WHERE relname = 'facts'")" == t
check "scans of a few rows start no VACUUM" stays 0
check "scans of many rows start it and the pages are all-visible again" returns 29
check "the answers are the table's" test "$(sql rd "SELECT sum(v) FROM facts WHERE key BETWEEN 0 AND 29")" == "$(sql rd "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET enable_indexonlyscan = off" "SELECT sum(v) FROM facts WHERE key BETWEEN 0 AND 29" | tail -1)"

sql rd "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
hot_churn 2
check "with the setting off the scans of many rows start none" stays 29
finish
