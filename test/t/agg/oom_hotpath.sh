#!/usr/bin/env bash
# The memory of an aggregate scan stays flat in the rows it reads: each case runs over a table of 20,000 rows and over one of 100,000, each in a new backend, and the peak
# of the resident memory the backend samples (sift._test_rss_peak_start) may grow from the first to the second by what work_mem bounds (twice it: the window holds its
# run and its queue of rows under it) and 1 MB. The window is planned at work_mem 4MB, the other cases run at 64kB. Before Spec 105 each of them held memory for
# every row until the statement ended: the numeric results of window functions (in the query context), date_trunc('month', date) of an unnest over more distinct days
# than its cache held, the result set of jsonb_each_text read again from the heap (a TupleDesc and a tuplestore a row), and the cells of the rows of a hybrid OR read
# from the heap.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init ah 86 "autovacuum = off
max_parallel_workers_per_gather = 0
sift.async_maintenance = off"
node_start ah || bail "start failed"
sql ah "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

declare -A ROWS=()
# pair NAME SMALL LARGE COLUMNS SELECT INDEXES: NAME_s with the first SMALL rows of SELECT (over i), NAME_l with LARGE; INDEXES with {T} for the table
pair() {
	ROWS[$1_s]=$2
	ROWS[$1_l]=$3
	for t in "$1_s" "$1_l"; do
		sql ah "CREATE TABLE $t ($4) WITH (autovacuum_enabled = false)" "INSERT INTO $t SELECT $5 FROM generate_series(1, ${ROWS[$t]}) i" "${6//\{T\}/$t}" \
			"VACUUM (FREEZE, ANALYZE) $t" >/dev/null || bail "table $t failed"
	done
}
pair wn 20000 100000 "id int, g int, x numeric(10,2)" "i, i % 4, (i % 997) / 8.0" "CREATE INDEX ON {T} USING sift_col (g, id, x)"
pair um 20000 100000 "id int, d date, items jsonb" "i, date '2000-01-01' + i % 10000, jsonb_build_array(jsonb_build_object('kind', 'k' || i % 5, 'qty', i % 7))" \
	"CREATE INDEX ON {T} USING sift_col (id, d, items) WITH (paths = 'items[]{kind text, qty int4}')"
pair ue 20000 100000 "id int, m jsonb" "i, jsonb_build_object('a' || i % 3, to_jsonb((i % 7)::text), 'b', i % 5)" "CREATE INDEX ON {T} USING sift_col (id, m) WITH (paths = 'm.* float8')"
pair ho 20000 100000 "id int, tenant int, region text, amt numeric(10,2), body text" \
	"i, 1 + i % 400, 'region ' || repeat('r', 40) || i % 6, (i % 1000) / 4.0, concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, 'tok' || i % 11)" \
	"CREATE INDEX ON {T} USING sift (body); CREATE INDEX ON {T} USING sift_col (tenant, id, region, amt)"

settings() { echo "SET seq_page_cost = 10000; SET random_page_cost = 10000; SET effective_cache_size = '8kB'; SET enable_seqscan = off; SET enable_bitmapscan = off; SET work_mem = '${1}kB';"; }

# peak WORK_MEM_KB QUERY: the peak resident kB of a new backend above what it held before QUERY; the rows go nowhere
peak() {
	psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir ah)/sock" -p "$(port ah)" -U postgres -d postgres <<-SQL | sed -n 's/^PEAK|//p'
		$(settings "$1")
		SELECT sift._test_thp_disable();
		SELECT sift._test_malloc_stats(true) IS NOT NULL;
		SELECT sift._test_rss_peak_stop() IS NOT NULL;
		SELECT 'BASE|' || sift._test_rss_peak_start();
		\o /dev/null
		$2;
		\o
		SELECT 'PEAK|' || sift._test_rss_peak_stop();
	SQL
}

# flat DESC TABLE WORK_MEM_KB NODE QUERY: QUERY ({T} for the table) runs as NODE, and its peak over TABLE_l is at most twice work_mem and 1 MB above its peak over TABLE_s
flat() {
	local s l max=$((2 * $3 + 1024))
	sql ah "$(settings "$3")" "EXPLAIN (COSTS OFF) ${5//\{T\}/$2_l}" | grep -q "$4" || { not_ok "$1: the plan has no $4"; return; }
	s=$(peak "$3" "${5//\{T\}/$2_s}")
	l=$(peak "$3" "${5//\{T\}/$2_l}")
	out="${ROWS[$2_s]} rows: ${s:-?} kB, ${ROWS[$2_l]} rows: ${l:-?} kB"
	diag "$1: $out"
	check "$1 holds the same memory over ${ROWS[$2_l]} rows as over ${ROWS[$2_s]} (+${s:+$((l - s))} kB, at most $max)" test "${l:-999999}" -le "$((${s:-0} + max))"
}

flat "numeric window functions" wn 4096 SiftAggV6Window "SELECT id, sum(x) OVER w, avg(x) OVER w, var_pop(x) OVER w, stddev_samp(x) OVER w FROM {T} WINDOW w AS (PARTITION BY g ORDER BY id)"
flat "date_trunc('month', date) of an unnest over 10,000 distinct days" um 64 "Unnest: lateral" \
	"SELECT date_trunc('month', d)::date, r.kind, sum(r.qty) FROM {T}, jsonb_to_recordset(items) r(kind text, qty int) GROUP BY 1, 2"
flat "jsonb_each_text of documents read again from the heap" ue 64 "Unnest: lateral" "SELECT e.key, sum(e.value::float8) FROM {T}, jsonb_each_text(m) e GROUP BY 1"
flat "a hybrid OR over text and numeric columns read from the heap" ho 64 "Hybrid Direction: OR" \
	"SELECT region, count(*), sum(amt) FROM {T} WHERE (body @@> 'common' OR tenant = 5) GROUP BY region"
check "the server log has no TRAP" bash -c "! grep -q TRAP '$(dir ah)/postgres.log'"
finish
