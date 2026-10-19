# Plan coverage in 3.1

What to push down next to SiftAgg and the column track, chosen from [coverage.tsv](coverage.tsv) (174 rows: 101 Yes, 19 Partly, 43 No, 11 Error),
the weak spots of the 3.0 benchmarks ([BENCH-GAP.md](BENCH-GAP.md)) and a probe of 66 common analytics shapes on a `sift_col` index and on a
`sift_table` twin (`EXPLAIN` of each, page costs raised as `scripts/coverage.sh` does, so the shape and not the table size decides).

## What the probe found

Almost every aggregate shape already runs as `Custom Scan (SiftAgg)` on both: filters, `FILTER`, `CASE`, expressions, date bucketing, `HAVING`,
`LIMIT`, grouping sets, ordered-set and statistical aggregates, `count(DISTINCT)`, `EXISTS`, `IN (SELECT)`, dimension joins, CTEs, `UNION ALL`,
search + `GROUP BY`. What kept the normal plan:

| shape | row | what the planner does |
|---|---|---|
| a window function over an aggregate: `sum(sum(x)) OVER (ORDER BY month)`, `rank() OVER (ORDER BY count(*))`, `lag(sum(x)) OVER ...`, share of total | W04 | `parse->hasWindowFuncs` stops the hook at the grouping stage, though PostgreSQL plans the window above it |
| `SELECT DISTINCT a, b` (and `count(*)` over it) | K12 | no grouping stage exists; the hook never sees `UPPERREL_DISTINCT` |
| `sift_table`: any aggregate that a `sift_col` index answers from pre-aggregates | AP07 | the table provider has none of the `preagg`, `unit_exact`, `unit_agg`, `unit_hist` callbacks (its sealed units already carry the sidecar) |
| `sift_table`: `ORDER BY col LIMIT n`, and the newest rows of a text match | W13 | the table provider has no `tids`, `zone`, `read_rows`; the match is read by `SiftScan` and sorted |
| `count(DISTINCT)` at 1M rows: 4 workers run 1.5x slower than the leader alone | X02 | the parallel path is not charged for merging the workers' distinct sets |
| `NOT IN (SELECT ...)` | J05 | a hashed `SubPlan` with the NULL rule of `NOT IN` |
| `INTERSECT`, `UNION` (not ALL), a correlated scalar sub-select per dimension row, `DISTINCT ON` | - | planned per arm or per outer row; no gain from one scan |

## The 73 rows that are not Yes, by what blocks them

| class | rows | verdict |
|---|---|---|
| not a function of the data (volatile functions, volatile aggregates, `FOR UPDATE`, `TABLESAMPLE`, row-level security, ctid) | A18 K10 P08 P10 W06 J21 | correct as they are |
| a column the index does not hold (aggregate, key or predicate outside the index) | A27 K11 P09 | the user adds the column to the index |
| type not a leaf of the column index or of a `sift_table` read: `numeric(p>18)`, `char(n)`, `bytea`, `time`, `interval`, `uuid`, `money`, enums, arrays, ranges, `NaN` | T04 T05 T07-T16 T23 T25 | **format decision** (a new leaf type changes the column format and the table layout): not done, listed for the owner |
| jsonb limits (undeclared paths, nested arrays, no paths, jsonpath corner cases) | T18 T21 T22 P12 P13 | shredder and format; not done |
| limits of the plan (more than 5 keys, ROLLUP of more than 4, 4 tables, large joined table, partitions, self join) | K05 K09 J08 J09 J18 J19 J20 | low value or a larger redesign |
| depth-2 cube, per-group `min/max`, float sums in pre-aggregates, units dirtied by churn | AP05 AP06 | **format decision** (the pre-aggregate sidecar) except churn, which is VACUUM policy |
| Top-N limits (text key, expression, join, offset past 100 000, OR of searches) | W09-W12 | low value; W13 below |
| BM25 rows (boosts, ORed queries, facets limits, several columns, parallel scan, errors by design) | S06 S11 S12 S14 S15 S16 S18 E01-E05 | search track, not the column track |
| sift_table limits (127 columns, locks, system columns, logical replication) | L03 L08 L09 L10 | table AM behavior |
| the rest | A17 J05-J07 J11 J13 J16 W04 W14 X02 X05 C01 C02 AP07 | ranked below |

## Ranking

Value is the share of real analytics queries and benchmark cells the shape moves (1 to 5); effort is files, risk and test surface (1 to 5).

| rank | item | row | value | effort | v/e | why |
|---|---|---|---|---|---|---|
| 1 | window functions over an aggregate | W04 | 4 | 1 | 4.0 | month-over-month, running totals, shares and ranks are in most dashboards; one condition in the planner hook |
| 2 | `count(DISTINCT)` worker choice for small inputs | X02 | 3 | 1 | 3.0 | the 1M cells of question 06 (4 workers 1.5x slower than serial); a cost term |
| 3 | pre-aggregates on `sift_table` | AP07 | 5 | 3 | 1.7 | revenue and customers per month scan 198 ms and 508 ms where `sift_col` answers in 0.8 and 5 ms; the sealed units already carry the sidecar, so no format change |
| 4 | `SELECT DISTINCT` as a group-by without aggregates | K12 | 3 | 2 | 1.5 | dimension lists, `count(*)` of distinct rows; one more planner stage |
| 5 | Top-N and newest-match on `sift_table` | W13 | 4 | 4 | 1.0 | the 10 newest matches take 88 ms against 6.6 ms for PostgreSQL at 10M; needs `tids`, `zone`, `read_rows` in the table provider and the planner to accept a table |
| 6 | `NOT IN (SELECT ...)` | J05 | 2 | 3 | 0.7 | `NOT EXISTS` already runs; the NULL rule makes this a second semi-join mode |
| - | RIGHT/FULL JOIN, outer-side tables | J06 J07 | 1 | 4 | 0.3 | rare in aggregates over one fact table |
| - | aggregate over a ranked sub-select | C02 | 2 | 5 | 0.4 | the sub-select is a plan, not a table |
| - | new leaf types, depth-2 cube, per-group min/max | T*, AP05 | 4 | 5 | - | format decisions: not implemented |

Done: 1 (W04), 4 (K12), 3 (AP07) and 5 (W13), in that order (planner-only items first, the table provider after). Not done: 2 (X02) and 6 (J05). Each item:
a failing regress test first (and a coldiff scenario for the `sift_col` shapes, an isolation spec for the table ones), the change, the coverage row
moved by `scripts/coverage.sh --gen`, instruction counts before and after (`bench/cpu-query.sh`, 100 000 rows).

## Not done

| item | why |
|---|---|
| X02 `count(DISTINCT)` worker choice | at 100 000 rows the default knobs already choose the serial plan; the 1.5x slowdown is measured at 1M rows with the benchmark's lowered `parallel_setup_cost`, which is past the 100 000-row cap of this task, and the cost term would change every parallel cell of the gate. The cost model charges the workers' start-up once; a per-worker term for the duplicated source opening needs a 1M-row calibration first |
| J05 `NOT IN (SELECT ...)` | `NOT EXISTS` already runs on the scan; the NULL rule of `NOT IN` needs a second anti-join mode in the join code for a shape that dashboards rarely use |
| months of a timestamp on `sift_table` | `date_trunc('month', ts)` is evaluated per distinct timestamp; a day-granularity leaf would fix it but adds a leaf to the column format: a format decision, left to the owner |
| depth-2 cube, per-group `min/max`, float sums, new leaf types (`numeric(p>18)`, `uuid`, `interval`, arrays) | change the pre-aggregate sidecar or the column format: not implemented |

## Standard analytics shapes (from an application trial)

Shapes an application's ordinary SQL datasets produce, ranked by how often analytics queries have them. Done: a constant in `GROUP BY` (K15), a
per-item aggregate under an outer aggregate with one arm, with the common table expression `NOT MATERIALIZED`, or with one union inside another
(J13, J14), the generic plan of a prepared statement (P16), a range lookup (`date <@ range`, or the bounds of the lookup row) with no equality
(J22), the value of `jsonb_each` cast to a number (T31) and the subscript `x['k']` of a jsonb column (T32). Not done:

| shape | why |
|---|---|
| `lower(col) IN (...)` is slow to choose, not unsupported | the filter is pushed; PostgreSQL's default 1% estimate for an expression makes the normal plan look cheap. `CREATE STATISTICS s (mcv) ON lower(col) FROM t; ANALYZE t` gives the planner the real share and the scan is chosen |
| the spine of periods `LEFT JOIN` above the aggregate (`spine s LEFT JOIN items t ON t.d <@ (SELECT range ...)`) | the scanned table is on the nullable side of a join to the table that is kept whole: the scan would have to return a group for every row of the other table (a RIGHT JOIN, J06), and the range is a sub-select per row. The form with the aggregate first and the spine joined above it is pushed |
| lookup keyed from a column of another joined lookup (`maps` on `g.type`) | the keys of a lookup are leaves, literals or expressions of leaves; a key read from a second lookup is a second join level (one combo that holds two tables): high effort for a rare shape. A literal or a `CASE` in the query, or a join of `maps` to the scan's own column, is pushed |
| a correlated `LEFT JOIN LATERAL (SELECT ... LIMIT 1)` lookup | the first row of the table is the one a scan of it finds first: PostgreSQL's choice with duplicate keys depends on its plan, so the scan cannot be exact unless the key is unique; the join to the table with the key written as a join is pushed |
| a non-null constant as the key of an arm (`SELECT id, 'z', avg(x) ... GROUP BY id` in the place of `NULL`) | the arms are the cells of one outer key; a constant is a value no arm's group expression has, so the key's value table would take it from the arm: an executor change for a labelled-block shape. `NULL` is pushed |
| `max`/`min` as the inner aggregate of a per-item arm, `FILTER` or `CASE` over a text leaf in one | the inner aggregates are count, sum and avg; the arms are pushed one by one and the outer aggregate runs in PostgreSQL. The fused form with a text leaf is the 3.0 crash fixed in 3.0.1: it waits for that fix to merge |
| `jsonb_exists(x, 'k')` (`x ? 'k'`), `x['k'] = '"v"'` (a comparison of jsonb values), `x['k'] IS NOT NULL`, `(x->'a')::int` over a `float8` leaf | a leaf stores NULL for a missing key and for a JSON null alike, and a jsonb comparison tells a string from a number: they are not a function of the stored values. `x ->> 'k'` and the cast to the leaf's own type are pushed. Declare the path with the type the query casts to (`int4`) for the last one |
| filters on columns or jsonb that the index does not hold (a timestamp, a jsonpath on another jsonb, `IS NOT NULL`, `EXISTS` with a lookup) | not a pushdown: the plan stays the normal one. Checked with the filter in a flat query and in per-item arms, with the common table expression materialized and not: the normal plan is chosen every time and no scan was slower than it on the data of the trial files (the slower runs were the per-item arms that returned a row per item, now fused) |
| `count(DISTINCT x) FILTER` in `GROUPING SETS`, bound parameters with custom plans, top-N per group, period keys (`date_trunc` to quarter or year, `to_char`, `CASE` over `extract`), dimension joins on the scan's columns | already pushed: the aggregate runs in the scan (a window over its few rows runs in PostgreSQL); no change needed |
| jsonb shapes (scalar-array unnest, `?` on scalar arrays, `jsonb_array_length`, `jsonb_typeof`, `->>` casts, regex on `jsonb_each_text`) | owned by the `sift_table` branch |

Float accumulation: the per-item `avg` and `sum` add the elements of an item from the first to the last in double (`docs/AGG.md`, "The comparison contract").
The three-row differences between plans of the normal executor itself on thresholds at a tie are shown there.

## Correctness risks seen

- 3.0 returned wrong rows for `ORDER BY col LIMIT n` on a `sift_col` index when an update had moved a row to an unprobed superblock: the scan decided
  "the column follows the table" from four zone probes and stopped at the first window with nothing better (repro: `col_topn_moved`). Fixed on
  `fix/topn-moved-rows` (for 3.0.1) and included here: a window is read when the zones of its units, taken together from the segment headers, can beat
  the n-th row. The table provider uses the same rule (the new version of an updated row is in the write buffer whatever its value).
- After `UPDATE` or `DELETE` a `sift_table` superblock with a tombstone is scanned, not answered from its pre-aggregates, until VACUUM rewrites the
  segment (`sift.table_dead_rewrite_pct`, default 50): the result is the same, only the speed. The planner prices the share of exact superblocks from
  the write buffers and the dead tuples of the statistics, an estimate.
- A numeric `NaN` in a `sift_table` column used to raise on every pushed shape; the source now hands the executor the overflow bitmap as the index
  does, so only the shapes the executor cannot evaluate with PostgreSQL still raise.

## Results

Instructions per transaction (`bench/cpu-query.sh`, 100 000 rows of the store data, `sift_col` on a heap and `sift_table`, this branch against the
3.0 build it branched from; below 1 is cheaper). Tests: regress `agg_window`, `agg_distinct`, `tbl_preagg`, `tbl_topn`, `col_topn_moved`, isolation
`tbl_preagg`, `tbl_topn`, coldiff fixtures `w_window_*` and `k_select_*` in every scenario.

| item | row | test | query | ratio |
|---|---|---|---|---|
| window over an aggregate | W04 | `agg_window`, `w_window_*` | running sum by month, `sift_col` / rank by country, `sift_col` / `sift_table` | 0.004 / 0.006 / 0.004 |
| `SELECT DISTINCT` | K12 | `agg_distinct`, `k_select_*` | `DISTINCT country, category` `sift_col` / `sift_table`; `count(*)` of `DISTINCT customer_id` | 0.023 / 0.022; 0.096 |
| pre-aggregates on `sift_table` | AP07 | `tbl_preagg`, isolation `tbl_preagg` | country count / category sum / whole table | 0.179 / 0.131 / 0.039 |
| top-N on `sift_table` | W13 | `tbl_topn`, isolation `tbl_topn` | newest 10 / filtered / search | 0.042 / 0.101 / 0.069 |
| constant `GROUP BY` key | K15 | `k_group_const*` | `SELECT 'cur', k, count(*) ... GROUP BY 1, 2` | 0.027 |
| per-item aggregate, one arm | J13 | `s2_per_order_one_arm` | `avg` per item and key, then `avg`, `count(DISTINCT item)` per key | 0.037 |
| per-item aggregate, two arms, `NOT MATERIALIZED` | J13 | `s2_per_order_inlined` | the same with a second per-item arm | 0.048 |
| generic plan, a date range of bound parameters over the unnested items | P16 | `col_agg_generic` | `created >= $1 AND created <= $2`, `force_generic_plan`, `sift_col` | 0.009 |
| range lookup grouped by the lookup column | J22 | `k_range_lookup*` | `JOIN periods p ON created <@ p.range` over the unnested items | 0.024 |
| value of `jsonb_each` cast to a number | T31 | `j_jsonb_value_cast` | `avg(kv.value::float8)` per key | 0.051 |
| nested per-item unions (three arms, one inside another) | J14 | `s2_per_order_nested_union` | `avg` and `count(DISTINCT id)` per key | 0.025 |

Gate on the six store questions (existing cells, `sift_col` and `sift_table`, 1000 transactions, 3 rounds, measured again after the last item): `sift_col` 1.000 to 1.002; `sift_table`
0.997, 1.000, 1.000 and 1.016 (the highest, below 1.02), and 0.175 and 0.128 for the two questions AP07 now answers from the sidecar. The month questions
of `sift_table` stay at 1.0 (362M instructions for the month sum against 0.7M on `sift_col`): the key is evaluated per distinct timestamp, which
needs a day leaf, a format decision. The BM25 gate (`bench/steps/cpu_gate.sh`) was not run: no BM25 code changed.

`sift_col` top-N fix for 3.0.1 (`col_topn_moved`, commit on `fix/topn-moved-rows`): 100 000 rows with a pad column (80 superblocks), `ORDER BY seq [DESC]
LIMIT 10` 1.002 and 0.985, with a filter 0.995; a per-unit check of every window measured 2.8x and was dropped for the header-only zone range.
