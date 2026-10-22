#!/usr/bin/env bash
# The VACUUM that churn and scans ask for (sift.col_auto_vacuum, off by default): after churn spread over a table every heap page is not all-visible, so a
# scan of a sift_col index checks the rows in the heap one by one. A backend that inserts a row version, or whose scan checked many rows, looks at the
# statistics of the table and starts a worker that vacuums it; the next scan checks almost none. Here: off, the scan checks the rows and nothing is
# vacuumed; on, the table is vacuumed and the next scan is cheap and answers as the heap does; a change of a few rows (under 50) asks for nothing; a table
# with autovacuum_enabled = false is left alone, and vacuumed once it is not.
. "$(dirname "$0")/lib.bash"

plan 9
node_init av 22 "autovacuum = off
sift.maintenance_naptime = 200"
node_start av || bail "start failed"
sql av "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 150000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"

# when the table was last vacuumed (the statistics are written when the VACUUM ends)
vacuumed() { sql av "SELECT coalesce(extract(epoch FROM last_vacuum)::text, '-') FROM pg_stat_user_tables WHERE relname = 'facts'"; }
# the scan the churn slows down; the rows it checked in the heap
SCAN="SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind"
vetted() { sql av "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF) $SCAN" | sed -n 's/.*Rows Vetted By Heap: //p'; }
groups() { sql av "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM ($SCAN) x" | tail -1; }
# until the table is vacuumed after $1 (what vacuumed printed then), asking with a scan every so often
await_vacuum() {
	for _ in $(seq 100); do
		vetted >/dev/null
		[[ $(vacuumed) == "$1" ]] || return 0
		sleep 0.2
	done
	return 1
}
# stays as it was for 2 s although scans ask
stays() {
	for _ in $(seq 8); do vetted >/dev/null; sleep 0.25; done
	[[ $(vacuumed) == "$1" ]]
}
churn() { sql av "UPDATE facts SET amount = amount + 1 WHERE id % 20 = $1" >/dev/null; }

# 1. off (the default)
T0=$(vacuumed)
churn 1
check "off: the scan checks the rows in the heap (the churn is spread over every page)" test "$(vetted)" -gt 100000
check "off: and the table is not vacuumed" stays "$T0"

# 2. on
sql av "ALTER DATABASE postgres SET sift.col_auto_vacuum = on" >/dev/null
check "on: the table is vacuumed after a scan that checked its rows" await_vacuum "$T0"
check "on: the next scan checks few rows" test "$(vetted)" -lt 10000
check "on: and answers as the heap does" test "$(groups on)" == "$(groups off)"

# 3. a few rows changed: fewer than the 50 under which a table is clean
sleep 1.5
T1=$(vacuumed)
sql av "UPDATE facts SET amount = amount + 1 WHERE id % 15000 = 1" >/dev/null
check "a change of ten rows (under 50): not vacuumed" stays "$T1"

# 4. autovacuum_enabled = false is left alone, and vacuumed once it is not
sql av "ALTER TABLE facts SET (autovacuum_enabled = false)" >/dev/null
churn 2
check "autovacuum_enabled = false: not vacuumed" stays "$T1"
sql av "ALTER TABLE facts RESET (autovacuum_enabled)" >/dev/null
check "reset: vacuumed" await_vacuum "$T1"
check "the index verifies" test "$(sql av "SELECT sift.col_verify('facts_idx', true)" 2>&1 | grep -c ERROR)" == 0

finish
