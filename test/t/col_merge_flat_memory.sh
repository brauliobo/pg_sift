#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A column merge holds a bounded amount of memory whatever the rows it merges, in exact bytes per memory context (scopes col_merge, col_merge_window, col_writer_spool,
# col_writer_order of core/membudget.h, and the kinds of context of the tree of col_merge, sift._test_mem_kinds()), at six row steps, plain and key_cut, 12 promoted inputs and
# the one the build wrote (13 inputs). The 8-page-superblock flavour of scripts/scaling-gate.sh holds a few thousand units in the steps, and sift.test_limits lowers the three
# floors that hide the budgets at small sizes (writer_min_budget, window_min_bytes, part_min_kb, eset_rows of core/limits.h: lowered together, a writer that cuts at its budget leaves
# segments the partition rule would merge again) at maintenance_work_mem 1 MB, the least:
#   n = inputs, U = SIFT_SB_PAGES * MaxHeapTuplesPerPage rows of a unit, R = bytes a decoded row of the batches holds (RB), F = the writer's budget (writer_min_budget), mwm = 1 MB
#   k = the inputs open together: those whose key range holds the key the merge is at (a key_cut merge opens an input, decodes its first unit and parses its dictionaries, when
#       its rows reach the lowest key of that input, from the zones of its first unit: col/v1/order.c wake_inputs; sift._test_col_stream_dicts() counts the dictionaries held at once)
#   window  <= max(mwm / 4, window_min_bytes) + 2 * n * U * R      the units of a step and the batch they are folded into (col/v1/maint/merge_window.c)
#   chunks  <= (2 * k + 4) * U * R                                 key_cut: the loaded unit of each open input and the one before it (col/v1/order.c MergeChunk), the unit the cut writes and its
#                                                                  window of references (col/v1/order.c cut_rows); 512 kB without a key (the merge's own state: its inputs' arrays, the cursors)
#   writer  <= 6 * F                                               the partition in hand, with its dictionaries and point arrays (the kinds of the partition, col writer dict and col writer points): the budget of the merge is the larger of F and the rows of a partition times sift_colw_row_memory
#                                                                  (col/v1/maint/merge.c), up to 4 F for these rows (a partition is sized by its bytes, 12 to 42 for each row of the inputs), and the
#                                                                  allocator holds up to 1.5 of the writer's estimate
#   spool, order <= 4 * F                                          the order arrays: a buffer of eset_rows entries (16 bytes each) for each leaf and region, the rest spilled to files
#   dicts   <= 2 * the bytes of the k largest inputs               the parsed dictionaries of the string leaves of the open inputs, each given back at its last unit: none is left at the end of the merge
#   inputs  <= dicts + n * 32 kB                                   the scope col_merge_inputs: the dictionaries and the open segments (header, unit directory and zones) of the inputs
#   tree    <= window + chunks + writer + dicts + n * 32 kB + 1 MB the whole tree of col_merge at its largest, the sum of the above and the state of the inputs
# What the terms do not do is follow the rows merged: every kind but the dictionaries is flat once each input has a unit (the fit starts at the first size at which each promoted input holds a unit of U rows: SATURATED), and the
# dictionaries are those of the open inputs: k is 13 when every input holds the whole range of the key (the second mode), one when the inputs hold a key range each (the third: 13 inputs of
# the same rows, each of a range of its own, of which a merge holds one at a time; sift.test_col_merge_eager, which opens every input at the first row, holds all 13 and fails the
# bound, with the same segments). The per-row term fitted over the steps is that of the non-dictionary kinds only, which the budgets bound: it may not pass SLOPE_MAX; the bytes of the
# dictionaries follow the bytes of the open inputs, which are the segments' size (the writer's budget when they were written), not the rows merged.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

SIZES=(12000 36000 60000 96000 132000 168000)
declare -A SATURATED=([plain]=36000 [cut]=60000 [disjoint]=132000)
INPUTS=12
STR_LEAVES=3
SB_PAGES=8 TUPLES_PAGE=291
F=262144 WMIN=65536 PMIN=64 MWM_KB=1024
RB=128
SLOPE_MAX=8
ESET=1024
LIMITS="eset_rows=$ESET"

plan 20
node_init mf 90 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 128MB
max_wal_size = 2GB
sift.shared_cache_kb = 0
sift.async_maintenance = off
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start mf || bail "start failed"
sql mf "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

# row KMID: the rows of generate_series g with the key expression KMID
row() { echo "g, g, $1, g % 10, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8], 'name' || (g::bigint * 7919) % 1000000,
	((g::bigint * 7919) % 29500)::numeric / 100, (g::bigint * 7919 % 29500)::float8 / 100, timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second'"; }

# table ROWS KEY_CUT [disjoint]: half of the rows in the build, the rest promoted in INPUTS segments; disjoint: ROWS / (INPUTS + 1) rows in the build and in each of them, each of a range of the key of its own
table() {
	local n=$1 cut=$2 half=$(($1 / 2)) per i lo hi kb="g % 5000" kp
	[[ ${3:-} != disjoint ]] || { half=$((n / (INPUTS + 1))); kb="1000000 + g % 1000"; }
	per=$((half / INPUTS + 1)); [[ ${3:-} != disjoint ]] || per=$half
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

# merge_run [EAGER]: "key value" lines of one forced merge: the inputs, the dictionaries held at once, the peaks of the scopes, the tree of col_merge by kind and the trace of its bytes
# along the merge in percent; EAGER on: sift.test_col_merge_eager
merge_run() {
	sql mf "SET maintenance_work_mem = '1MB'" "SET max_parallel_maintenance_workers = 0" "SET sift.test_limits = '$LIMITS'" "SET sift.test_col_partition_kb = $PMIN" "SET sift.test_mem_kinds = on" "SET sift.test_mem_trace = on" \
		"SET sift.test_col_merge_eager = ${1:-off}" \
		"SELECT 'inputs ' || count(*) || E'\nbytes ' || sum(bytes) FROM sift.col_segments('e_col') WHERE kind = 'immutable'" \
		"SELECT 'inb ' || bytes FROM sift.col_segments('e_col') WHERE kind = 'immutable' ORDER BY bytes DESC" \
		"SELECT 'reset ' || parsed FROM sift._test_col_stream_dicts(true)" \
		"SELECT sift.col_merge('e_col', true) >= 0" \
		"SELECT 'open ' || peak || E'\nparsed ' || parsed || E'\nheld ' || live FROM sift._test_col_stream_dicts()" \
		"SELECT scope || ' ' || peak FROM sift._test_mem_highwater() WHERE peak > 0" \
		"SELECT 'tree ' || bytes FROM sift._test_mem_kinds() WHERE scope = 'col_merge' AND kind = '(tree)'" \
		"SELECT replace(kind, ' ', '_') || ' ' || bytes FROM sift._test_mem_kinds() WHERE scope = 'col_merge' AND kind <> '(tree)'" \
		"WITH t AS (SELECT seq, sum(bytes) FILTER (WHERE kind = '(tree)') AS tot, coalesce(sum(bytes) FILTER (WHERE kind = 'sift_col stream dictionary'), 0) AS dict
				FROM sift._test_mem_trace() WHERE scope = 'col_merge' GROUP BY seq),
			r AS (SELECT *, ntile(100) OVER (ORDER BY seq) AS pct FROM t)
		 SELECT 'mid_flat ' || coalesce(max(tot - dict) FILTER (WHERE pct BETWEEN 25 AND 60), 0) || E'\nend_flat ' || coalesce(max(tot - dict) FILTER (WHERE pct > 60), 0) ||
			E'\nmid_dict ' || coalesce(max(dict) FILTER (WHERE pct BETWEEN 25 AND 60), 0) || E'\nend_dict ' || coalesce(max(dict) FILTER (WHERE pct > 60), 0) ||
			E'\nlast_dict ' || coalesce((SELECT dict FROM t ORDER BY seq DESC LIMIT 1), -1) ||
			E'\nmax_dict ' || coalesce(max(dict), 0) FROM r" \
		"SELECT 'segs ' || count(*) || E'\nafter ' || coalesce(sum(bytes), 0) FROM sift.col_segments('e_col') WHERE kind = 'immutable'" \
		"SELECT 'answers ' || (md5(string_agg(tid::text || val, ',' ORDER BY tid)) = (SELECT md5(string_agg(ctid::text || txt, ',' ORDER BY ctid)) FROM e))
			FROM sift._test_col_read('e_col', 4)" \
		"SELECT 'units ' || md5(string_agg(sb || ':' || nrows || ':' || min || ':' || max, ',' ORDER BY sb)) FROM sift._test_col_zones('e_col', 1)"
}

val() { awk -v k="$2" '$1 == k { print $2; found = 1 } END { if (!found) print 0 }' <<<"$1"; }


# top OUT K: the bytes of the K largest inputs
top() { awk -v k="$2" '$1 == "inb" && ++i <= k { s += $2 } END { print s + 0 }' <<<"$1"; }

# fit ROWS_AND_BYTES: the least-squares slope of the second column over the first, bytes a row
fit() { awk 'NF { n++; x[n] = $1; y[n] = $2; sx += $1; sy += $2 } END { mx = sx / n; my = sy / n; for (i = 1; i <= n; i++) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) ^ 2 } printf "%.0f\n", sxy / sxx }' <<<"$1"; }

U=$((SB_PAGES * TUPLES_PAGE))
for mode in plain cut disjoint; do
	cut=off; [[ $mode == plain ]] || cut=k_mid
	# 13 segments of 0.5 to 1.5 partitions, apart in the key, are the minimum a forced merge leaves (the merge writes nothing): the disjoint steps are all past it
	sizes=("${SIZES[@]}"); [[ $mode != disjoint ]] || sizes=(132000 168000 204000 240000 276000)
	open_max=$((INPUTS + 1)); [[ $mode != disjoint ]] || open_max=1; [[ $mode != plain ]] || open_max=2
	flat="" dictfit="" over="" tail_ok=1 total_bad=0 win_bad=0 chunk_bad=0 part_bad=0 spool_bad=0 dict_bad=0 dlast_bad=0 segs_bad=0 open_bad=0
	for n in "${sizes[@]}"; do
		table "$n" "$cut" "$mode"
		out=$(merge_run) || bail "merge of $n rows failed"
		inputs=$(val "$out" inputs); inbytes=$(val "$out" bytes); open=$(val "$out" open)
		win=$(val "$out" col_merge_window); tree=$(val "$out" tree); dict=$(val "$out" sift_col_stream_dictionary)
		chunks=$(val "$out" sift_col_merge); part=$(($(val "$out" sift_col_merge_partition) + $(val "$out" sift_col_writer_dict) + $(val "$out" sift_col_writer_points))); wdict=$(val "$out" sift_col_writer_dict); spool=$(val "$out" col_writer_spool); order=$(val "$out" col_writer_order)
		unit=$((inputs * U * RB))
		win_max=$((MWM_KB * 1024 / 4 > WMIN ? MWM_KB * 1024 / 4 : WMIN)); win_max=$((win_max + 2 * unit))
		chunk_max=$(((2 * open_max + 4) * U * RB)); [[ $mode != plain ]] || chunk_max=$((512 * 1024))
		part_max=$((6 * F)); spool_max=$((4 * F)); dict_max=$((2 * $(top "$out" "$open_max")))
		in_max=$((dict_max + inputs * 32768))
		tree_max=$((win_max + chunk_max + part_max + dict_max + inputs * 32768 + 1048576))
		diag "$mode $n rows, $inputs inputs of $inbytes bytes: tree $tree (window $win, chunks $chunks, writer $part (its dictionaries $wdict), dictionaries $dict, spool $spool, order $order), scope col_merge_inputs $(val "$out" col_merge_inputs) of at most $in_max, segments after $(val "$out" segs)"
		diag "$mode $n rows trace: flat part mid $(val "$out" mid_flat) end $(val "$out" end_flat), dictionaries mid $(val "$out" mid_dict) end $(val "$out" end_dict) last $(val "$out" last_dict); $open held at once (the most), $(val "$out" parsed) parsed, the $open_max largest inputs hold $(top "$out" "$open_max") bytes"
		((tree <= tree_max)) || total_bad=$((total_bad + 1))
		((win <= win_max)) || win_bad=$((win_bad + 1))
		((chunks <= chunk_max)) || chunk_bad=$((chunk_bad + 1))
		((part <= part_max)) || part_bad=$((part_bad + 1))
		((spool <= spool_max && order <= spool_max)) || spool_bad=$((spool_bad + 1))
		((dict <= dict_max && $(val "$out" col_merge_inputs) <= in_max)) || dict_bad=$((dict_bad + 1))
		((open <= open_max * STR_LEAVES)) || open_bad=$((open_bad + 1))
		((inputs == INPUTS + 1)) || segs_bad=$((segs_bad + 1))
		((tree > 0)) || segs_bad=$((segs_bad + 1))
		[[ $(val "$out" answers) == true ]] || segs_bad=$((segs_bad + 1))
		((($(val "$out" last_dict)) == 0)) || dlast_bad=$((dlast_bad + 1))
		((n < SATURATED[$mode])) || flat+="$n $((tree - dict))"$'\n'
		dictfit+="$n $(val "$out" max_dict)"$'\n'
	done
	lazy_units=$(val "$out" units) lazy_after=$(val "$out" after) lazy_segs=$(val "$out" segs) lazy_dict=$(val "$out" max_dict)
	slope=$(fit "$flat")
	out=""
	diag "$mode: non-dictionary bytes of the tree by rows from ${SATURATED[$mode]}: $(tr '\n' ' ' <<<"$flat")fitted $slope bytes a row; dictionaries by rows: $(tr '\n' ' ' <<<"$dictfit")fitted $(fit "$dictfit") bytes a row"
	check "$mode: every merge had $((INPUTS + 1)) inputs and left the rows of the heap" test "$segs_bad" -eq 0
	check "$mode: the tree of col_merge is under the sum of its terms at every step" test "$total_bad" -eq 0
	check "$mode: the window, the chunks, the writer, the order arrays, the dictionaries and the scope of the inputs are under their terms at every step" test "$((win_bad + chunk_bad + part_bad + spool_bad + dict_bad))" -eq 0
	check "$mode: the fitted per-row term of the tree without its dictionaries is at most $SLOPE_MAX bytes" test "$slope" -le "$SLOPE_MAX"
	check "$mode: no dictionary is left at the last step of the merge" test "$dlast_bad" -eq 0
	check "$mode: at most $open_max inputs hold their dictionaries at once, at every step" test "$open_bad" -eq 0
	[[ $mode == disjoint ]] || continue
	table "$n" "$cut" "$mode"
	out=$(merge_run on) || bail "eager merge of $n rows failed"
	diag "disjoint $n rows, every input opened at the first row (sift.test_col_merge_eager): $(val "$out" open) dictionaries held at once, dictionaries $(val "$out" max_dict) bytes against $lazy_dict, segments $(val "$out" segs) of $(val "$out" after) bytes against $lazy_segs of $lazy_after"
	check "disjoint: opening every input at the first row (the mutant) holds all the dictionaries and breaks the bound of the open inputs" test "$(val "$out" open)" -gt "$((open_max * STR_LEAVES))" -a "$(val "$out" max_dict)" -gt "$dict_max"
	check "disjoint: the merge of the waiting inputs leaves the segments, the units and the rows of the merge that opens them all" test "$(val "$out" segs)" -eq "$lazy_segs" -a "$(val "$out" after)" -eq "$lazy_after" -a "$(val "$out" units)" = "$lazy_units" -a "$(val "$out" answers)" = true
done
finish
