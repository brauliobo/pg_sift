#!/usr/bin/env bash
# The memory of the serial CREATE INDEX of a sift_col index in exact bytes (scope col_build of core/membudget.h, sift._test_mem_highwater()), on the 8-page-superblock flavour
# (scripts/scaling-gate.sh) so that a budget of a few MB fills on a few tens of thousands of rows. The floors of the budgets (sift.test_limits: build_serial_budget, writer_min_budget)
# are lowered so that maintenance_work_mem is what cuts the runs and the segments, as it is on a table of hundreds of millions of rows at the default.
#  - key_cut: the rows of a run are held up to half of maintenance_work_mem and the run is sorted and written beside them, so the scope holds 2.1 times its budget (a run) and the files of
#    the ranges a table of many runs is split into (8.3 kB each, at most 512): bound = 1.2 * maintenance_work_mem + 6 MB, and the fitted per-row term over the steps that fill the budget stays under SLOPE_MAX
#  - without key_cut: the writer of a segment holds up to (maintenance_work_mem - 30 MB) / 1.4 as it estimates itself and up to 1.5 times that resident: bound = 1.5 * budget + 1.5 MB
#  - the same rows under the floor of the shipped build (64 MB for the serial build) are not held to maintenance_work_mem, and the same bound fails: the check proves the budget is what holds
# The backend of each size is a new one; the maintenance worker and the merge of the build are off.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

SIZES=(40000 60000 80000 100000 120000)
SLOPE_MAX=32
MB=1048576
LOWER="build_serial_budget=0,writer_min_budget=$MB"

plan 10
node_init cb 89 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
sift.col_auto_merge = off
sift.async_maintenance = off
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start cb || bail "start failed"
sql cb "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

# build_peak ROWS MWM OPTS LIMITS: the bytes the scope of the build held at the most
build_peak() {
	local n=$1 mwm=$2 opt=$3 limits=$4
	sql cb "DROP TABLE IF EXISTS e CASCADE" \
		"CREATE TABLE e WITH (autovacuum_enabled = off) AS SELECT g AS id, g AS k_hi, g % 5000 AS k_mid, g % 10 AS k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] AS cat,
			'name' || (g::bigint * 7919) % 1000000 AS txt, ((g::bigint * 7919) % 29500)::numeric / 100 AS amount, (g::bigint * 7919 % 29500)::float8 / 100 AS fl,
			timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second' AS ts, repeat('x', 400) AS p FROM generate_series(1, $n) g" \
		"VACUUM ANALYZE e" >/dev/null || bail "setup of $n rows failed"
	sql cb "SET max_parallel_maintenance_workers = 0" "SET maintenance_work_mem = '$mwm'" "SET sift.test_limits = '$limits'" \
		"CREATE INDEX e_col ON e USING sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts) $opt" \
		"SELECT peak FROM sift._test_mem_highwater(true) WHERE scope = 'col_build'" | tail -1
}

fit() {
	awk 'NF == 2 { n++; x[n] = $1; y[n] = $2; sx += $1; sy += $2 }
		END { mx = sx / n; my = sy / n; for (i = 1; i <= n; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) ^ 2 }; printf "%.0f\n", sxy / sxx }' <<<"$1"
}

# series MWM_MB OPTS LIMITS BOUND: every step under BOUND bytes, the fitted per-row term at most SLOPE_MAX
series() {
	local mwm=$1 opt=$2 limits=$3 bound=$4 n peak rows="" over=0 slope
	for n in "${SIZES[@]}"; do
		peak=$(build_peak "$n" "${mwm}MB" "$opt" "$limits")
		rows+="$n $peak"$'\n'
		((peak <= bound)) || over=$((over + 1))
	done
	slope=$(fit "$rows")
	diag "col_build bytes by rows at ${mwm} MB ${opt:-plain}: $(tr '\n' ' ' <<<"$rows") fitted $slope bytes a row, bound $bound"
	SERIES_OVER=$over SERIES_SLOPE=$slope
}

for mwm in 8 16; do
	series $mwm "WITH (key_cut = 'k_mid')" "$LOWER" $((mwm * MB * 12 / 10 + 6 * MB))
	check "key_cut at $mwm MB: every step is under 1.2 * maintenance_work_mem + 6 MB" test "$SERIES_OVER" -eq 0
	check "key_cut at $mwm MB: the fitted per-row term is at most $SLOPE_MAX bytes" test "$SERIES_SLOPE" -le "$SLOPE_MAX"
done
for mwm in 36 40; do
	budget=$(((mwm * MB - 30 * MB) * 100 / 140))
	series $mwm "" "$LOWER" $((budget * 3 / 2 + 3 * MB / 2))
	check "plain at $mwm MB: every step is under 1.5 * its writer budget + 1.5 MB" test "$SERIES_OVER" -eq 0
	check "plain at $mwm MB: the fitted per-row term is at most $SLOPE_MAX bytes" test "$SERIES_SLOPE" -le "$SLOPE_MAX"
done
# 1 MB: the files of the ranges climb to their cap, the rest is one batch of superblocks
series 1 "WITH (key_cut = 'k_mid')" "$LOWER" $((12 * MB / 10 + 6 * MB))
check "key_cut at 1 MB: every step is under 1.2 * maintenance_work_mem + 6 MB" test "$SERIES_OVER" -eq 0

series 8 "WITH (key_cut = 'k_mid')" "writer_min_budget=$MB" $((8 * MB * 12 / 10 + 6 * MB))
check "key_cut at 8 MB under the shipped floor of 64 MB: the bound fails (the budget does not hold the rows)" test "$SERIES_OVER" -gt 0
finish
