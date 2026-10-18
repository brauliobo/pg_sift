# Aggregate pushdown on column indexes (SiftAgg)

Plain SQL over a table with a `sift_col` index (or a `sift_table` table) runs on the column data when the planner can prove the result is the one of the normal plan; everything else keeps the normal plan. `sift.enable_agg` (default on) switches the matcher. The result of a pushed query is the one of the plain plan, row for row: `scripts/coldiff.sh` runs every query both ways and compares (docs/AGG.md).

Files: `src/sift_aggplan.c` (shape matcher, cost), `src/sift_aggspec.[ch]` (what the planner proved, serialized into `custom_private`), `src/sift_agg.c` (executor, EXPLAIN), `src/sift_aggkern*.c` (vectorized operators, `docs/KERNELS.md`), `src/sift_colsrc.h` and `src/sift_aggsrc.c` (the seam to the column data and its provider registry), `src/sift_aggcol.c` (provider over the `sift_col` index), `src/sift_aggstub*.c` (test provider, the `sift_colstub` index AM, `SIFT_TESTING` builds).

## What matches

`create_upper_paths_hook`, stage `UPPERREL_GROUP_AGG`. Query shape: one relation (no inheritance, tablesample or row security) with an index of a provider, at most one lateral unnest of an indexed jsonb array or object (`jsonb_to_recordset`, `jsonb_array_elements`, `jsonb_each`, `jsonb_each_text`), inner joins in the FROM list flattened, no window functions or DISTINCT ON in the aggregating query.

| Part | Supported |
|---|---|
| Columns | an indexed attribute or index expression (`equal()` match), typed jsonb paths (`->`, `->>`, `#>`, `#>>`, casts) as the index declares them, `jsonb_array_length`, the columns of the unnest |
| Quals | `= <> < <= > >=` against constants/params, `IN`/`NOT IN`, `IS [NOT] NULL`, `AND`/`OR`; on `numeric` columns and numeric expressions the same operators, `BETWEEN` and `IN` lists of constants (see Numeric); any other boolean expression of ONE dictionary/integer column becomes a lookup table evaluated by PostgreSQL itself on each distinct value |
| Existential tests | one array of objects, same-element semantics, usable in `WHERE`, in `FILTER` and under `AND`/`OR`/`NOT`: `[NOT] EXISTS (SELECT 1 FROM jsonb_to_recordset / jsonb_array_elements / jsonb_each[_text](col) ... WHERE <element quals>)`; `jsonb_path_exists` / `@?` with a lax `$[*] ? (...)` of `@.key == < <= > >=` against a string (`==`), integer or boolean literal joined by `&&` `\|\|`; `col @> '[{"k": v, ...}, ...]'` and `'[...]' <@ col` with string, integer and boolean members. The element quals are the row quals above over the array's leaves; the test is true when some element passes |
| Group keys | up to 4: a column, or any expression of one dictionary/integer column (`date_trunc`, `lower`, `CASE`, ...) evaluated per distinct value; a `numeric(p <= 18, s)` or `int8` column, or a numeric expression (`round(price, 1)`, `price * qty`, `floor(x)::int8`) ; a float4/float8 key is one group per float value as `float8_eq` has it (below) |
| Grouping sets | `GROUPING SETS`, `ROLLUP`, `CUBE` (any mix, `GROUP BY DISTINCT`) and `GROUPING()`: the sets are what PostgreSQL expanded, all aggregated in the one scan (a row is filtered, decoded and unnested once, then added to the groups of each set), the NULLs of the sets and the `GROUPING()` bits are the planner's own |
| Aggregates | `count(*)`, `count(x)`, `count(DISTINCT x)` (dictionary/integer columns), `count(DISTINCT pk)` at element level (distinct parent rows, proven by a unique NOT NULL column), `sum`/`avg` of int2/int4 and of float8 (also float4 for `avg`), `sum`/`avg` of float8 expressions built of `+ - *` over leaves (`sum(qty * (price - discount))`), `sum`/`avg`/`min`/`max`/`count`/`count(DISTINCT)` and `sum`/`avg(DISTINCT)` of a `numeric(p <= 18, s)` column or of a numeric expression (`sum(price * qty)`, exact, see Numeric), `sum`/`avg`/`percentile_cont` of a `numeric(p <= 15, s)` column cast to float8, `min`/`max` of numbers and dates, `percentile_cont(c)` / `percentile_disc(c) WITHIN GROUP (ORDER BY x)` with a constant fraction in [0, 1] (x: a float8 expression as above, for `percentile_disc` also a float4, int2, int4 or date leaf), each with `FILTER` ; the same over the columns of a lookup join (below) |
| Above | `HAVING`, projection, ORDER BY, LIMIT, and window functions of an outer query stay in the normal plan on top of the custom scan |

Unproven means no path. Not matched today: `sum(float4)` (PostgreSQL adds in float4), numeric division and modulo, numeric functions other than `abs`, `round`, `trunc`, `floor` and `ceil`, comparisons of `numeric` with float8, a `numeric(p, s)` cast, expressions of more than 38 digits (26 for a sum, see Numeric), `min`/`max` of text, the array and interval forms of `percentile_*`, aggregates with DISTINCT/ORDER BY other than the two `count(DISTINCT)` forms, grouping sets, volatile expressions, columns that are not in the index, joins to other tables (lookup tables, semi joins), a CTE the query reads once (PostgreSQL inlines it, so it is the query above), a `UNION` of aggregates in one pass (see Shared CTE), `EXISTS`/`@>`/`jsonb_path_exists` over elements, expressions of more than one column as a key or filter, nested aggregation of the unnested rows, and the BM25 `@@>` mask. Each is a documented gap, not a silent difference: the query keeps its plan.




### Lookup joins, several leaves, per-order aggregates

- **Lookup join**: `[LEFT] JOIN small ON small.k = <leaf>` (one to three equalities of dictionary/integer/date leaves and columns of the table, up to four tables). The table is read at executor start with the query's snapshot; its rows are matched per distinct tuple of leaf values, cached across units. Exact: every matching row replicates the input row (a non-unique table needs no proof), a NULL key matches nothing, text compares as `=` does (deterministic collations), the unmatched rows are one row with NULLs (`LEFT`) or gone (`INNER`). The mapped columns are keys, filters and `CASE`/`coalesce` operands like leaves. `count`, `count(DISTINCT)` (hashable types), `min`, `max` (types with a btree order) and `sum`, `avg` (`numeric`, `float8`) of an expression of the joined columns are taken per matching row: the value is `NULL` for an unmatched `LEFT` row (an `INNER` join drops the row), the distinct values of the expression are kept once, and the aggregate compares them (the type's own comparison function) or adds them (`numeric` through its own functions, so the scale of the result is the normal plan's) when the scan ends.
- **Several leaves**: a key or filter over up to three leaves (of the order and of its items: `CASE WHEN o.lang = 'en' THEN i.category ELSE ... END`) is evaluated by PostgreSQL per distinct tuple of the values, cached across units. `x->>'k' IS [NOT] NULL` on a leaf of another type is the null test of that leaf (a value it cannot hold raises the overflow error above).
- **Key of a free key object**: `(o.metrics->>'overall')::float8` next to the unnest, when the index has `metrics.* float8`, is a row level leaf built per unit from the elements whose key is `overall` (NULL for the rows without it).
- **Per-order aggregates**: `SELECT .. avg(x) .. FROM (SELECT o.id, keys, agg(..) AS x .. GROUP BY o.id, keys) GROUP BY keys` and the same over a `UNION ALL` of up to four such arms (rollup levels over a shared CTE), where each arm groups by the unique id and by outer keys (or a NULL constant). The inner groups are one row's share of an outer group, so the scan keeps the inner aggregates (`count`, `sum`, `avg`, with `FILTER`) of the current row per outer group and adds them to the outer `count(*)`, `count(DISTINCT id)`, `avg`, `sum` (each with `FILTER` over the inner columns) when the row is done. A WHERE on the outer query is taken when PostgreSQL pushed all of it into the arms: quals on the keys filter the rows of an arm, quals on the inner aggregates are the arm's HAVING (a cell that fails it is no group of the arm and adds nothing to the outer aggregates). A qual that stays above the arms (on an expression of several arms, a volatile or a `pseudoconstant` one), grouping sets in the aggregating query, and groups that are not one row's share keep the plan.


## Ordered-set aggregates

`percentile_cont` and `percentile_disc` keep the values of each group exactly and pick the ranks with the final functions' arithmetic: `ceil(c * n)` for `disc`, `floor` and `ceil` of `c * (n - 1)` and `lo + frac * (hi - lo)` in double for `cont`, `DESC` counting from the end, NULL inputs left out, NaN last, `-0` equal to `0`. Aggregates on the same argument share one set of values. The values are an exact histogram of the distinct (group, value) pairs as long as it fits `work_mem` (4 slots of 16 bytes per pair at most); beyond that every value goes through a `tuplesort` in `work_mem` that spills to disk like the normal plan's sort (EXPLAIN ANALYZE: `Ordered Sets Spilled`), so memory is bounded and nothing fails or falls back. A constant fraction outside [0, 1], a fraction that is not a constant, the array forms and other types keep the normal plan (which raises its own error). The overflow errors inside a float8 expression argument are not reproduced (as for `sum`).

## Shared CTE

PostgreSQL computes a CTE that the query names more than once once and hands the rows to every reference, so aggregates over it (S4: a time series at four granularities, each arm a `GROUP BY` of the same CTE) never reach the index. When a CTE with no recursion over a table that has a column index is read twice or more, `sift_aggcte.c` (a `planner_hook`) plans the query again with the CTE `NOT MATERIALIZED` (PostgreSQL itself refuses that for a CTE with a volatile function or data-modifying statement) and keeps that plan only if it has a `Custom Scan (SiftAgg)` and costs 6 times less than the materialized one. The margin is the planner's 100 rows per `jsonb_to_recordset` call against the handful the data has: on 300000 orders the estimates cross where the times do at about 6. Both plans are plans of the same query. Each arm is its own pass over the index (four for S4); one pass computing the four granularities needs a scan with several group sets sharing the filters, decoded leaves and unnest, and a planner step that fuses the arms of the `UNION ALL` (they are planned apart, each with its own PlannerInfo), which is not done.

## Numeric

A `numeric(p, s)` column with `p <= 18` is an INT leaf holding the number times 10^s (docs/COL.md), and the plan treats it as an int64 leaf with a scale: the same comparison kernels, group ids and accumulators as every integer leaf, PostgreSQL's own numeric functions for everything that is not integer arithmetic. An `int8` column is the same leaf with scale 0 (so it is also a group key now). Unconstrained, wider or negative-scale numeric is refused at `CREATE INDEX`, so those columns are never on the index.

**Predicates** (`numeric` column against a constant or a parameter): `= <> < <= > >=`, `BETWEEN`, `IS [NOT] NULL`, `IN`/`NOT IN` of up to 64 constants (an OR of equalities, an AND of inequalities, as PostgreSQL evaluates them: `NOT IN` with a NULL member is never true). The operand is evaluated when the scan starts and scaled with `numeric_mul`, `numeric_floor` and `numeric_ceil`; the operator is rewritten for the stored integers: `x = c` holds for `c * 10^s` when it is an integer and for no value otherwise (`x <> c` for every one), `x < c` is `x < ceil(c * 10^s)`, `x <= c` is `x <= floor(c * 10^s)`, `x > c` is `x > floor(c * 10^s)`, `x >= c` is `x >= ceil(c * 10^s)`. A constant beyond the integer range, NaN (greater than every number in PostgreSQL's numeric order) and the infinities give the same answer for every value of the column; none of them reaches the kernels.

**Expressions** (aggregate arguments, group keys, predicates, `count(DISTINCT)`): `+ - *`, unary minus, `abs`, `round`/`trunc` with a constant number of digits (also negative), `floor`, `ceil`, numeric constants, `int2`/`int4`/`int8` columns cast to numeric, and numeric cast to `int2`/`int4`/`int8`. They run as a program of int128 values scaled by the result's `dscale`, which is what PostgreSQL gives: the larger scale of the operands for `+` and `-`, their sum for `*`, the digits asked for by `round` and `trunc` (0 for a negative count), 0 for `floor` and `ceil`. Two expressions in a comparison are compared as their difference against 0. The planner tracks the digits each step can have from the columns' declared precision and refuses the expression when a step could need more than 38 (int128), a sum or average when its argument could need more than 26 (room for 10^12 rows), a cast to an integer when the digits it keeps could overflow it (PostgreSQL raises the error then). Equality is by value: a distinct count, a group key and a `min` over one expression compare scaled integers of one scale, so `1.0` and `1.00` are the same number. The text of a result is the one of PostgreSQL: a sum, key or extreme is written with the scale above, so trailing zeros are kept.

**Aggregates**: `sum`/`avg`/`min`/`max`/`count`/`count(DISTINCT)` of a column or an expression (`sum(DISTINCT)` and `avg(DISTINCT)` too). `sum` adds in 128 bits, so it is exact however many values there are, and is written as the numeric of the argument's scale (the `dscale` PostgreSQL gives the sum); `avg` is `numeric_div` of that sum and the count (as `numeric_avg` does); `min`/`max` are the value; a group without values is NULL. `sum`, `avg` and `percentile_cont` of `numeric(p <= 15, s)` cast to float8 divide the integer by 10^s in double: below 2^53 that is the correctly rounded value `numeric_float8` computes.

**Not matched, and the rule that blocks it**: division (`numeric_div` picks its scale by `select_div_scale` from the values' leading digits and rounds to at least 16 significant digits, so the result scale is not a function of the operands' scales), `%`, `sqrt`, `power` and every other function (rounded results or errors that are not known here), `numeric` against float8 (a float8 comparison of `numeric_float8` of the column), the `numeric(p, s)` cast (rounds and raises `numeric field overflow` per value), a NaN or infinite constant inside the arithmetic, `IN` with a parameter array or more than 64 constants, `min`/`max(DISTINCT)` and `percentile_*` over numerics (the percentile functions pick from the sorted values; the scan keeps float8 ones only). Every one keeps its plan. A `NaN` in the column is stored as NULL with the overflow flag, so the scan raises `0A000` with a hint like any value the index cannot hold, where the normal plan returns the NaN: set `sift.enable_agg` off for such a table.

On 300000 rows (one core, one `numeric(12, 2)` and one `numeric(18, 6)` column, `sift_col` index, median of 5), plain plan against SiftAgg:

| Shape | Plain ms | SiftAgg ms |
|---|---:|---:|
| `WHERE price > 100.005 AND price <= 900.5`, `count`, `sum(price)` | 19.1 | 1.0 |
| `weight IN (1, 2, 3, 50.5, 4000)` per store | 27.7 | 0.6 |
| `GROUP BY price` (100000 groups) | 40.9 | 16.8 |
| `GROUP BY rate` (300000 groups) | 90.5 | 56.1 |
| `sum(price * qty)`, `avg(price - weight)` per store | 50.9 | 3.1 |
| `GROUP BY round(price, -1)` | 39.0 | 3.5 |
| `count(DISTINCT price)` per store (400 groups) | 129.8 | 15.2 |
| `WHERE price * qty > 5000` per store | 22.2 | 1.8 |

Existential tests keep the normal plan when equality with the plain plan is not provable from the index: jsonpath `!=` and `!()` (a JSON null member is "not equal" to a literal, an absent key is not; the index holds a NULL for both), jsonpath numbers against a float leaf (jsonb numbers compare exactly, the leaf holds the rounded double), strict mode and variables, a literal of another type than the leaf (unknown in jsonpath), `@>` with a JSON null, a float or an empty object, an element compared with the outer row, `LIMIT`/`OFFSET` and a second array in the sub-query. `NOT` of `jsonb_path_exists` or `@>` is NULL for a NULL document; it is taken only for a column with a NOT NULL constraint (`NOT EXISTS` is never NULL). A document the index cannot hold (an object or a string where the array is, a member of another type than declared) raises `0A000` as for any pushed query, where the plain jsonpath and `@>` return a result; a JSON null where `jsonb_to_recordset` raises in the plain plan has no elements here. The element predicate runs on every element of the unit's rows before the other quals are applied.

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

### Grouping sets and float keys

- **Sets**: the scan keeps one group table per set over the same key ids; the set is a key of the groups, so a group of the empty set is one group and a data `NULL` is not the `NULL` of a rolled up key. `GROUPING(a, b)` is the bit mask of the sets (the first argument is the highest bit); the empty set returns its one row over no input too. A query of N sets is priced as N group updates per tuple; the filters, leaf reads and unnest are paid once.
- **Float keys**: a float4/float8 column or expression is a key through its bits, with `-0` as `0` and every NaN as one NaN (`float8_eq` and `hashfloat8`); the index stores `-0` as `0` and a value it cannot hold raises `0A000` as above, and the group is emitted as the type's value (`float4` stays `float4`).

## Cost

`agg_cost` prices the scan from the source's statistics: a leaf read, a predicate, a tuple that passes the filters, a key id and an aggregate update, each in `cpu_operator_cost` (about 3 ns here: the constants were fitted on whole table runs of one to three keys and aggregates, 17 ns per unnested element all in). The scan reads every unit (zone pruning by the query's predicates is not requested yet), so a selective query is priced by its whole table scan against the btree plan's selected rows: the measured crossover keeps the normal plan for the 10 and 400 store sets and takes the scan for the 4000 store set and the whole table. The grouping sets, the mapped aggregates and the outer WHERE add the update cost of their sets and accumulators only: no constant changed. Heap vetting of the rows on pages that are not all-visible is added per page. Elements per row of an array default to 3 when the source has no statistic.

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
| S7@K4000 | 50.1 | 9.3 | 5.4 | `EXISTS` of three conditions on one item |
| S6@K4000 with an integer sum | 32.1 | 7.3 | 4.4 | `jsonb_path_exists` flag in two `FILTER`s; the corpus S6 sums `numeric`: not matched |
| S7@K10, S7@K400 | 0.5, 4.9 | 0.6, 5.2 | 0.9 | normal plan kept (btree) |

The forms of grouping sets, a WHERE on the outer query, mapped aggregates and float keys over the same orders (best of 4 runs of the query, K400 and K4000 stores, `sift.enable_agg` off against on):

| Query | Plain ms | SiftAgg ms | Speedup | Note |
|---|---:|---:|---:|---|
| S3 `ROLLUP (category, period)`, `grouping()` | 23, 218 | 18, 23 | 1.3, 9.3 | S3 itself: 13, 198 against 16, 19 |
| S3 `CUBE (category, period, lang)` | 27, 253 | 13, 19 | 2.1, 13.3 | |
| S1 `ROLLUP (metric, aspect, period)` | 14, 230 | 11, 15 | 1.2, 14.9 | S1 itself: 17, 204 against 9.5, 10.9 |
| S2 with an outer WHERE on the category / on an inner average / both | 41, 757 / 56, 757 / 48, 818 | 17, 45 / 20, 41 / 18, 42 | 2.5, 16.7 / 2.8, 18.3 / 2.6, 19.5 | S2 itself: 39, 687 against 17, 38 |
| `count`, `min`, `max`, `count(DISTINCT)` of a mapped column by language | 14, 167 | 6.7, 8.6 | 2.1, 19.4 | lookup join |
| float8 key (`metrics->>'overall'`) | 3.4, 31 | 7.3, 7.8 | 0.5, 4.0 | K400 is the mis-priced crossover of S3 above (a few ms either way) |
| float8 key of the items (`price`) | 5.8, 106 | 7.5, 19 | 0.8, 5.6 | |

The comparator found no difference in the 145 corpus runs, 545 fixture runs and the 13 DML/maintenance scenarios (insert, update, delete, abort, vacuum, HOT, TID recycling, held writers and snapshots, reindex, promote), on the `sift_col` index and on the test provider, at `sift.simd` scalar and auto, with 0 and 2 workers (the scan itself is not parallel). The one fixture that differs is the error code above.

## Open

- Shapes S1 to S3, S6, S7 (lookup joins to mapping tables, `EXISTS` and `jsonb_path_exists` over the same element, cross-leaf `CASE`) and the one-pass S4 (several group sets in one scan).
- Parallel scan: morsels over `sift_colsrc_nsb`, partial aggregation per worker, final combine; the group tables merge by key id values (not by dictionary codes).
- Zone pruning from the query's predicates (`SiftSrcPrune` is in the seam, no provider uses it): this is what makes selective store sets cheap on a clustered table.
- `count(DISTINCT)` is a hash set per aggregate (about 25 ns per tuple): a per-group bitmap or sort-based form for dense domains.
- A lookup-table expression (a key or a filter that is not a plain comparison) over an integer column whose values span more than 2^22 raises an error with a hint to switch `sift.enable_agg` off; plain group keys, comparisons and `IN` lists over such columns work (hash ids, binary search).
- Statistics of the `sift_col` provider (distinct values and elements per row per leaf) once the engine exposes them.
