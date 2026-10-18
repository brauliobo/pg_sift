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

Unproven means no path. Not matched today: `sum(float4)` (PostgreSQL adds in float4), numeric division, modulo and functions other than the ones in Numeric, comparisons of `numeric` with float8, a `numeric(p, s)` cast, expressions of more than 38 digits (26 for a sum), `min`/`max` of text, the array and interval forms of `percentile_*`, aggregates with DISTINCT/ORDER BY other than the `count(DISTINCT)`, `sum`/`avg(DISTINCT)` of numeric forms, volatile expressions, columns that are not in the index, semi joins and joins that are not lookups of the kind below, a `UNION` of aggregates in one pass (see Shared CTE), nested aggregation of the unnested rows, and the BM25 `@@>` mask. Each is a documented gap, not a silent difference: the query keeps its plan.

### Lookup joins, several leaves, per-order aggregates

- **Lookup join**: `[LEFT] JOIN small ON small.k = <leaf>` (one to three equalities of dictionary/integer/date leaves and columns of the table, up to four tables). The table is read at executor start with the query's snapshot; its rows are matched per distinct tuple of leaf values, cached across units. Exact: every matching row replicates the input row (a non-unique table needs no proof), a NULL key matches nothing, text compares as `=` does (deterministic collations), the unmatched rows are one row with NULLs (`LEFT`) or gone (`INNER`). The mapped columns are keys, filters and `CASE`/`coalesce` operands like leaves. `count`, `count(DISTINCT)` (hashable types), `min`, `max` (types with a btree order) and `sum`, `avg` (`numeric`, `float8`) of an expression of the joined columns are taken per matching row: the value is `NULL` for an unmatched `LEFT` row (an `INNER` join drops the row), the distinct values of the expression are kept once, and the aggregate compares them (the type's own comparison function) or adds them (`numeric` through its own functions, so the scale of the result is the normal plan's) when the scan ends.
- **Several leaves**: a key or filter over up to three leaves (of the order and of its items: `CASE WHEN o.lang = 'en' THEN i.category ELSE ... END`) is evaluated by PostgreSQL per distinct tuple of the values, cached across units. `x->>'k' IS [NOT] NULL` on a leaf of another type is the null test of that leaf (a value it cannot hold raises the overflow error above).
- **Key of a free key object**: `(o.metrics->>'overall')::float8` next to the unnest, when the index has `metrics.* float8`, is a row level leaf built per unit from the elements whose key is `overall` (NULL for the rows without it).
- **Per-order aggregates**: `SELECT .. avg(x) .. FROM (SELECT o.id, keys, agg(..) AS x .. GROUP BY o.id, keys) GROUP BY keys` and the same over a `UNION ALL` of up to four such arms (rollup levels over a shared CTE), where each arm groups by the unique id and by outer keys (or a NULL constant). The inner groups are one row's share of an outer group, so the scan keeps the inner aggregates (`count`, `sum`, `avg`, with `FILTER`) of the current row per outer group and adds them to the outer `count(*)`, `count(DISTINCT id)`, `avg`, `sum` (each with `FILTER` over the inner columns) when the row is done. A WHERE on the outer query is taken when PostgreSQL pushed all of it into the arms: quals on the keys filter the rows of an arm, quals on the inner aggregates are the arm's HAVING (a cell that fails it is no group of the arm and adds nothing to the outer aggregates). A qual that stays above the arms (on an expression of several arms, a volatile or a `pseudoconstant` one), grouping sets in the aggregating query, and groups that are not one row's share keep the plan.


## Ordered-set aggregates

`percentile_cont` and `percentile_disc` keep the values of each group exactly and pick the ranks with the final functions' arithmetic: `ceil(c * n)` for `disc`, `floor` and `ceil` of `c * (n - 1)` and `lo + frac * (hi - lo)` in double for `cont`, `DESC` counting from the end, NULL inputs left out, NaN last, `-0` equal to `0`. Aggregates on the same argument share one set of values. The values are an exact histogram of the distinct (group, value) pairs as long as it fits `work_mem` (4 slots of 16 bytes per pair at most); beyond that every value goes through a `tuplesort` in `work_mem` that spills to disk like the normal plan's sort (EXPLAIN ANALYZE: `Ordered Sets Spilled`), so memory is bounded and nothing fails or falls back. A constant fraction outside [0, 1], a fraction that is not a constant, the array forms and other types keep the normal plan (which raises its own error). The overflow errors inside a float8 expression argument are not reproduced (as for `sum`).

## Shared CTE

PostgreSQL computes a CTE that the query names more than once once and hands the rows to every reference, so aggregates over it (S4: a time series at four granularities, each arm a `GROUP BY` of the same CTE) never reach the index. When a CTE with no recursion over a table that has a column index is read twice or more, `sift_aggcte.c` (a `planner_hook`) plans the query again with the CTE `NOT MATERIALIZED` (PostgreSQL itself refuses that for a CTE with a volatile function or data-modifying statement) and keeps that plan only if it has a `Custom Scan (SiftAgg)` and costs 2 times less than the materialized one, on top of the factor of about 6 that `agg_cost` charges every scan of an unnest (see Cost): the planner assumes 100 rows per `jsonb_to_recordset` call against the handful the data has. S4 keeps the materialized plan for the 400 store set and takes the inline one for the 4000 store set, where it is 5 times cheaper. Both plans are plans of the same query. Each arm is its own pass over the index (four for S4); one pass computing the four granularities needs a scan with several group sets sharing the filters, decoded leaves and unnest, and a planner step that fuses the arms of the `UNION ALL` (they are planned apart, each with its own PlannerInfo), which is not done.

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

## Parallel scan

The planner offers a second path with the workers PostgreSQL would give a parallel sequential scan of the table (`compute_parallel_worker`: table size, the `parallel_workers` reloption, `max_parallel_workers_per_gather`), only when the whole query is parallel safe (`glob->parallelModeOK`, no unsafe function in the spec) and the spec is one of the parallel kinds below; the cheaper of the two paths wins. The cost divides the scan by PostgreSQL's divisor (the workers plus the share of a leader that scans too, `parallel_leader_participation`), adds `parallel_setup_cost`, and charges every partial table for its groups (merge cost per group). `sift.enable_agg = off` removes both paths.

The node is not under a Gather: its workers return tables, not tuples, so it keeps a parallel context of its own (`sift_agg_worker_main`, one `shm_mq` per worker). The leader opens the source, creates the context, fills the shared state and launches, and waits for the workers with the source still open: the sources keep their page pinned, never locked, between calls, so no interrupt is held off (a held-off interrupt makes `InitializeParallelDSM` plan no workers and the wait never end). A worker opens the relations and the source with the leader's snapshot and parameters and registers the same predicates and keys from the spec. The work is a counter in shared memory that the leader and the workers take items from:

- Exact aggregates (counts, integer sums, min/max, count(DISTINCT)): an item is 2 superblocks. A participant aggregates all its items into one table and sends it when the counter runs out; the leader merges the tables by key value (dictionary codes and key ids are per process), unions the count(DISTINCT) sets and adds the rest. Integer arithmetic is exact, so the order does not matter.
- Float sums (`sum`, `avg` of float8): additions are not associative. The scan has 16 lanes (`SIFT_AGG_LANES`): lane l is the superblocks l, l + 16, l + 32, ..., summed in superblock order into a table of its own, and the leader adds the lane tables in lane order, as soon as the lanes before them arrived. Whether the leader or a worker scans a lane, the additions are the same, so the result is bit-identical for 0, 1, 2, ... workers and every SIMD level. `avg` counts and the "no float seen" flag merge with it; the overflow guard of `avg(float8)` takes the largest input of all lanes.
- Without workers the leader runs the same items, so a serial run is the same computation; a query whose workers cannot be launched (none free, nested in a worker, no DSM) runs on the leader alone.

All the memory that grows with the groups (key ids and values, the group arrays, count(DISTINCT) sets) is in one table context that is reset between lanes; the buffers of a unit and the registered expressions live as long as the scan.

Which specs run in parallel is `sift_aggspec_parallel_ok`, shared by the planner and the executor:

| | Parallel (a table is dumped, merged by value, reset) | Serial only (the leader scans alone: no workers planned, no lanes) |
|---|---|---|
| Keys | a column (dictionary, integer, date, bool), an expression of one column (`SAK_LEAF`, `SAK_LUT`) | float keys, numeric keys (`SAK_NUM`, an `IdMap` of values), keys of a combo (several leaves), the grouping set number (`SAK_SET`) |
| Aggregates | `count(*)`, `count(x)`, `count(DISTINCT row id)` of an unnest, `sum` / `avg` of integers and float8 (leaf or expression), `min` / `max` of a leaf, `count(DISTINCT)` of a dictionary or integer leaf, any `FILTER` | numeric `sum` / `avg` (int128 states), `min` / `max` / `count(DISTINCT)` of a numeric expression, ordered-set aggregates (`percentile_*`: a histogram per group), mapped aggregates (`SAA_M_*`: values of a lookup table), `GROUPING()`, per-order aggregates (`SAA_O_*`) |
| Shape | row filters of every kind, element filters, unnest, `sum(x * y)` programs | lookup joins and combos, two-level (per-order arms), grouping sets, virtual leaves (`jsonb` member by key) |

The serial-only kinds keep their state in structures that are not dumped (histograms, tuple sorts, arms, maps of ids) and have no merge defined: their scan is one item over all the superblocks into one table, with float sums in tuple order. The test `aggstub_scan` checks that they get no workers and give the rows of the normal plan.

`Workers Planned` and `Workers Launched` are in EXPLAIN; `Units`, `Rows Scanned`, `Tuples Aggregated`, `Units Pruned` and the heap counters add the workers'.

## Zone pruning

The row conjuncts of the query that compare an integer leaf of the rows (`int2`, `int4`, `int8`, `date`, `timestamp`, `timestamptz`, `bool`; not a jsonb element, not a lossy leaf) with a constant or parameter by `=`, `<`, `<=`, `>`, `>=` become, per leaf, an interval `[lo, hi]` (`sift_aggspec_zone_cmps`, `prune_init`: the operand is evaluated once per scan, comparisons of one leaf are intersected, `<>`, `IN`, `OR`, element conjuncts and float leaves are not used). The executor passes the intervals to `sift_colsrc_begin` as `SiftSrcPrune`; the `sift_col` provider turns each into two comparisons (`>= lo`, `<= hi`) for `sift_col_begin`, which skips the units whose min/max zone map excludes either, before any leaf is decoded, and counts them (`Units Pruned`). The test provider skips by the minimum and maximum of the unit's values. The executor still applies every conjunct to the rows of the units it reads, so pruning only removes work: a unit is skipped only when no row of it can pass. It applies to every spec, parallel or not.

`agg_cost` prices the scan of the units that are left. The share kept for one comparison is `f + (1 - f) * (1 - corr^2)`, f being the selectivity PostgreSQL estimates for it (`clause_selectivity`: the column's MCVs and histogram, taken when the conjunct is compiled) and corr the correlation of the heap column with the heap order from `pg_statistic` (0 when unknown, and for jsonb paths and index expressions); the most selective comparison counts, and at least one unit is read. A column that grows with the heap (ids, insertion time) has |corr| near 1 and a range query is priced by its range, an unclustered column is priced by the whole table. The leaf read, predicate and unit terms scale with the share; tuples and aggregate updates already follow the selectivity.

### Statistics request

The correlation is a stand-in for what the planner needs, and `SiftSrcStats` of the `sift_col` provider carries only the row and unit counts and the visibility share today (`ndistinct`, `min`, `max` and `elems_per_row` are zero). For each integer leaf of the rows, from the engine (computed at build or ANALYZE from the zone maps it already keeps, nothing read per query):

- `zone_width[leaf]`: the mean over units of `(zone.max - zone.min) / (leaf max - leaf min)`: how wide a unit's zone is relative to the range (0 for a perfectly clustered column, 1 for a random one). With it the share of units a window of fraction f of the range overlaps is `min(1, f + zone_width)`, which replaces `f + (1 - f) * (1 - corr^2)`.
- `zone_hist[leaf]`: 32 equal-width buckets of the range, each with the number of units whose zone overlaps it, to price skewed ranges and `=` on a clustered column exactly.
- `ndistinct`, `min`, `max` of every scalar leaf and `elems_per_row` of every group leaf, for the key-table sizes and the unnest cost.

## Exactness

Integer, date, timestamp, dictionary, boolean and numeric results are exact. `sum(int)` is int8, `avg(int)` is the `numeric` division of the exact sum and count. Float `sum` and `avg` add in the order of the lanes above per group (a scan that is serial only adds in tuple order), so the result does not depend on the SIMD level or the number of workers; it can differ from the normal plan, which adds in heap order, by the rounding of reordered float addition (the comparator's tolerance: relative 1e-9, absolute 1e-12).

PostgreSQL's float8 aggregates raise errors that a plain double sum would hide. `sum(float8)` raises the overflow error when a group's sum is infinite and none of its inputs was. `avg(float8)` keeps a sum of squares (`float8_accum`) that overflows first; the scan does not keep it and raises the same error when the inputs are so large that it could have (`8 * n * max(x)^2 >= DBL_MAX`). The one input where the normal plan does not fail although the bound is met (all values equal and above 1e150) fails here too; set `sift.enable_agg` off for it. Overflow errors inside a float8 expression argument are not reproduced (the result is infinite).

A value the index cannot hold (a jsonb member that does not fit the declared type, text over 255 bytes: the shredder's overflow) makes the scan raise `0A000` with a hint instead of the cast error the normal plan raises on such a value (`22P02`): both fail, the codes differ. The planner refuses leaves the engine marks lossy.

## SIMD

Kernels are chosen at run time through the `sift_cpu` rebind registry and `sift.simd`; the scalar table is the specification and the AVX2 and AVX-512 tables are bit-identical to it, so every result is the same at every level (`docs/KERNELS.md`, "Aggregation kernels"). `sift_aggkern_for(level)` returns a level's table for the differential test. `scripts/coldiff.sh` runs the corpus at `sift.simd = scalar` and `auto`.

### Grouping sets and float keys

- **Sets**: the scan keeps one group table per set over the same key ids; the set is a key of the groups, so a group of the empty set is one group and a data `NULL` is not the `NULL` of a rolled up key. `GROUPING(a, b)` is the bit mask of the sets (the first argument is the highest bit); the empty set returns its one row over no input too. A query of N sets is priced as N group updates per tuple; the filters, leaf reads and unnest are paid once.
- **Float keys**: a float4/float8 column or expression is a key through its bits, with `-0` as `0` and every NaN as one NaN (`float8_eq` and `hashfloat8`); the index stores `-0` as `0` and a value it cannot hold raises `0A000` as above, and the group is emitted as the type's value (`float4` stays `float4`).

## Cost

`agg_cost` prices the scan from the source's statistics: a leaf read, a predicate, a tuple that passes the filters, a key id and an aggregate update, each in `cpu_operator_cost` (about 3 ns here: the constants were fitted on whole table runs of one to three keys and aggregates, 17 ns per unnested element all in). The scan reads every unit that the zone tests leave (above, "Zone pruning"; none on a column the heap order does not follow), so a selective query is priced by its table scan against the btree plan's selected rows: the measured crossover keeps the normal plan for the 10 and 400 store sets and takes the scan for the 4000 store set and the whole table. The grouping sets, the mapped aggregates and the outer WHERE add the update cost of their sets and accumulators only: no constant changed. Heap vetting of the rows on pages that are not all-visible is added per page. Elements per row of an array default to 3 when the source has no statistic.

Two terms keep the selective queries on the normal plan, where the scan reads every unit and the btree plan reads the selected rows. The scan pays a start-up of 140000 operator costs (about 0.4 ms: opening the source, the lookup tables, the first units), which a plan on a few dozen rows does not. A scan of an unnest is charged the square root of the excess of the unnested rows the normal plan expects (100 per call of `jsonb_to_recordset`, `jsonb_array_elements`, `jsonb_each`, less what its filters cut) over the elements the selected orders hold (about 6 for an unfiltered unnest of 3 elements per row, 1 when an element filter cuts the expectation: G2), because its sort, join and aggregate costs grow with that excess though the data has a handful per row. The estimates of the two plans then cross where the times do. A metric of a free key object looked up by key (`metrics->>'overall'`) is charged 9 operator costs per element of the object and key (`AGG_VLEAF_ELEM`): a float key of it at K400 was 0.3 times before. S3 at K400 (0.6 to 0.85 times before) and the summed-`numeric` shape without a path flag at K400 keep the btree plan; S1 and S2 at K400 and every shape at K4000 keep the scan. The fixtures `p_cost_selective_unnest`, `p_cost_selective_numeric` and `p_cost_selective_metric_key` assert it (`expect: plain`).

## Results

The analytics corpus (`bench/sql/analytics`) on 300000 orders (about 960000 items, `sift_col` index), wall ms, median of 7 runs of `scripts/coldiff.sh --bench --runs 7` on a machine shared with other sessions (load 7 to 10 on 8 cores; two runs agree to about 10 percent). Plain is the normal plan, serial; SiftAgg is the same node with no workers (W=0) and with `--bench-workers 4` (W=4: `max_parallel_workers_per_gather = 4` under the default parallel costs, which plan workers for the parallel kinds only):

| Query | Plain | SiftAgg W=0 | SiftAgg W=4 | Speedup W=0 / W=4 | Note |
|---|---:|---:|---:|---:|---|
| G1 count, one item filter | 402 | 3.3 | 11.6 | 121 / 35 | |
| G2 avg/sum/count, three filters | 152 | 5.0 | 11.9 | 31 / 13 | |
| G3 two keys, avg | 494 | 9.7 | 12.3 | 51 / 40 | |
| G4 count(DISTINCT order id) | 279 | 7.0 | 11.0 | 40 / 25 | |
| G5 group by store | 357 | 5.9 | 11.4 | 61 / 24 | |
| G6 month and language | 546 | 12.3 | 12.4 | 45 / 39 | |
| G7 percentile | 554 | 45.4 | 44.4 | 12 / 12 | serial only (ordered sets) |
| G8 group by order (231151 groups) | 281 | 16.3 | 24.8 | 17 / 11 | |
| S1@K4000 | 274 | 12.0 | 12.9 | 23 / 21 | lookup join, multi-leaf key; serial only |
| S1@K400 | 13.8 | plain kept | 7.1 | 1.0 / 2.0 | the two plans are priced within 10 percent of each other |
| S2@K4000 | 675 | 34.6 | 33.4 | 19.5 / 19.9 | per-order aggregates, three arms; serial only |
| S2@K400 | 36.6 | 18.8 | 21.1 | 1.9 / 1.8 | |
| S3@K4000 | 236 | 18.2 | 18.2 | 13 / 12.5 | decode-bound; serial only |
| S4@K4000 | 271 | 46.9 | 59.2 | 5.8 / 4.7 | shared CTE planned inline, four passes |
| S5@K4000 | 87 | 30.4 | 29.4 | 2.9 / 3.0 | count(DISTINCT customer) per group; the window above the node is PostgreSQL's |
| S6@K4000 | 34 | 6.9 | 7.5 | 4.9 / 4.4 | `jsonb_path_exists` flag and `sum` of a `numeric` column: matched |
| S7@K4000 | 48 | 7.1 | 7.4 | 6.6 / 6.6 | `EXISTS` of three conditions on one item |
| S1 to S7 at K10; S3 to S7 at K400 | | | | 0.8 to 1.0 | normal plan kept (btree), no regression beyond noise (the 0.8 of S2 and S4 at K10 is the planning time of the matcher on the plain plan, 0.3 to 0.5 ms) |

Reading the table. The scans that are cheap serially (G1 to G5: 3 to 6 ms) do not gain from workers on this machine: starting four processes costs about 8 ms here, in a session that has just run a long query (PostgreSQL's own Gather over a 300000 row table goes from 3.5 to 10.5 ms after the same plain query in the same session), and the cost model charges `parallel_setup_cost` as the server does, so those plans pick workers and run at that floor; in a fresh session the same node with four workers takes 3.5 to 4.5 ms on G1. The scans of 10 to 20 ms (G3, G6) are at the floor too: G6 takes 11.8 ms with the leader alone and 13 ms with four workers. G7, G8, S1 to S3 keep their state in the leader and take no workers (W=4 equals W=0). What the changes bought in the serial path (measured when they were made, before the numeric and set kinds joined): the key-range and parent passes (`rt_cover`, `build_parents`) took G1, G2, G4, G5 and G8 down by 11 to 25 percent, one allocation of the group arrays for a single key took G8 from 18.4 to 15.4 ms, and the zone tests make a range of a clustered column (`id > 290000`: 111 of 125 units skipped) 0.4 ms instead of the 3 to 4 ms of the btree plan.

What is left to the 20 times of the K4000 shapes: S5@K4000 is 32 ms against 89 ms plain, mostly PostgreSQL's sort, window and JIT above the node (a top 3 per store inside the node would be needed); inside the node the profile is flat. S3@K4000, S4@K4000, S6@K4000 and S7@K4000 are bound by decoding the leaves they read and by serial-only state (above).

The forms of grouping sets, a WHERE on the outer query, mapped aggregates and float keys over the same orders (best of 4 runs of the query, K400 and K4000 stores, `sift.enable_agg` off against on):

| Query | Plain ms | SiftAgg ms | Speedup | Note |
|---|---:|---:|---:|---|
| S3 `ROLLUP (category, period)`, `grouping()` | 23, 218 | 18, 23 | 1.3, 9.3 | S3 itself: 13, 198 against 16, 19 |
| S3 `CUBE (category, period, lang)` | 27, 253 | 13, 19 | 2.1, 13.3 | |
| S1 `ROLLUP (metric, aspect, period)` | 14, 230 | 11, 15 | 1.2, 14.9 | S1 itself: 17, 204 against 9.5, 10.9 |
| S2 with an outer WHERE on the category / on an inner average / both | 41, 757 / 56, 757 / 48, 818 | 17, 45 / 20, 41 / 18, 42 | 2.5, 16.7 / 2.8, 18.3 / 2.6, 19.5 | S2 itself: 39, 687 against 17, 38 |
| `count`, `min`, `max`, `count(DISTINCT)` of a mapped column by language | 14, 167 | 6.7, 8.6 | 2.1, 19.4 | lookup join |
| float8 key (`metrics->>'overall'`) | 1.5, 24 | 1.5, 6.9 | 1.0, 3.5 | K400 keeps the btree plan (the key lookup is priced) |
| float8 key of the items (`price`) | 4.7, 49 | 4.3, 13.9 | 1.1, 3.5 | |

The comparator found no difference in the 145 corpus runs, 880 fixture runs and the 13 DML/maintenance scenarios (insert, update, delete, abort, vacuum, HOT, TID recycling, held writers and snapshots, reindex, promote), on the `sift_col` index and on the test provider, at `sift.simd` scalar and auto, with 0 and 2 workers (the plain kinds run in the workers: `on-par` plans them with free parallel costs). The one error code that differs (above) is tagged `-- error: either` in its fixture.

## Open

- The one-pass S4 (the arms of the `UNION ALL` fused into one scan with several group sets: today four passes).
- A top-k per group inside the node (S5), and decode-bound shapes (S3, S6, S7 at K4000): fewer bytes per element, parallel kinds for the state that is serial-only.
- Partial tables for the serial-only kinds (numeric sums, ordered sets, mapped aggregates, grouping sets, combos and joins, arms): each needs a dump and a merge by value of its own state, and the lookup tables of a join loaded once and shared.
- Float sums in 16 lanes dump and merge a table per lane even in a serial scan; one table with a float accumulator per lane, combined in lane order at the end, would give the same bits for less work.
- The statistics of the "Statistics request" above.
- `count(DISTINCT)` is a hash set per aggregate (about 25 ns per tuple): a per-group bitmap or sort-based form for dense domains.
- A lookup-table expression (a key or a filter that is not a plain comparison) over an integer column whose values span more than 2^22 raises an error with a hint to switch `sift.enable_agg` off; plain group keys, comparisons and `IN` lists over such columns work (hash ids, binary search).
