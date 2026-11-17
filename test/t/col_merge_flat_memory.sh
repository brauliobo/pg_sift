#!/usr/bin/env bash
# A column merge holds a bounded amount of memory whatever the rows it merges, in exact bytes per memory context (scopes col_merge, col_merge_window, col_writer_spool,
# col_writer_order of core/membudget.h, and the kinds of context of the tree of col_merge, sift._test_mem_kinds()), at six row steps, plain and key_cut, 12 promoted inputs and
# the one the build wrote (13 inputs). The 8-page-superblock flavour of scripts/scaling-gate.sh holds a few thousand units in the steps, and sift.test_limits lowers the three
# floors that hide the budgets at small sizes (writer_min_budget, window_min_bytes, part_min_kb, eset_rows of core/limits.h: lowered together, a writer that cuts at its budget leaves
# segments the partition rule would merge again) at maintenance_work_mem 1 MB, the least:
#   n = inputs, U = SIFT_SB_PAGES * MaxHeapTuplesPerPage rows of a unit, R = bytes a decoded row of the batches holds (RB), F = the writer's budget (writer_min_budget), mwm = 1 MB
#   window  <= max(mwm / 4, window_min_bytes) + 2 * n * U * R      the units of a step and the batch they are folded into (col/maint/merge_window.c)
#   chunks  <= 2 * n * U * R                                       key_cut: the loaded unit of each input and the one before it (col/order.c MergeChunk); 512 kB without a key (the
#                                                                  merge's own state: its inputs' arrays, the cursors)
#   writer  <= 6 * F                                               the partition in hand: the budget of the merge is the larger of F and the rows of a partition times sift_colw_row_memory
#                                                                  (col/maint/merge.c), up to 4 F for these rows (a partition is sized by its bytes, 12 to 42 for each row of the inputs), and the
#                                                                  allocator holds up to 1.5 of the writer's estimate
#   spool, order <= 4 * F                                          the order arrays: a buffer of eset_rows entries (16 bytes each) for each leaf and region, the rest spilled to files
#   dicts   <= 3 * the bytes of the inputs                         the parsed dictionaries of the string leaves of the inputs a key_cut merge reads together; a merge of inputs that follow
#                                                                  each other gives each back at its last unit: the end of its trace holds less than its middle
#   tree    <= window + chunks + writer + dicts + n * 32 kB + 1 MB the whole tree of col_merge at its largest, the sum of the above and the state of the inputs
# What the terms do not do is follow the rows merged: every kind but the dictionaries is flat once each input has a unit (the second part of the merge is not above the first),
# the dictionaries are loaded by the first units and none grows after, and none is left at the last checkpoint (the inputs of a third merge hold a range of the key each: the one still
# to come is held from the first unit, as the others, and the ones whose range is done give their dictionary back: what remains in the last 30% is at most 70% of the most held; the
# flat part of that merge is bounded by the terms alone, the unit of the input left being larger than those of the rest). The per-row term fitted over the steps is that of the non-dictionary
# kinds only, which the budgets bound: it may not pass SLOPE_MAX; the bytes of the dictionaries follow the bytes of the inputs, which are the segments' size (the writer's budget
# when they were written), not the rows merged.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

SIZES=(12000 36000 60000 96000 132000 168000)
SATURATED=36000
INPUTS=12
SB_PAGES=8 TUPLES_PAGE=291
F=262144 WMIN=65536 PMIN=64 MWM_KB=1024
RB=128
SLOPE_MAX=8
ESET=1024
LIMITS="writer_min_budget=$F,window_min_bytes=$WMIN,part_min_kb=$PMIN,eset_rows=$ESET"

plan 16
node_init mf 90 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 128MB
max_wal_size = 2GB
sift.col_auto_merge = off
sift.col_dict_shared_cache_kb = 0
sift.async_maintenance = off
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start mf || bail "start failed"
sql mf "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

# row KMID: the rows of generate_series g with the key expression KMID
row() { echo "g, g, $1, g % 10, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8], 'name' || (g::bigint * 7919) % 1000000,
	((g::bigint * 7919) % 29500)::numeric / 100, (g::bigint * 7919 % 29500)::float8 / 100, timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second'"; }

# table ROWS KEY_CUT [disjoint]: half of the rows in the build, the rest promoted in INPUTS segments; disjoint: each of them holds a range of the key of its own
table() {
	local n=$1 cut=$2 half=$(($1 / 2)) per i lo hi kb="g % 5000" kp
	[[ ${3:-} != disjoint ]] || kb="1000000 + g % 1000"
	per=$((half / INPUTS + 1))
	sql mf "DROP TABLE IF EXISTS e CASCADE" \
		"CREATE TABLE e (id int, k_hi int, k_mid int, k_lo int, cat text, txt text, amount numeric, fl float8, ts timestamp)" \
		"INSERT INTO e SELECT $(row "$kb") FROM generate_series(1, $half) g" "ALTER TABLE e ADD PRIMARY KEY (id)" \
		"CREATE INDEX e_col ON e USING sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts) WITH (key_cut = '$cut')" "VACUUM ANALYZE e" >/dev/null || bail "table of $n rows failed"
	for ((i = 0; i < INPUTS; i++)); do
		lo=$((half + i * per + 1)) hi=$((half + (i + 1) * per))
		((hi > n)) && hi=$n
		kp=$kb; [[ ${3:-} != disjoint ]] || kp="$i * 10000 + g % 1000"
		sql mf "INSERT INTO e SELECT $(row "$kp") FROM generate_series($lo, $hi) g" "SELECT sift.col_promote('e_col')" >/dev/null || bail "promotion $i of $n rows failed"
	done
}

# merge_run: "key value" lines of one forced merge: the inputs, the peaks of the scopes, the tree of col_merge by kind and the trace of its bytes along the merge in percent
merge_run() {
	sql mf "SET maintenance_work_mem = '1MB'" "SET max_parallel_maintenance_workers = 0" "SET sift.test_limits = '$LIMITS'" "SET sift.test_mem_kinds = on" "SET sift.test_mem_trace = on" \
		"SELECT 'inputs ' || count(*) || E'\nbytes ' || sum(bytes) FROM sift.col_segments('e_col') WHERE kind = 'immutable'" \
		"SELECT sift.col_merge('e_col', true) >= 0" \
		"SELECT scope || ' ' || peak FROM sift._test_mem_highwater() WHERE peak > 0" \
		"SELECT 'tree ' || bytes FROM sift._test_mem_kinds() WHERE scope = 'col_merge' AND kind = '(tree)'" \
		"SELECT replace(kind, ' ', '_') || ' ' || bytes FROM sift._test_mem_kinds() WHERE scope = 'col_merge' AND kind <> '(tree)'" \
		"WITH t AS (SELECT seq, sum(bytes) FILTER (WHERE kind = '(tree)') AS tot, coalesce(sum(bytes) FILTER (WHERE kind = 'sift_col stream dictionary'), 0) AS dict
				FROM sift._test_mem_trace() WHERE scope = 'col_merge' GROUP BY seq),
			r AS (SELECT *, ntile(100) OVER (ORDER BY seq) AS pct FROM t)
		 SELECT 'mid_flat ' || coalesce(max(tot - dict) FILTER (WHERE pct BETWEEN 25 AND 60), 0) || E'\nend_flat ' || coalesce(max(tot - dict) FILTER (WHERE pct > 60), 0) ||
			E'\nmid_dict ' || coalesce(max(dict) FILTER (WHERE pct BETWEEN 25 AND 60), 0) || E'\nend_dict ' || coalesce(max(dict) FILTER (WHERE pct > 60), 0) ||
			E'\nlast_dict ' || coalesce((SELECT dict FROM t ORDER BY seq DESC LIMIT 1), -1) ||
			E'\nmax_dict ' || coalesce(max(dict), 0) || E'\nlate_dict ' || coalesce(max(dict) FILTER (WHERE pct > 70), 0) FROM r" \
		"SELECT 'segs ' || count(*) FROM sift.col_segments('e_col') WHERE kind = 'immutable'"
}

val() { awk -v k="$2" '$1 == k { print $2; found = 1 } END { if (!found) print 0 }' <<<"$1"; }

# fit ROWS_AND_BYTES: the least-squares slope of the second column over the first, bytes a row
fit() { awk 'NF { n++; x[n] = $1; y[n] = $2; sx += $1; sy += $2 } END { mx = sx / n; my = sy / n; for (i = 1; i <= n; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) ^ 2 } printf "%.0f\n", sxy / sxx }' <<<"$1"; }

U=$((SB_PAGES * TUPLES_PAGE))
for mode in plain cut disjoint; do
	cut=off; [[ $mode == plain ]] || cut=k_mid
	sizes=("${SIZES[@]}"); [[ $mode != disjoint ]] || sizes=(36000 96000 168000)
	late_bad=0
	flat="" over="" dicts="" tail_ok=1 total_bad=0 win_bad=0 chunk_bad=0 part_bad=0 spool_bad=0 dict_bad=0 mid_bad=0 dgrow_bad=0 dlast_bad=0 segs_bad=0
	for n in "${sizes[@]}"; do
		table "$n" "$cut" "$mode"
		out=$(merge_run)
		inputs=$(val "$out" inputs); inbytes=$(val "$out" bytes)
		win=$(val "$out" col_merge_window); tree=$(val "$out" tree); dict=$(val "$out" sift_col_stream_dictionary)
		chunks=$(val "$out" sift_col_merge); part=$(val "$out" sift_col_merge_partition); spool=$(val "$out" col_writer_spool); order=$(val "$out" col_writer_order)
		unit=$((inputs * U * RB))
		win_max=$((MWM_KB * 1024 / 4 > WMIN ? MWM_KB * 1024 / 4 : WMIN)); win_max=$((win_max + 2 * unit))
		chunk_max=$((2 * unit)); [[ $mode != plain ]] || chunk_max=$((512 * 1024))
		part_max=$((6 * F)); spool_max=$((4 * F)); dict_max=$((3 * inbytes))
		tree_max=$((win_max + chunk_max + part_max + dict_max + inputs * 32768 + 1048576))
		diag "$mode $n rows, $inputs inputs of $inbytes bytes: tree $tree (window $win, chunks $chunks, writer $part, dictionaries $dict, spool $spool, order $order) segments after $(val "$out" segs)"
		diag "$mode $n rows trace: flat part mid $(val "$out" mid_flat) end $(val "$out" end_flat), dictionaries mid $(val "$out" mid_dict) end $(val "$out" end_dict) last $(val "$out" last_dict)"
		((tree <= tree_max)) || total_bad=$((total_bad + 1))
		((win <= win_max)) || win_bad=$((win_bad + 1))
		((chunks <= chunk_max)) || chunk_bad=$((chunk_bad + 1))
		((part <= part_max)) || part_bad=$((part_bad + 1))
		((spool <= spool_max && order <= spool_max)) || spool_bad=$((spool_bad + 1))
		((dict <= dict_max)) || dict_bad=$((dict_bad + 1))
		((inputs == INPUTS + 1)) || segs_bad=$((segs_bad + 1))
		[[ $mode == disjoint ]] || ((($(val "$out" end_flat)) <= $(val "$out" mid_flat) * 11 / 10 + 131072)) || mid_bad=$((mid_bad + 1))
		((($(val "$out" end_dict)) <= $(val "$out" mid_dict))) || dgrow_bad=$((dgrow_bad + 1))
		((($(val "$out" last_dict)) == 0)) || dlast_bad=$((dlast_bad + 1))
		((($(val "$out" late_dict)) * 10 <= $(val "$out" max_dict) * 7)) || late_bad=$((late_bad + 1))
		((n < SATURATED)) || flat+="$n $((tree - dict))"$'\n'
	done
	slope=$(fit "$flat")
	out=""
	diag "$mode: non-dictionary bytes of the tree by rows from $SATURATED: $(tr '\n' ' ' <<<"$flat")fitted $slope bytes a row"
	check "$mode: every merge had $((INPUTS + 1)) inputs" test "$segs_bad" -eq 0
	check "$mode: the tree of col_merge is under the sum of its terms at every step" test "$total_bad" -eq 0
	check "$mode: the window, the chunks, the writer, the order arrays and the dictionaries are under their terms at every step" test "$((win_bad + chunk_bad + part_bad + spool_bad + dict_bad))" -eq 0
	check "$mode: the fitted per-row term of the tree without its dictionaries is at most $SLOPE_MAX bytes" test "$slope" -le "$SLOPE_MAX"
	check "$mode: the second part of the merge holds no more than the first once each input has a unit, and no dictionary grows or is left" test "$((mid_bad + dgrow_bad + dlast_bad))" -eq 0
	[[ $mode != disjoint ]] || check "disjoint: an input whose range is done gave its dictionary back: the dictionaries held in the last 30% of the merge are at most 70% of the most held" test "$late_bad" -eq 0
done
finish
