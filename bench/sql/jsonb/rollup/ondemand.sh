#!/usr/bin/env bash
# ondemand.sh HOST PORT (the database jb of the usage in run.sql): ALTER INDEX ... SET (rollup = ...) on the docs index (rollup = 'off' before), then a query every 100 ms in its own transaction until all units answer
P="psql -h $1 -p $2 -U postgres -X -At -d ${DB:-jb}"
Q="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT e.kind, e.flag, count(*), sum(e.score) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) GROUP BY 1, 2"
$P -c "SET maintenance_work_mem = '1GB'" -c "DROP INDEX IF EXISTS docs_col" -c "CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, scores, items) WITH (paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, scores.* float8, items[]{kind text, sub text, flag text, score float8}', rollup = 'off')" -c "VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) docs" >/dev/null
t0=$(date +%s.%N)
$P -c "ALTER INDEX docs_col SET (rollup = 'items[].kind, items[].flag')" >/dev/null
n=0
while :; do
	out=$($P -c "SET max_parallel_workers_per_gather = 0" -c "SET seq_page_cost = 10000" -c "$Q")
	t=$(date +%s.%N)
	pre=$(grep -o 'Units From Pre-aggregates: [0-9]*' <<<"$out" | grep -o '[0-9]*$'); units=$(grep -oE '^ *Units: [0-9]+' <<<"$out" | grep -o '[0-9]*$')
	n=$((n + 1))
	[[ $n == 1 ]] && echo "first query after the ALTER: $(echo "$t - $t0" | bc) s, $pre of $units units from the rollups"
	[[ $pre == "$units" ]] && { echo "first query answered by the rollups: $(echo "$t - $t0" | bc) s after the ALTER (query $n)"; break; }
	sleep 0.1
done
