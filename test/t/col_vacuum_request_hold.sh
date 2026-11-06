#!/usr/bin/env bash
# The VACUUM that a build or a bulk write asks the worker for at its commit (col/vacreq.c) while a snapshot older than the rows holds the horizon back: the VACUUM cannot mark the pages
# all-visible then, and the table stays due. The worker looks again after a nap and vacuums at most once per five naptimes, never in a loop, and the first VACUUM after the snapshot
# ends marks the pages.
. "$(dirname "$0")/lib.bash"

plan 4
node_init p 41 "autovacuum = off
sift.col_auto_merge = on
sift.maintenance_naptime = 200"
node_start p || bail "start failed"
D=$(dir p)

sql p "CREATE EXTENSION pg_sift" "CREATE TABLE vh (id int PRIMARY KEY, g int NOT NULL, w int) WITH (fillfactor = 100)" >/dev/null || bail "setup failed"
vacuums() { sql p "SELECT pg_stat_clear_snapshot()" "SELECT vacuum_count FROM pg_stat_user_tables WHERE relname = 'vh'" | tail -1; }
pages() { sql p "SELECT relallvisible || '/' || relpages FROM pg_class WHERE relname = 'vh'"; }

# the holder: a repeatable read snapshot taken before the rows exist, kept for 20 s
psql -X -At -q -h "$D/sock" -p "$(port p)" -U postgres -d postgres -c "BEGIN ISOLATION LEVEL REPEATABLE READ" -c "SELECT count(*) FROM vh" -c "SELECT pg_sleep(20)" >/dev/null 2>&1 &
holder=$!
sleep 1

sql p "INSERT INTO vh SELECT i, i % 10, 0 FROM generate_series(1, 30000) i" "CREATE INDEX vh_c ON vh USING sift_col (g, w)" "UPDATE vh SET w = w + 1 WHERE id % 5 = 0" >/dev/null || bail "load failed"
sleep 8
n=$(vacuums)
between=f
(( n >= 1 && n <= 12 )) && between=t
check "a worker held by a snapshot vacuums more than once and less than a loop would ($n in 8 s)" [ "$between" == t ]
check "the pages are not all-visible while the snapshot holds the horizon ($(pages))" [ "$(sql p "SELECT relallvisible < relpages FROM pg_class WHERE relname = 'vh'")" == t ]

wait "$holder" 2>/dev/null
for _ in $(seq 100); do [[ $(sql p "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()") == 0 ]] && break; sleep 0.1; done
sql p "UPDATE vh SET w = w + 1 WHERE id % 5 = 1" >/dev/null
all=f
for _ in $(seq 150); do
	[[ $(sql p "SELECT relallvisible > 0 AND relallvisible = relpages FROM pg_class WHERE relname = 'vh'") == t ]] && { all=t; break; }
	sleep 0.2
done
check "the first VACUUM after the snapshot ends marks the pages ($(pages))" [ "$all" == t ]
check "the answers are the same through the index and the heap" [ "$(sql p "SELECT (SELECT sum(w) FROM vh), (SELECT sum(w) FROM vh WHERE g >= 0)")" == "$(sql p "SET sift.enable_agg = off" "SELECT (SELECT sum(w) FROM vh), (SELECT sum(w) FROM vh WHERE g >= 0)" | tail -1)" ]

node_stop p
finish
