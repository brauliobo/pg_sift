#!/usr/bin/env bash
# The memory of a key_cut build that reads the heap through a btree on the key, in exact bytes (scope col_build of core/membudget.h, sift._test_mem_highwater()), on the 8-page-superblock
# flavour (scripts/scaling-gate.sh) so that a budget of a few MB fills on a few tens of thousands of rows:
#  - the serial build: the rows of a run are held up to the segment budget and the run is written beside them, so the scope holds 2.1 times the budget and the buffers of the writer: bound =
#    2.1 * budget + 6 MB at every size, and the fitted per-row term over the sizes stays under SLOPE_MAX
#  - a participant of the parallel build (the leader and the workers, each with a budget of its own): the same bound for the participant that held most, read from the scope line each backend
#    logs when it exits
#  - the choice of the path (scope col_btree_pick): the walk of the btree through a simulated cache of the heap blocks holds the same bytes whatever shared_buffers is (the cache is scaled
#    down to 65,536 blocks), and under the budget of the scope
# The backend of each size is a new one; the maintenance worker and the merge of the build are off.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

IFS=, read -ra SIZES <<<"$(knob SIFT_BTREE_SIZES 40000,80000,120000 16000,32000,48000)"
SEG_KB=$(knob SIFT_BTREE_SEG_KB 4096 1664)
PAR_SEG_KB=$(knob SIFT_BTREE_PAR_SEG_KB 1024 416)
BIG=${SIZES[2]}
SLOPE_MAX=32
MB=1048576
BOUND=$((SEG_KB * 1024 * 21 / 10 + 6 * MB))
PAR_BOUND=$((PAR_SEG_KB * 1024 * 21 / 10 + 6 * MB))
PICK_BUDGET=$((24 * MB))
LOWER=""

plan 10
node_init bm 91 "sift.test_create_format1 = on
sift.col_build_source = auto
autovacuum = off
max_parallel_workers_per_gather = 0
max_worker_processes = 16
max_parallel_workers = 8
shared_buffers = 64MB
sift.async_maintenance = off
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start bm || bail "start failed"
sql bm "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"
LOG=$(dir bm)/postgres.log

# load ROWS: the table and the btree on the key, which the build is told to read the heap through
load() {
	sql bm "DROP TABLE IF EXISTS e CASCADE" \
		"CREATE TABLE e WITH (autovacuum_enabled = off) AS SELECT g AS id, g AS k_hi, g % 5000 AS k_mid, g % 10 AS k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] AS cat,
			'name' || (g::bigint * 7919) % 1000000 AS txt, ((g::bigint * 7919) % 29500)::numeric / 100 AS amount, repeat('x', 400) AS p FROM generate_series(1, $1) g" \
		"CREATE INDEX e_k ON e (k_mid)" "VACUUM ANALYZE e" >/dev/null || bail "setup of $1 rows failed"
}

# build ROWS WORKERS CACHE SEG_KB: one build in a new backend; prints "LEADER_PEAK PICK_PEAK WALK_BYTES PATH WORKERS_LAUNCHED"
build() {
	sql bm "SET max_parallel_maintenance_workers = $2" "SET min_parallel_table_scan_size = 0" "SET maintenance_work_mem = '96MB'" "SET sift.test_limits = '$LOWER'" \
		"SET sift.test_col_segment_kb = $4" "SET sift.test_col_spill_run_kb = $4" "SET sift.test_col_build_btree = 1" "SET sift.test_col_pick_cache_blocks = $3" \
		"CREATE INDEX e_col ON e USING sift_col (k_hi, k_mid, k_lo, cat, txt, amount) WITH (key_cut = 'k_mid')" \
		"SELECT (SELECT max(peak) FILTER (WHERE scope = 'col_build') || ' ' || max(peak) FILTER (WHERE scope = 'col_btree_pick') FROM sift._test_mem_highwater(true)) || ' ' ||
			current_setting('sift.test_col_pick_walk_bytes') || ' ' || CASE WHEN current_setting('sift.test_col_pick_reads')::int > 0 AND current_setting('sift.test_col_spill_bytes')::int = 0 THEN 'btree' ELSE 'other' END || ' ' || current_setting('sift.test_col_workers')" | tail -1
}

fit() {
	awk 'NF == 2 { n++; x[n] = $1; y[n] = $2; sx += $1; sy += $2 }
		END { mx = sx / n; my = sy / n; for (i = 1; i <= n; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) ^ 2 }; printf "%.0f\n", sxy / sxx }' <<<"$1"
}

# worker_peak OFFSET: the greatest peak of the col_build scope among the participants that exited after OFFSET of the log (the leader's backend logs at its exit too): every process of the
# build has exited, and logged, when the postmaster has no more children than before it (a participant that was given no rows logs no peak, so the lines cannot be counted)
postmaster_children() { pgrep -P "$(head -1 "$(dir bm)/data/postmaster.pid")" | wc -l; }
worker_peak() {
	wait_for "(( \$(postmaster_children) <= $2 ))" 60
	tail -c +$(($1 + 1)) "$LOG" | sed -n 's/.*SIFTMEM_SCOPE name=col_build peak=\([1-9][0-9]*\) .* checks=[1-9].*/\1/p' | sort -n | tail -1
}

serial="" parallel="" over_serial=0 over_parallel=0 paths_ok=1
for n in "${SIZES[@]}"; do
	load "$n"
	read -r peak _ _ path _ <<<"$(build "$n" 0 -1 "$SEG_KB")"
	sql bm "DROP INDEX e_col" >/dev/null
	serial+="$n $peak"$'\n'
	((peak <= BOUND)) || over_serial=$((over_serial + 1))
	[[ $path == btree ]] || paths_ok=0
	off=$(wc -c <"$LOG" | tr -d " ") idle=$(postmaster_children)
	read -r _ _ _ path launched <<<"$(build "$n" 2 -1 "$PAR_SEG_KB")"
	peak=$(worker_peak "$off" "$idle")
	sql bm "DROP INDEX e_col" >/dev/null
	parallel+="$n $peak"$'\n'
	((peak <= PAR_BOUND)) || over_parallel=$((over_parallel + 1))
	[[ $path == btree && $launched -ge 2 ]] || paths_ok=0
done
diag "col_build bytes by rows, serial btree build at ${SEG_KB} kB: $(tr '\n' ' ' <<<"$serial") fitted $(fit "$serial") bytes a row, bound $BOUND"
diag "col_build bytes by rows, the participant of 3 that held most, at ${PAR_SEG_KB} kB: $(tr '\n' ' ' <<<"$parallel") fitted $(fit "$parallel") bytes a row, bound $PAR_BOUND"
check "serial btree build: every step is under 2.1 * segment budget + 6 MB" test "$over_serial" -eq 0
check "serial btree build: the fitted per-row term is at most $SLOPE_MAX bytes" test "$(fit "$serial")" -le "$SLOPE_MAX"
check "parallel btree build: every participant, at every step, is under 2.1 * segment budget + 6 MB" test "$over_parallel" -eq 0
check "parallel btree build: the fitted per-row term is at most $SLOPE_MAX bytes" test "$(fit "$parallel")" -le "$SLOPE_MAX"
check "both builds read the heap through the btree, the parallel one by workers" test "$paths_ok" -eq 1

# the choice: the walk keeps a node and a hash entry for each block of the simulated cache (at most 65,536 of them), so the bytes grow with the cache up to that and not with the table:
# measured with a cache of none, 2,048 and 4,096 blocks (the table has more blocks than that), the bytes at 65,536 are what the fit says, and are under the budget of the scope
load $BIG
picks=""
for cache in 0 2048 4096; do
	read -r _ pick walk _ _ <<<"$(build "$BIG" 0 "$cache" "$SEG_KB")"
	sql bm "DROP INDEX e_col" >/dev/null
	picks+="$cache $walk $pick"$'\n'
done
p0=$(sed -n 1p <<<"$picks" | cut -d' ' -f2) p2k=$(sed -n 2p <<<"$picks" | cut -d' ' -f2) p4k=$(sed -n 3p <<<"$picks" | cut -d' ' -f2)
scope0=$(sed -n 1p <<<"$picks" | cut -d' ' -f3)
per_block=$(((p4k - p2k) / 2048))
at_max=$((scope0 + per_block * 65536))
diag "col_btree_pick: the walk's bytes and the scope's peak by cache blocks: $(tr '\n' ' ' <<<"$picks") fitted $per_block bytes a block, $at_max at 65536 blocks, budget $PICK_BUDGET"
check "choice: the walk holds bytes at every cache size" test "$p0" -gt 0 -a "$p2k" -gt 0 -a "$p4k" -gt 0
check "choice: the bytes grow with the cache, at most 128 a block (a node and its hash entry)" test "$per_block" -gt 0 -a "$per_block" -le 128
check "choice: the bytes at the largest cache the simulation holds (65,536 blocks) are under the budget of the scope" test "$at_max" -le "$PICK_BUDGET"
check "choice: with no cache the scope is under the bound of the build at the largest size (it is not larger than the run it saves)" test "$scope0" -le "$BOUND"
check "no scope of the btree build went over its budget (the memory gate of a build fails the statement)" test -z "$(grep -c 'sift memory budget exceeded' "$LOG" | grep -v '^0$')"
finish
