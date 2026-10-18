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
| Aggregates | `count(*)`, `count(x)`, `count(DISTINCT x)` (dictionary/integer columns), `count(DISTINCT pk)` at element level (distinct parent rows, proven by a unique NOT NULL column), `sum`/`avg` of int2/int4 and of float8 (also float4 for `avg`), `sum`/`avg` of float8 expressions built of `+ - *` over leaves (`sum(qty * (price - discount))`), `min`/`max` of numbers and dates, each with `FILTER` |
| Above | `HAVING`, projection, ORDER BY, LIMIT, and window functions of an outer query stay in the normal plan on top of the custom scan |

Unproven means no path. Not matched today: `sum(float4)` (PostgreSQL adds in float4), `sum`/`avg` of `numeric`, `min`/`max` of text, `percentile_*`, aggregates with DISTINCT/ORDER BY other than the two `count(DISTINCT)` forms, grouping sets, volatile expressions, columns that are not in the index, joins to other tables (lookup tables, semi joins), CTEs and `UNION` arms, `EXISTS`/`@>`/`jsonb_path_exists` over elements, expressions of more than one column as a key or filter, nested aggregation of the unnested rows, and the BM25 `@@>` mask. Each is a documented gap, not a silent difference: the query keeps its plan.

## Executor

One unit at a time (the rows of a segment inside one superblock), through the seam of `src/sift_colsrc.h`:

1. row predicates to a bitmap with the kernel table (`cmp_*`, `lut_*`, bitmap operations); dictionary predicates are evaluated once per distinct value of the unit's dictionary;
2. with an unnest, element predicates in vectors of 4096 elements, the row bitmap lifted to the elements through their parent ordinals;
3. `sift_colsrc_live` and `sift_colsrc_vet`: dead rows out, then, after the value filters, the rows that the snapshot does not see (all-visible pages pass, the others are read in the heap; a `sift_table` table has nothing to check);
4. group ids: dictionary codes and integer values map to dense key ids through tables built once per dictionary / value range; ids combine into a direct array (up to 2^20 groups) or a hash table beyond;
5. aggregates accumulate by group in tuple order.

The groups leave as the rows of `custom_scan_tlist` (keys, then aggregates); the plan above projects and filters. `Rows Vetted By Heap`, `Heap Pages`, `Units`, `Units Pruned`, `Rows Scanned`, `Tuples Aggregated` and `Groups` are in EXPLAIN ANALYZE, the filters, group keys and pushed aggregates in plain EXPLAIN.

## Exactness

Integer, date, timestamp, dictionary and boolean results are exact. `sum(int)` is int8, `avg(int)` is the `numeric` division of the exact sum and count. Float `sum` and `avg` add in tuple order per group, so the result does not depend on the SIMD level or the number of workers; it can differ from the normal plan, which adds in heap order, by the rounding of reordered float addition (the comparator's tolerance: relative 1e-9, absolute 1e-12).

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
| G7 percentile | 1017 | 998 | 1.0 | not matched |
| S5@K4000 | 109 | 33.8 | 3.2 | count(DISTINCT customer) |
| S5@K10, S5@K400 | 0.9, 10.3 | 1.0, 12.0 | 0.9 | normal plan kept (btree) |
| S1 to S4, S6, S7 at K10, K400, K4000 | | | 0.7 to 1.3 | not matched, normal plan, no regression beyond noise |

The comparator found no difference in the 145 corpus runs, 395 fixture runs and the 13 DML/maintenance scenarios (insert, update, delete, abort, vacuum, HOT, TID recycling, held writers and snapshots, reindex, promote), on the `sift_col` index and on the test provider, at `sift.simd` scalar and auto, with 0 and 2 workers (the scan itself is not parallel). The one fixture that differs is the error code above.

## Open

- Shapes S1 to S4, S6, S7 and G7 (lookup joins to mapping tables, CTE/subquery inlining, `EXISTS` and `jsonb_path_exists` over the same element, cross-leaf `CASE`, `numeric` sums, percentiles).
- Parallel scan: morsels over `sift_colsrc_nsb`, partial aggregation per worker, final combine; the group tables merge by key id values (not by dictionary codes).
- Zone pruning from the query's predicates (`SiftSrcPrune` is in the seam, no provider uses it): this is what makes selective store sets cheap on a clustered table.
- `count(DISTINCT)` is a hash set per aggregate (about 25 ns per tuple): a per-group bitmap or sort-based form for dense domains.
- A lookup-table expression (a key or a filter that is not a plain comparison) over an integer column whose values span more than 2^22 raises an error with a hint to switch `sift.enable_agg` off; plain group keys, comparisons and `IN` lists over such columns work (hash ids, binary search).
- Statistics of the `sift_col` provider (distinct values and elements per row per leaf) once the engine exposes them.
