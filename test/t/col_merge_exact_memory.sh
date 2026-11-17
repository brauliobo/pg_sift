#!/usr/bin/env bash
# The memory of a forced column merge in exact bytes (scope col_merge of core/membudget.h, sift._test_mem_highwater()), at five small row steps, plain and key_cut:
# the bytes the contexts of the merge hold are a function of the data, not of where the allocator places its blocks, so one run a size proves the model of docs/SIZING.md:
# peak = P + D + C, P the writer of the partition (its point arrays, 77 to 90 bytes a row, until its budget ends the segment: 16 MB at least, so none of these sizes reaches it),
# D the dictionaries the inputs pin, C what the merge creates besides. The per-row term is the least-squares slope over the five steps and may not pass SLOPE_MAX; every
# step stays under INTERCEPT_MAX + SLOPE_MAX * rows. A merge that keeps 64 more bytes of every index row passes neither.
# The backend of each size is a new one with maintenance_work_mem at 1 MB; the shared dictionary ring and the maintenance worker are off, as in mem_slope_legacy.sh.
. "$(dirname "$0")/lib.bash"

SIZES=(8000 16000 24000 32000 40000)
SLOPE_MAX_PLAIN=1 INTERCEPT_MAX_PLAIN=304128
SLOPE_MAX_CUT=444 INTERCEPT_MAX_CUT=3502244

plan 4
node_init me 88 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.col_auto_merge = off
sift.col_dict_shared_cache_kb = 0
sift.async_maintenance = off"
node_start me || bail "start failed"
sql me "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

merge_peak() {
	local n=$1 opt=$2
	sql me "DROP TABLE IF EXISTS e CASCADE" \
		"CREATE TABLE e AS SELECT g AS id, g AS k_hi, g % 5000 AS k_mid, g % 10 AS k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] AS cat,
			'name' || (g::bigint * 7919) % 1000000 AS txt, ((g::bigint * 7919) % 29500)::numeric / 100 AS amount, (g::bigint * 7919 % 29500)::float8 / 100 AS fl,
			timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second' AS ts FROM generate_series(1, $n) g" \
		"ALTER TABLE e ADD PRIMARY KEY (id)" \
		"CREATE INDEX e_col ON e USING sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts) $opt" \
		"VACUUM ANALYZE e" \
		"UPDATE e SET amount = amount + 1 WHERE k_lo < 2" "UPDATE e SET fl = fl + 1 WHERE k_lo BETWEEN 5 AND 6" >/dev/null || bail "setup of $n rows failed"
	sql me "SET maintenance_work_mem = '1MB'" "SET max_parallel_maintenance_workers = 0" "SELECT sift.col_merge('e_col', true)" \
		"SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'col_merge'" | tail -1
}

fit() {
	awk -v max_slope="$2" -v max_icpt="$3" '{ n++; x[n] = $1; y[n] = $2; sx += $1; sy += $2 }
		END { mx = sx / n; my = sy / n; for (i = 1; i <= n; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) ^ 2 }
			s = sxy / sxx; bad = 0; for (i = 1; i <= n; i++) if (y[i] > max_icpt + max_slope * x[i]) bad++
			printf "%.0f %d\n", s, bad }' <<<"$1"
}

measure() {
	local opt=$1 n peak rows=""
	for n in "${SIZES[@]}"; do
		peak=$(merge_peak "$n" "$opt")
		rows+="$n $peak"$'\n'
	done
	printf '%s' "$rows"
}

for mode in plain cut; do
	if [[ $mode == plain ]]; then opt=""; slope_max=$SLOPE_MAX_PLAIN; icpt_max=$INTERCEPT_MAX_PLAIN; else opt="WITH (key_cut = 'k_mid')"; slope_max=$SLOPE_MAX_CUT; icpt_max=$INTERCEPT_MAX_CUT; fi
	rows=$(measure "$opt")
	diag "col_merge scope bytes by rows, $mode: $(tr '\n' ' ' <<<"$rows")"
	read -r slope bad <<<"$(fit "$rows" "$slope_max" "$icpt_max")"
	diag "$mode: fitted $slope bytes a row (ceiling $slope_max)"
	check "$mode: the fitted per-row term is at most $slope_max bytes" test "$slope" -le "$slope_max"
	check "$mode: every step is under $icpt_max + $slope_max * rows bytes" test "$bad" -eq 0
done
finish
