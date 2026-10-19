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

Items 1 to 5 are done in this order of risk, not of rank: 1, 4, 2, 3, 5 (items that change the cost model or the table provider come after the
planner-only ones). Item 6 and the rest stay No. Each item: a failing regress test and a coldiff scenario first, the change, the coverage row
moved by `scripts/coverage.sh --gen`, instruction counts before and after (`bench/cpu-query.sh`, at most 100 000 rows).

## Results

Filled in as the items land.
