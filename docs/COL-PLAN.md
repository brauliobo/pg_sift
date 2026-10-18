# Aggregate pushdown on column indexes (SiftAgg)

Plain SQL over a table with a `sift_col` index (or a `sift_table` table) runs on the column data when the planner can prove the result is the one of the normal plan; everything else keeps the normal plan. `sift.enable_agg` (default on) switches the matcher. The result of a pushed query is the one of the plain plan, row for row: `scripts/coldiff.sh` runs every query both ways and compares (docs/AGG.md).

Files: `src/sift_aggplan.c` (shape matcher, cost), `src/sift_aggspec.[ch]` (what the planner proved, serialized into `custom_private`), `src/sift_agg.c` (executor, EXPLAIN), `src/sift_aggkern*.c` (vectorized operators, `docs/KERNELS.md`), `src/sift_colsrc.h` and `src/sift_aggsrc.c` (the seam to the column data and its provider registry), `src/sift_aggcol.c` (provider over the `sift_col` index), `src/sift_aggstub*.c` (test provider, the `sift_colstub` index AM, `SIFT_TESTING` builds).

## What matches

`create_upper_paths_hook`, stage `UPPERREL_GROUP_AGG`. Query shape: one relation (no inheritance, tablesample or row security) with an index of a provider, at most one lateral unnest of an indexed jsonb array or object (`jsonb_to_recordset`, `jsonb_array_elements`, `jsonb_each`, `jsonb_each_text`), inner joins in the FROM list flattened, no window functions, grouping sets or DISTINCT ON in the aggregating query.

| Part | Supported |
|---|---|
| Columns | an indexed attribute or index expression (`equal()` match), typed jsonb paths (`->`, `->>`, `#>`, `#>>`, casts) as the index declares them, `jsonb_array_length`, the columns of the unnest |
| Quals | `= <> < <= > >=` against constants/params, `IN`/`NOT IN`, `IS [NOT] NULL`, `AND`/`OR`; any other boolean expression of ONE dictionary/integer column becomes a lookup table evaluated by PostgreSQL itself on each distinct value |
| Group keys | up to 4: a column, or any expression of one dictionary/integer column (`date_trunc`, `lower`, `CASE`, ...) evaluated per distinct value |
| Aggregates | `count(*)`, `count(x)`, `count(DISTINCT x)` (dictionary/integer columns), `count(DISTINCT pk)` at element level (distinct parent rows, proven by a unique NOT NULL column), `sum`/`avg` of int2/int4 and of float8 (also float4 for `avg`), `sum`/`avg` of float8 expressions built of `+ - *` over leaves (`sum(qty * (price - discount))`), `sum`/`avg`/`min`/`max`/`count` of a `numeric(p <= 18, s)` column (exact, see Numeric), `min`/`max` of numbers and dates, `percentile_cont(c)` / `percentile_disc(c) WITHIN GROUP (ORDER BY x)` with a constant fraction in [0, 1] (x: a float8 expression as above, for `percentile_disc` also a float4, int2, int4 or date leaf), each with `FILTER` |
| Above | `HAVING`, projection, ORDER BY, LIMIT, and window functions of an outer query stay in the normal plan on top of the custom scan |

Unproven means no path. Not matched today: `sum(float4)` (PostgreSQL adds in float4), predicates and keys on `numeric`, `min`/`max` of text, the array and interval forms of `percentile_*`, aggregates with DISTINCT/ORDER BY other than the two `count(DISTINCT)` forms, grouping sets, volatile expressions, columns that are not in the index, joins to other tables (lookup tables, semi joins), a CTE the query reads once (PostgreSQL inlines it, so it is the query above), a `UNION` of aggregates in one pass (see Shared CTE), `EXISTS`/`@>`/`jsonb_path_exists` over elements, expressions of more than one column as a key or filter, nested aggregation of the unnested rows, and the BM25 `@@>` mask. Each is a documented gap, not a silent difference: the query keeps its plan.

### Lookup joins, several leaves, per-order aggregates

- **Lookup join**: `[LEFT] JOIN small ON small.k = <leaf>` (one to three equalities of dictionary/integer/date leaves and columns of the table, up to four tables). The table is read at executor start with the query's snapshot; its rows are matched per distinct tuple of leaf values, cached across units. Exact: every matching row replicates the input row (a non-unique table needs no proof), a NULL key matches nothing, text compares as `=` does (deterministic collations), the unmatched rows are one row with NULLs (`LEFT`) or gone (`INNER`). The mapped columns are keys, filters and `CASE`/`coalesce` operands like leaves; aggregates over them are not matched.
- **Several leaves**: a key or filter over up to three leaves (of the order and of its items: `CASE WHEN o.lang = 'en' THEN i.category ELSE ... END`) is evaluated by PostgreSQL per distinct tuple of the values, cached across units. `x->>'k' IS [NOT] NULL` on a leaf of another type is the null test of that leaf (a value it cannot hold raises the overflow error above).
- **Key of a free key object**: `(o.metrics->>'overall')::float8` next to the unnest, when the index has `metrics.* float8`, is a row level leaf built per unit from the elements whose key is `overall` (NULL for the rows without it).
- **Per-order aggregates**: `SELECT .. avg(x) .. FROM (SELECT o.id, keys, agg(..) AS x .. GROUP BY o.id, keys) GROUP BY keys` and the same over a `UNION ALL` of up to four such arms (rollup levels over a shared CTE), where each arm groups by the unique id and by outer keys (or a NULL constant). The inner groups are one row's share of an outer group, so the scan keeps the inner aggregates (`count`, `sum`, `avg`, with `FILTER`) of the current row per outer group and adds them to the outer `count(*)`, `count(DISTINCT id)`, `avg`, `sum` (each with `FILTER` over the inner columns) when the row is done. A WHERE on the outer query, and groups that are not one row's share, keep the plan.


## Ordered-set aggregates

`percentile_cont` and `percentile_disc` keep the values of each group exactly and pick the ranks with the final functions' arithmetic: `ceil(c * n)` for `disc`, `floor` and `ceil` of `c * (n - 1)` and `lo + frac * (hi - lo)` in double for `cont`, `DESC` counting from the end, NULL inputs left out, NaN last, `-0` equal to `0`. Aggregates on the same argument share one set of values. The values are an exact histogram of the distinct (group, value) pairs as long as it fits `work_mem` (4 slots of 16 bytes per pair at most); beyond that every value goes through a `tuplesort` in `work_mem` that spills to disk like the normal plan's sort (EXPLAIN ANALYZE: `Ordered Sets Spilled`), so memory is bounded and nothing fails or falls back. A constant fraction outside [0, 1], a fraction that is not a constant, the array forms and other types keep the normal plan (which raises its own error). The overflow errors inside a float8 expression argument are not reproduced (as for `sum`).

## Shared CTE

PostgreSQL computes a CTE that the query names more than once once and hands the rows to every reference, so aggregates over it (S4: a time series at four granularities, each arm a `GROUP BY` of the same CTE) never reach the index. When a CTE with no recursion over a table that has a column index is read twice or more, `sift_aggcte.c` (a `planner_hook`) plans the query again with the CTE `NOT MATERIALIZED` (PostgreSQL itself refuses that for a CTE with a volatile function or data-modifying statement) and keeps that plan only if it has a `Custom Scan (SiftAgg)` and costs 6 times less than the materialized one. The margin is the planner's 100 rows per `jsonb_to_recordset` call against the handful the data has: on 300000 orders the estimates cross where the times do at about 6. Both plans are plans of the same query. Each arm is its own pass over the index (four for S4); one pass computing the four granularities needs a scan with several group sets sharing the filters, decoded leaves and unnest, and a planner step that fuses the arms of the `UNION ALL` (they are planned apart, each with its own PlannerInfo), which is not done.

## Numeric

A `numeric(p, s)` column with `p <= 18` is an INT leaf holding the number times 10^s (docs/COL.md). `sum`, `avg`, `min`, `max` and `count` of such a column (not of an expression or a cast of it, not `count(DISTINCT)`) are pushed down. `sum` adds the scaled values in 128 bits, so it is exact however many values there are, and is written as the numeric of scale s (the `dscale` PostgreSQL gives the sum of the column's values); `avg` is `numeric_div` of that sum and the count (as `numeric_avg` does); `min`/`max` are the stored value; a group without values is NULL. Predicates and group keys on a numeric column are not matched (a constant would have to be scaled, and a NaN, infinite or int64-overflowing constant refused as `numeric_int8` does). A `NaN` in the column is stored as NULL with the overflow flag, so the scan raises `0A000` with a hint like any value the index cannot hold, where the normal plan returns the NaN: set `sift.enable_agg` off for such a table. Unconstrained, wider or negative-scale numeric is refused at `CREATE INDEX`, so those columns are never on the index.

## Executor

One unit at a time (the rows of a segment inside one superblock), through the seam of `src/sift_colsrc.h`:

1. row predicates to a bitmap with the kernel table (`cmp_*`, `lut_*`, bitmap operations); dictionary predicates are evaluated once per distinct value of the unit's dictionary;
2. with an unnest, element predicates in vectors of 4096 elements, the row bitmap lifted to the elements through their parent ordinals;
3. `sift_colsrc_live` and `sift_colsrc_vet`: dead rows out, then, after the value filters, the rows that the snapshot does not see (all-visible pages pass, the others are read in the heap; a `sift_table` table has nothing to check);
4. group ids: dictionary codes and integer values map to dense key ids through tables built once per dictionary / value range; ids combine into a direct array (up to 2^20 groups) or a hash table beyond;
5. aggregates accumulate by group in tuple order.

The groups leave as the rows of `custom_scan_tlist` (keys, then aggregates); the plan above projects and filters. `Rows Vetted By Heap`, `Heap Pages`, `Units`, `Units Pruned`, `Rows Scanned`, `Tuples Aggregated` and `Groups` are in EXPLAIN ANALYZE, the filters, group keys and pushed aggregates in plain EXPLAIN.

## Exactness

Integer, date, timestamp, dictionary, boolean and numeric results are exact. `sum(int)` is int8, `avg(int)` is the `numeric` division of the exact sum and count. Float `sum` and `avg` add in tuple order per group, so the result does not depend on the SIMD level or the number of workers; it can differ from the normal plan, which adds in heap order, by the rounding of reordered float addition (the comparator's tolerance: relative 1e-9, absolute 1e-12).

PostgreSQL's float8 aggregates raise errors that a plain double sum would hide. `sum(float8)` raises the overflow error when a group's sum is infinite and none of its inputs was. `avg(float8)` keeps a sum of squares (`float8_accum`) that overflows first; the scan does not keep it and raises the same error when the inputs are so large that it could have (`8 * n * max(x)^2 >= DBL_MAX`). The one input where the normal plan does not fail although the bound is met (all values equal and above 1e150) fails here too; set `sift.enable_agg` off for it. Overflow errors inside a float8 expression argument are not reproduced (the result is infinite).

A value the index cannot hold (a jsonb member that does not fit the declared type, text over 255 bytes: the shredder's overflow) makes the scan raise `0A000` with a hint instead of the cast error the normal plan raises on such a value (`22P02`): both fail, the codes differ. The planner refuses leaves the engine marks lossy.

## SIMD

Kernels are chosen at run time through the `sift_cpu` rebind registry and `sift.simd`; the scalar table is the specification and the AVX2 and AVX-512 tables are bit-identical to it, so every result is the same at every level (`docs/KERNELS.md`, "Aggregation kernels"). `sift_aggkern_for(level)` returns a level's table for the differential test. `scripts/coldiff.sh` runs the corpus at `sift.simd = scalar` and `auto`.

## Cost

`agg_cost` prices the scan from the source's statistics: a leaf read, a predicate, a tuple that passes the filters, a key id and an aggregate update, each in `cpu_operator_cost` (about 3 ns here: the constants were fitted on whole table runs of one to three keys and aggregates, 17 ns per unnested element all in). The scan reads every unit (zone pruning by the query's predicates is not requested yet), so a selective query is priced by its whole table scan against the btree plan's selected rows: the measured crossover keeps the normal plan for the 10 and 400 store sets and takes the scan for the 4000 store set and the whole table. Heap vetting of the rows on pages that are not all-visible is added per page. Elements per row of an array default to 3 when the source has no statistic.

## Results

The analytics corpus (`bench/sql/analytics`) on 300000 orders (about 960000 items, one core, `sift_col` index, `scripts/coldiff.sh --bench --runs 5`, median of 5), plain plan against SiftAgg:

| Query | Plain ms | SiftAgg ms | Speedup | Note |
|---|---:|---:|---:|---|
| G1 count, one item filter | 665 | 12.6 | 52.7 | |
| G2 avg/sum/count, three filters | 246 | 15.3 | 16.0 | |
| G3 two keys, avg | 738 | 22.3 | 33.1 | |
| G4 count(DISTINCT order id) | 425 | 17.2 | 24.6 | |
| G5 group by store | 364 | 13.9 | 26.1 | |
| G6 month and language | 848 | 22.5 | 37.7 | |
| G8 group by order (300000 groups) | 509 | 52.4 | 9.7 | |
| G7 percentile | 585 | 47.6 | 12.3 | `percentile_cont` x2, exact histogram |
| S5@K4000 | 109 | 33.8 | 3.2 | count(DISTINCT customer) |
| S5@K10, S5@K400 | 0.9, 10.3 | 1.0, 12.0 | 0.9 | normal plan kept (btree) |
| S1@K400, S1@K4000 | 13.0, 258 | 9.3, 11.7 | 1.4, 22.0 | lookup join, multi-leaf key |
| S2@K400, S2@K4000 | 37.6, 781 | 13.9, 33.2 | 2.7, 23.5 | per-order aggregates, three arms |
| S3@K400, S3@K4000 | 10.6, 229 | 17.9, 20.1 | 0.6, 11.4 | K400 is a plan the cost model mis-prices (the normal plan's estimate is 4 times SiftAgg's, its run 1.7 times faster); K4000 is bound by decoding the leaves it reads (about 20 ms for the whole table) |
| S4@K4000 | 265 | 49.8 | 5.3 | shared CTE planned inline, four passes |
| S4@K10, S4@K400 | 0.9, 12.6 | 1.3, 13.5 | 0.7, 0.9 | materialized plan kept; includes the second planning |
| S6, S7 | | | 0.7 to 1.3 | not matched, normal plan |

The comparator found no difference in the 145 corpus runs, 395 fixture runs and the 13 DML/maintenance scenarios (insert, update, delete, abort, vacuum, HOT, TID recycling, held writers and snapshots, reindex, promote), on the `sift_col` index and on the test provider, at `sift.simd` scalar and auto, with 0 and 2 workers (the scan itself is not parallel). The one fixture that differs is the error code above.

## Open

- Shapes S1 to S3, S6, S7 (lookup joins to mapping tables, `EXISTS` and `jsonb_path_exists` over the same element, cross-leaf `CASE`, predicates on numeric columns) and the one-pass S4 (several group sets in one scan).
- Parallel scan: morsels over `sift_colsrc_nsb`, partial aggregation per worker, final combine; the group tables merge by key id values (not by dictionary codes).
- Zone pruning from the query's predicates (`SiftSrcPrune` is in the seam, no provider uses it): this is what makes selective store sets cheap on a clustered table.
- `count(DISTINCT)` is a hash set per aggregate (about 25 ns per tuple): a per-group bitmap or sort-based form for dense domains.
- A lookup-table expression (a key or a filter that is not a plain comparison) over an integer column whose values span more than 2^22 raises an error with a hint to switch `sift.enable_agg` off; plain group keys, comparisons and `IN` lists over such columns work (hash ids, binary search).
- Statistics of the `sift_col` provider (distinct values and elements per row per leaf) once the engine exposes them.
