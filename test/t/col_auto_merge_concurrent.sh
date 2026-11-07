#!/usr/bin/env bash
# The rewrite of a sift_col index (sift.col_maintain, the work of sift.col_auto_merge) beside readers, writers and VACUUM: a writer updates, deletes and inserts
# in small transactions, VACUUM runs, the rewrite runs again and again (it is skipped while VACUUM holds the maintenance lock), and a reader compares in one
# repeatable read snapshot the answer of the column scan with the heap's. They all go on until the rewrite has run three times (it waits for the snapshots that hold the
# blocks of the last one, so how long that takes is not for a script to fix) and the reader has compared 30 snapshots. No answer differs, nothing fails, and the index verifies and ends clean.
. "$(dirname "$0")/lib.bash"

plan 7
node_init cc 23 "autovacuum = off
sift.log_min_duration = 0"
node_start cc || bail "start failed"
sql cc "CREATE EXTENSION pg_sift CASCADE" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 60000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"
D=$(dir cc)

# every worker goes on until $D/stop
running() { [[ ! -e $D/stop ]]; }
writer() {
	local i=0
	while running; do
		i=$((i + 1))
		sql cc "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $((i % 50))" "DELETE FROM facts WHERE id % 100 = $((i % 50 + 50)) AND id < 60000" \
			"INSERT INTO facts SELECT 1000000 + $i * 1000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 300) g" >/dev/null 2>>"$D/writer.err" || echo failed >>"$D/writer.err"
		echo >>"$D/writer.n"
	done
}
vacuumer() { while running; do sql cc "VACUUM facts" >/dev/null 2>>"$D/vacuum.err"; sleep 0.3; done; }
maintainer() { while running; do sql cc "SELECT sift.col_maintain('facts_idx')" >/dev/null 2>>"$D/maintain.err"; done; }
# the answers of the column scan and of the heap in one snapshot: 0 when they agree
reader() {
	while running; do
		sql cc "BEGIN ISOLATION LEVEL REPEATABLE READ" "SET max_parallel_workers_per_gather = 0" \
			"SET sift.enable_agg = on" "CREATE TEMP TABLE a ON COMMIT DROP AS SELECT kind, count(*) c, sum(amount) s FROM facts GROUP BY kind" \
			"SET sift.enable_agg = off" "CREATE TEMP TABLE b ON COMMIT DROP AS SELECT kind, count(*) c, sum(amount) s FROM facts GROUP BY kind" \
			"SELECT count(*) FROM ((SELECT * FROM a EXCEPT SELECT * FROM b) UNION ALL (SELECT * FROM b EXCEPT SELECT * FROM a)) d" "COMMIT" 2>>"$D/reader.err" | tail -1
	done
}
lines() { [[ -f $1 ]] && wc -l <"$1" | tr -d ' ' || echo 0; }
rewrites() { grep -ac "rewrote (.*) the segments of index" "$D/postgres.log"; }

rm -f "$D/stop" "$D/writer.n"
for f in writer vacuum maintain reader; do : >"$D/$f.err"; done
writer & wp=$!
vacuumer & vp=$!
maintainer & mp=$!
reader >"$D/reader.out" & rp=$!
# two minutes at most; the state at the end is in the diagnostic
for _ in $(seq 1 600); do
	[[ $(rewrites) -ge 3 && $(lines "$D/reader.out") -ge 30 && $(lines "$D/writer.n") -ge 30 ]] && break
	sleep 0.2
done
touch "$D/stop"
wait "$wp" "$vp" "$mp" "$rp"
diag "rewrites $(rewrites), writer transactions $(lines "$D/writer.n"), reader snapshots $(lines "$D/reader.out"), promotions $(grep -ac "promoted (" "$D/postgres.log")"
check "the writer's transactions all committed" test ! -s "$D/writer.err" -a "$(lines "$D/writer.n")" -ge 30
check "the rewrite ran at least three times beside them" test "$(rewrites)" -ge 3
check "VACUUM and the rewrite ran without an error" test ! -s "$D/vacuum.err" -a ! -s "$D/maintain.err"
check "the reader ran 30 snapshots without an error" test "$(lines "$D/reader.out")" -ge 30 -a ! -s "$D/reader.err"
check "the column scan and the heap agreed in every snapshot" test "$(sort -u "$D/reader.out")" == 0
sql cc "VACUUM facts" "SELECT sift.col_maintain('facts_idx')" >/dev/null
check "afterwards the index verifies" test -z "$(sql cc "SELECT sift.col_verify('facts_idx', true)" 2>&1)"
check "and the answers are the heap's" test "$(sql cc "SET sift.enable_agg = on" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1)" == \
	"$(sql cc "SET sift.enable_agg = off" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1)"
finish
