#!/usr/bin/env bash
# A forced merge stays within maintenance_work_mem: the process that merges 4,000,000 rows at 64 MB takes under 64 MB of anonymous memory in all, and the segments it writes
# are the partitions that memory allows, all of one size (the last one no short one). The inputs' batches and the output's used to be held until the end of the merge (97 MB
# for 5,000,000 rows at 64 MB), and the partitions to be cut at a share of the bytes a unit overshoots, with a short last one.
. "$(dirname "$0")/lib.bash"

plan 4
node_init mem 11 "autovacuum = off
sift.col_auto_merge = off
sift.async_maintenance = off"
node_start mem || bail "start failed"
sql mem "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, g int, ts timestamp, amount int, pad text)" \
	"INSERT INTO t SELECT i, i % 50, timestamp '2026-01-01' + floor(random() * 100003)::int * interval '1 minute', i % 100, md5(i::text) FROM generate_series(1, 4000000) i" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "load failed"
sql mem "SET maintenance_work_mem = '64MB'" "SET max_parallel_maintenance_workers = 2" "CREATE INDEX t_col ON t USING sift_col (id, g, ts, amount)" >/dev/null || bail "build failed"
check "the build wrote several segments" test "$(sql mem "SELECT count(*) > 3 FROM sift.col_segments('t_col') WHERE kind = 'immutable'")" == t

D=$(dir mem)
PM=$(head -1 "$D/data/postmaster.pid")
rm -f "$D/stop"
(
	best=0
	while [[ ! -e $D/stop ]]; do
		for p in $(pgrep -P "$PM"); do
			kb=$(awk '/^RssAnon/ {print $2}' "/proc/$p/status" 2>/dev/null)
			(( ${kb:-0} > best )) && best=$kb
		done
		sleep 0.02
	done
	echo $((best / 1024)) >"$D/peak"
) &
POLL=$!
sql mem "SET maintenance_work_mem = '64MB'" "SELECT sift.col_merge('t_col', true)" >/dev/null || bail "merge failed"
touch "$D/stop"
wait "$POLL"
diag "merge peak: $(<"$D/peak") MB of anonymous memory in one process"
if [[ -z ${SIFT_PRELOAD:-} ]]; then
	check "the merge stays under 64 MB at a 64 MB budget" test "$(<"$D/peak")" -lt 64
else
	skip 1 "the sanitizer's shadow memory and quarantine inflate the process (SANITIZE=1)"
fi
diag "partition bytes: $(sql mem "SELECT string_agg(bytes::text, ' ' ORDER BY gen) FROM sift.col_segments('t_col') WHERE kind = 'immutable'")"
check "the partitions are of one size: the smallest is over 4/5 of the largest" \
	test "$(sql mem "SELECT min(bytes) > 0.8 * max(bytes) FROM sift.col_segments('t_col') WHERE kind = 'immutable'")" == t
WAL=$(sql mem "SELECT pg_current_wal_lsn()")
sql mem "SET maintenance_work_mem = '64MB'" "SELECT sift.col_merge('t_col', true)" >/dev/null
check "a second force at the same memory writes nothing" test "$(sql mem "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '$WAL') < 65536")" == t
finish
