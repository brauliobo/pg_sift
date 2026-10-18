# Aggregate pushdown on column indexes (SiftAgg)

Plain SQL over a table with a `sift_col` index (or a `sift_table` table) runs on the column data when the planner can prove the result is the one of the normal plan; everything else keeps the normal plan. `sift.enable_agg` (default on) switches the matcher. The result of a pushed query is the one of the plain plan, row for row: `scripts/coldiff.sh` runs every query both ways and compares (docs/AGG.md).

Files: `src/sift_aggplan.c` (shape matcher, cost), `src/sift_aggspec.[ch]` (what the planner proved, serialized into `custom_private`), `src/sift_agg.c` (executor, EXPLAIN), `src/sift_aggkern*.c` (vectorized operators, `docs/KERNELS.md`), `src/sift_colsrc.h` and `src/sift_aggsrc.c` (the seam to the column data and its provider registry), `src/sift_aggcol.c` (provider over the `sift_col` index), `src/sift_table_src.c` (provider over a `sift_table` table, docs/COL-TAM.md section 6), `src/sift_aggstub*.c` (test provider, the `sift_colstub` index AM, `SIFT_TESTING` builds).

## What matches

`create_upper_paths_hook`, stage `UPPERREL_GROUP_AGG`. Query shape: one relation (no inheritance, tablesample or row security) with an index of a provider, at most one lateral unnest of an indexed jsonb array or object (`jsonb_to_recordset`, `jsonb_array_elements`, `jsonb_each`, `jsonb_each_text`), inner joins in the FROM list flattened, no window functions or DISTINCT ON in the aggregating query.

| Part | Supported |
|---|---|
| Columns | an indexed attribute or index expression (`equal()` match), typed jsonb paths (`->`, `->>`, `#>`, `#>>`, casts) as the index declares them, `jsonb_array_length`, the columns of the unnest |
| Quals | `= <> < <= > >=` against constants/params, `IN`/`NOT IN`, `IS [NOT] NULL`, `AND`/`OR`; on `numeric` columns and numeric expressions the same operators, `BETWEEN` and `IN` lists of constants (see Numeric); any other boolean expression of ONE dictionary/integer column becomes a lookup table evaluated by PostgreSQL itself on each distinct value |
| Existential tests | one array of objects, same-element semantics, usable in `WHERE`, in `FILTER` and under `AND`/`OR`/`NOT`: `[NOT] EXISTS (SELECT 1 FROM jsonb_to_recordset / jsonb_array_elements / jsonb_each[_text](col) ... WHERE <element quals>)`; `jsonb_path_exists` / `@?` with a lax `$[*] ? (...)` of `@.key == < <= > >=` against a string (`==`), integer or boolean literal joined by `&&` `\|\|`; `col @> '[{"k": v, ...}, ...]'` and `'[...]' <@ col` with string, integer and boolean members. The element quals are the row quals above over the array's leaves; the test is true when some element passes |
| Group keys | up to 4: a column, or any expression of one dictionary/integer column (`date_trunc`, `lower`, `CASE`, ...) evaluated per distinct value; a `numeric(p <= 18, s)` or `int8` column, or a numeric expression (`round(price, 1)`, `price * qty`, `floor(x)::int8`) ; a float4/float8 key is one group per float value as `float8_eq` has it (below) |
| Grouping sets | `GROUPING SETS`, `ROLLUP`, `CUBE` (any mix, `GROUP BY DISTINCT`) and `GROUPING()`: the sets are what PostgreSQL expanded, all aggregated in the one scan (a row is filtered, decoded and unnested once, then added to the groups of each set), the NULLs of the sets and the `GROUPING()` bits are the planner's own |
| Aggregates | `count(*)`, `count(x)`, `count(DISTINCT x)` (dictionary/integer columns), `count(DISTINCT pk)` at element level (distinct parent rows, proven by a unique NOT NULL column), `sum`/`avg` of int2/int4 and of float8 (also float4 for `avg`; `sum(float4)` is generic), `sum`/`avg` of float8 expressions built of `+ - *` over leaves (`sum(qty * (price - discount))`), `sum`/`avg`/`min`/`max`/`count`/`count(DISTINCT)` and `sum`/`avg(DISTINCT)` of a `numeric(p <= 18, s)` column or of a numeric expression (`sum(price * qty)`, exact, see Numeric), `sum`/`avg`/`percentile_cont` of a `numeric(p <= 15, s)` column cast to float8, `min`/`max` of numbers and dates, `percentile_cont(c)` / `percentile_disc(c) WITHIN GROUP (ORDER BY x)` with a constant fraction in [0, 1] (x: a float8 expression as above, for `percentile_disc` also a float4, int2, int4 or date leaf), each with `FILTER`; every other aggregate with a non-volatile transition function, run by PostgreSQL's own functions (Generic aggregates below); the same over the columns of a lookup join (below) |
| Above | `HAVING`, projection, ORDER BY, LIMIT, and window functions of an outer query stay in the normal plan on top of the custom scan |

Unproven means no path. Not matched today: numeric division, modulo and functions other than the ones in Numeric, comparisons of `numeric` with float8, a `numeric(p, s)` cast and expressions of more than 38 digits (26 for a sum) as the argument of a *native* numeric aggregate (as the argument of a generic one they run in PostgreSQL and match), an `ORDER BY` inside an aggregate call, hypothetical-set and variadic aggregates, volatile expressions and transition functions, a float column read by a generic aggregate whose result could show the sign of zero, columns that are not in the index, semi joins and joins that are not lookups of the kind below, a `UNION` of aggregates in one pass (see Shared CTE), nested aggregation of the unnested rows, and the BM25 `@@>` mask. Each is a documented gap, not a silent difference: the query keeps its plan. The list of the aggregates that stay, with the reason, is in Generic aggregates.

### Lookup joins, several leaves, per-order aggregates

- **Lookup join**: `[LEFT] JOIN small ON small.k = <leaf>` (one to three equalities of dictionary/integer/date leaves and columns of the table, up to four tables), `EXISTS` / `IN (SELECT ..)` (semi join: the row stays once however many rows match) and `NOT EXISTS` (anti join: the row stays when none matches), a key that is an expression of leaves (`small.k = lower(o.source)`), the rest of an `ON` that is no key (`ON small.k = o.x AND small.flag`, `AND o.y > small.min`: a filter of the combo; for a `LEFT` join it decides the match, for an inner join it is a `WHERE`), and a `WHERE` on the joined columns of an outer join (evaluated after the join, so an unmatched row meets NULLs). At least one equality of leaves and columns is the key; a join with none stays. The table is read at executor start with the query's snapshot; its rows are matched per distinct tuple of leaf values, cached across units. Exact: every matching row replicates the input row (a non-unique table needs no proof), a NULL key matches nothing, text compares as `=` does (deterministic collations), the unmatched rows are one row with NULLs (`LEFT`) or gone (`INNER`). The mapped columns are keys, filters and `CASE`/`coalesce` operands like leaves. `count`, `count(DISTINCT)` (hashable types), `min`, `max` (types with a btree order) and `sum`, `avg` (`numeric`, `float8`) of an expression of the joined columns are taken per matching row: the value is `NULL` for an unmatched `LEFT` row (an `INNER` join drops the row), the distinct values of the expression are kept once, and the aggregate compares them (the type's own comparison function) or adds them (`numeric` through its own functions, so the scale of the result is the normal plan's) when the scan ends. `sum` and `avg` of `int2`/`int4`/`int8`, `interval` and `avg(float4)` run PostgreSQL's own transition and final functions of `pg_aggregate` on the distinct values and their counts (`SAA_M_PG`: the state is copied and added `count` times as `nodeAgg` would, strict inputs skipped), so the type and scale of the result are the normal plan's.
- **Several leaves**: a key or filter over up to three leaves (of the order and of its items: `CASE WHEN o.lang = 'en' THEN i.category ELSE ... END`) is evaluated by PostgreSQL per distinct tuple of the values, cached across units. `x->>'k' IS [NOT] NULL` on a leaf of another type is the null test of that leaf (a value it cannot hold raises the overflow error above).
- **Key of a free key object**: `(o.metrics->>'overall')::float8` next to the unnest, when the index has `metrics.* float8`, is a row level leaf built per unit from the elements whose key is `overall` (NULL for the rows without it).
- **Per-order aggregates**: `SELECT .. avg(x) .. FROM (SELECT o.id, keys, agg(..) AS x .. GROUP BY o.id, keys) GROUP BY keys` and the same over a `UNION ALL` of up to four such arms (rollup levels over a shared CTE), where each arm groups by the unique id and by outer keys (or a NULL constant). The inner groups are one row's share of an outer group, so the scan keeps the inner aggregates (`count`, `sum`, `avg`, with `FILTER`) of the current row per outer group and adds them to the outer `count(*)`, `count(DISTINCT id)`, `avg`, `sum` (each with `FILTER` over the inner columns) when the row is done. A WHERE on the outer query is taken when PostgreSQL pushed all of it into the arms: quals on the keys filter the rows of an arm, quals on the inner aggregates are the arm's HAVING (a cell that fails it is no group of the arm and adds nothing to the outer aggregates). A HAVING or WHERE conjunct that is not `inner aggregate <op> constant` (`sum(x) > 2 * count(y)`, an `OR` of aggregates, a function of several) is a boolean expression of the arm's inner aggregates that PostgreSQL's evaluator runs per cell, with the aggregates as parameters. A `pseudoconstant` qual PostgreSQL leaves above the arms (`WHERE (SELECT ...) > 3`, `now() > ..`, a stable function of constants) is a gate: it is evaluated when the scan starts and turns the arm off or on. **Grouping sets** (`ROLLUP`, `CUBE`, `GROUPING SETS`, `GROUPING()`) in the aggregating query run in the same scan: a cell is added to the group of each set, keyed by the set number as for flat sets.


## Ordered-set aggregates

`percentile_cont` and `percentile_disc` keep the values of each group exactly and pick the ranks with the final functions' arithmetic: `ceil(c * n)` for `disc`, `floor` and `ceil` of `c * (n - 1)` and `lo + frac * (hi - lo)` in double for `cont`, `DESC` counting from the end, NULL inputs left out, NaN last, `-0` equal to `0`. Aggregates on the same argument share one set of values. The values are an exact histogram of the distinct (group, value) pairs as long as it fits `work_mem` (4 slots of 16 bytes per pair at most); beyond that every value goes through a `tuplesort` in `work_mem` that spills to disk like the normal plan's sort (EXPLAIN ANALYZE: `Ordered Sets Spilled`), so memory is bounded and nothing fails or falls back. A constant fraction outside [0, 1] and a fraction that is not a constant keep the normal plan (which raises its own error); the array forms and the other argument types are generic aggregates (below). The overflow errors inside a float8 expression argument are not reproduced (as for `sum`).

## Generic aggregates

An aggregate without a native kernel runs on PostgreSQL's own functions (`SAA_GEN`): the scan decodes the argument leaves of the rows that pass the filters, builds the Datums of the argument expression (an expression of up to 4 leaves is evaluated by `ExecEvalExpr` per row, or per element below an unnest, like the lookup tables above) and calls the aggregate's transition function per row and its final function per group, the way `ExecAgg` does: initial value, strict transition functions with a first-value copy, expanded states, `FILTER`, the extra arguments and NULL direct arguments of the final function, the function's `fn_expr` (so `AggCheckCallContext`/`AggGetAggref` work), and the privileges (`EXECUTE` of the aggregate for the user, of the component functions for the aggregate's owner). Nothing is rewritten: the result is the one of the normal plan, row for row, including its errors (a float4 overflow, a bad cast), which are raised by the same function; the order of the groups' rows is the order the scan reads them, which is the heap order the sequential scan of the normal plan gives, so the order sensitive ones (`array_agg`, `string_agg`, `jsonb_agg` without `ORDER BY`) agree as long as the index is in heap order (the planner promises no more than PostgreSQL does for a plain scan: the SQL result of those is unordered).

| Family | Examples | How |
|---|---|---|
| Statistics | `stddev*`, `var*`, `corr`, `covar_*`, `regr_*`, over numeric, float8, int | `float8_accum`/`numeric_accum`/`int8_accum` and their finals |
| Booleans, bits | `bool_and`, `bool_or`, `every`, `bit_and`, `bit_or`, `bit_xor` | the transition functions |
| Sums without a kernel | `sum(float4)` | `float4pl` in heap order (the kernel adds in float8) |
| Min and max of text | `min(text)`, `max(varchar)`, any collation | `text_smaller`/`text_larger` over the decoded text |
| `DISTINCT` | `min`/`max`/`sum`/`avg`/`count(DISTINCT)` of other types, `string_agg(DISTINCT t, ',')`, `array_agg(DISTINCT x)`, `jsonb_agg(DISTINCT x)` | the distinct values of the one varying argument are kept per group and fed in the type's btree order (NULL last, the first of equal values kept) |
| Collecting | `string_agg`, `array_agg`, `json_agg`, `jsonb_agg`, `json_object_agg` without `ORDER BY` | the transition function in scan order |
| Ordered sets | `percentile_cont`/`percentile_disc` with an array of fractions, over an interval, a date, text or any other type with a btree order, `mode()`, `DESC`, with `FILTER` | the values of a group go through one `tuplesort` of `(group, value)` (bounded by `work_mem`, spills like the normal plan's sort), then the group's transition function over the sorted values and the final function with the direct arguments |
| User aggregates | any `CREATE AGGREGATE` whose transition and final functions are not volatile | as above |

Rules that keep the plan, all checked at plan time:

- `ORDER BY` inside the aggregate call (`string_agg(x, ',' ORDER BY y)`), hypothetical-set aggregates (`rank(c) WITHIN GROUP`, `dense_rank`, `percent_rank`, `cume_dist`), variadic aggregates and aggregates of more than 4 arguments: the input order or the argument shape is not the scan's.
- A volatile transition or final function, or a volatile argument expression: calling it per row and per group is not a function of the data.
- A column that is not a scalar leaf of the index (`jsonb` members of a type the index does not hold, arrays, `numeric(p > 18)`, `uuid`, `time`, `interval`, `bytea`, `char(n)`), an expression of more than 4 leaves, a column of another table than the index's (a joined or mapped column) or a lookup, and any sub-query or parameter that is not a constant.
- A float leaf whose result shows the sign of a zero (`-0` is stored as `0`): a generic aggregate over `float4`/`float8` is taken only when its result is a number or a boolean (`stddev`, `sum`, `min`, `bool_and`, ...), not text, an array, jsonb or an interval of the value (`array_agg(f8)`, `string_agg(f8::text)`).
- `DISTINCT` over more than one varying argument, or over a type without a btree comparison.
- A fraction that is not a constant, outside [0, 1] or a hypothetical value: the normal plan raises its own error.
- Parallel scan: generic aggregates are serial only (their states are PostgreSQL datums with no dump format). Merging per-worker states with the aggregate's combine function is possible for the ones that have one (`stddev`, `sum`, `bool_*`, `array_agg`) and is not done: a query with a generic aggregate plans no workers. `DISTINCT` and ordered sets keep their values until the scan ends (the DISTINCT sets are not bounded by `work_mem`: one entry of the value's size per distinct pair; the ordered ones are).

Cost: a generic aggregate is priced per row by `AGG_GEN_ROW` (12 operator costs for the call through `fmgr` and the Datum of a leaf) plus the transition function's own cost (`procost`: 1 for the built-in ones, 100 for a SQL function), the argument's expression cost and 2 per leaf, `AGG_GEN_DISTINCT` (10: the hash probe of the pair) and `AGG_GEN_ORDERED` (24: the tuplesort put), and per group by `AGG_GEN_GROUP` (150: state, final function and the sort of an ordered group). They are the measured prices (a boolean or bit aggregate takes about 40 ns a row, a numeric accumulation 100 ns, a SQL function 1 us). The calls count the elements the data has, outside the factor that the unnest of the normal plan puts on the rest of the scan (see Cost): the whole table takes the scan, a selective store set keeps the btree plan (the S shapes at K10 and K400 of the analytics corpus did not change, and the fixture `g_cost_selective` asserts it).

On 300000 orders (960000 items, one core, `sift_col` index, median of 7, wall ms; "default" is the plan PostgreSQL's planner picks with the default costs and workers), plain plan against SiftAgg without workers:

| Family and query | Plain W=0 | Plain default | SiftAgg | Speedup | Default plan |
|---|---:|---:|---:|---:|---|
| `stddev_samp(numeric)`, `var_pop(int)` per 50 groups | 89 | 87 | 19 | 4.7 | SiftAgg |
| `stddev_samp`, `corr` (float8) of the items per category | 531 | 258 | 40 | 13.2 | SiftAgg |
| `bool_and`, `bit_or` per status | 91 | 42 | 18 | 5.1 | SiftAgg |
| `min`, `max` of text per country | 102 | 49 | 16 | 6.2 | SiftAgg |
| `sum(total::float4)` per status | 104 | 53 | 32 | 3.2 | SiftAgg |
| `sum(DISTINCT)`, `string_agg(DISTINCT)` per country | 301 | 267 | 23 | 13.1 | SiftAgg |
| `array_agg(DISTINCT date)` per status | 200 | 203 | 8.6 | 23.3 | SiftAgg |
| `percentile_cont(array)` per 50 groups | 143 | 143 | 90 | 1.6 | SiftAgg |
| `mode()` of text per country | 141 | 143 | 59 | 2.4 | SiftAgg |
| `percentile_disc` of a date per status | 114 | 129 | 9.5 | 12.0 | SiftAgg |
| `array_agg(int)` per status | 95 | 54 | 20 | 4.7 | SiftAgg |
| user aggregate (SQL transition function) per status | 291 | 292 | 180 | 1.6 | SiftAgg |

The scan is the cost of decoding the leaves and building a Datum per value; what the aggregate itself costs (the numeric accumulation, a SQL function, the tuplesort of an ordered set) is PostgreSQL's and is the same in both plans, which is why the user aggregate and the sorting ones gain least.

## Shared CTE

PostgreSQL computes a CTE that the query names more than once once and hands the rows to every reference, so aggregates over it (S4: a time series at four granularities, each arm a `GROUP BY` of the same CTE) never reach the index. When a CTE with no recursion over a table that has a column index is read twice or more, `sift_aggcte.c` (a `planner_hook`) plans the query again with the CTE `NOT MATERIALIZED` (PostgreSQL itself refuses that for a CTE with a volatile function or data-modifying statement) and keeps that plan only if it has a `Custom Scan (SiftAgg)` and costs 2 times less than the materialized one, on top of the factor of about 6 that `agg_cost` charges every scan of an unnest (see Cost): the planner assumes 100 rows per `jsonb_to_recordset` call against the handful the data has. S4 keeps the materialized plan for the 400 store set and takes the inline one for the 4000 store set, where it is 5 times cheaper. Both plans are plans of the same query. Each arm is its own pass over the index (four for S4); one pass computing the four granularities needs a scan with several group sets sharing the filters, decoded leaves and unnest, and a planner step that fuses the arms of the `UNION ALL` (they are planned apart, each with its own PlannerInfo), which is not done.

## Numeric

A `numeric(p, s)` column with `p <= 18` is an INT leaf holding the number times 10^s (docs/COL.md), and the plan treats it as an int64 leaf with a scale: the same comparison kernels, group ids and accumulators as every integer leaf, PostgreSQL's own numeric functions for everything that is not integer arithmetic. An `int8` column is the same leaf with scale 0 (so it is also a group key now). Unconstrained, wider or negative-scale numeric is refused at `CREATE INDEX`, so those columns are never on the index.

**Predicates** (`numeric` column against a constant or a parameter): `= <> < <= > >=`, `BETWEEN`, `IS [NOT] NULL`, `IN`/`NOT IN` of up to 64 constants (an OR of equalities, an AND of inequalities, as PostgreSQL evaluates them: `NOT IN` with a NULL member is never true). The operand is evaluated when the scan starts and scaled with `numeric_mul`, `numeric_floor` and `numeric_ceil`; the operator is rewritten for the stored integers: `x = c` holds for `c * 10^s` when it is an integer and for no value otherwise (`x <> c` for every one), `x < c` is `x < ceil(c * 10^s)`, `x <= c` is `x <= floor(c * 10^s)`, `x > c` is `x > floor(c * 10^s)`, `x >= c` is `x >= ceil(c * 10^s)`. A constant beyond the integer range, NaN (greater than every number in PostgreSQL's numeric order) and the infinities give the same answer for every value of the column; none of them reaches the kernels.

**Expressions** (aggregate arguments, group keys, predicates, `count(DISTINCT)`): `+ - *`, unary minus, `abs`, `round`/`trunc` with a constant number of digits (also negative), `floor`, `ceil`, numeric constants, `int2`/`int4`/`int8` columns cast to numeric, and numeric cast to `int2`/`int4`/`int8`. They run as a program of int128 values scaled by the result's `dscale`, which is what PostgreSQL gives: the larger scale of the operands for `+` and `-`, their sum for `*`, the digits asked for by `round` and `trunc` (0 for a negative count), 0 for `floor` and `ceil`. Two expressions in a comparison are compared as their difference against 0. The planner tracks the digits each step can have from the columns' declared precision and refuses the expression when a step could need more than 38 (int128), a sum or average when its argument could need more than 26 (room for 10^12 rows), a cast to an integer when the digits it keeps could overflow it (PostgreSQL raises the error then). Equality is by value: a distinct count, a group key and a `min` over one expression compare scaled integers of one scale, so `1.0` and `1.00` are the same number. The text of a result is the one of PostgreSQL: a sum, key or extreme is written with the scale above, so trailing zeros are kept.

**Aggregates**: `sum`/`avg`/`min`/`max`/`count`/`count(DISTINCT)` of a column or an expression (`sum(DISTINCT)` and `avg(DISTINCT)` too). `sum` adds in 128 bits, so it is exact however many values there are, and is written as the numeric of the argument's scale (the `dscale` PostgreSQL gives the sum); `avg` is `numeric_div` of that sum and the count (as `numeric_avg` does); `min`/`max` are the value; a group without values is NULL. `sum`, `avg` and `percentile_cont` of `numeric(p <= 15, s)` cast to float8 divide the integer by 10^s in double: below 2^53 that is the correctly rounded value `numeric_float8` computes.

**Not matched, and the rule that blocks it**: division (`numeric_div` picks its scale by `select_div_scale` from the values' leading digits and rounds to at least 16 significant digits, so the result scale is not a function of the operands' scales), `%`, `sqrt`, `power` and every other function (rounded results or errors that are not known here), `numeric` against float8 (a float8 comparison of `numeric_float8` of the column), the `numeric(p, s)` cast (rounds and raises `numeric field overflow` per value), a NaN or infinite constant inside the arithmetic, `IN` with a parameter array or more than 64 constants. The native numeric kernels refuse each of them; the aggregate then runs as a generic aggregate (below), where PostgreSQL evaluates the expression and adds with its own functions. A `NaN` in the column is stored as NULL with the overflow flag, so the scan raises `0A000` with a hint like any value the index cannot hold, where the normal plan returns the NaN: set `sift.enable_agg` off for such a table.

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

1. row predicates to a bitmap with the kernel tables (integer comparisons are `range` and dictionary lookups `lut` of the column table, `cmp_f64` / `between_f64` / `lut_i32` of the aggregation table; bitmap operations are the column table's `bits_*`); dictionary predicates are evaluated once per distinct value of the unit's dictionary;
2. with an unnest, element predicates in vectors of 4096 elements, the row bitmap lifted to the elements through their parent ordinals;
3. `sift_colsrc_live` and `sift_colsrc_vet`: dead rows out, then, after the value filters, the rows that the snapshot does not see (all-visible pages pass, the others are read in the heap; a `sift_table` table has nothing to check);
4. group ids: dictionary codes and integer values map to dense key ids through tables built once per dictionary / value range; ids combine into a direct array (up to 2^20 groups) or a hash table beyond;
5. aggregates accumulate by group in tuple order.

The groups leave as the rows of `custom_scan_tlist` (keys, then aggregates); the plan above projects and filters. `Rows Vetted By Heap`, `Heap Pages`, `Units`, `Units Pruned`, `Rows Scanned`, `Tuples Aggregated` and `Groups` are in EXPLAIN ANALYZE, the filters, group keys and pushed aggregates in plain EXPLAIN.

### One pass for the arms

The inlined arms of S4 scan the same rows for the same aggregates and differ in their keys. The nodes of one executor state register when they begin (`fuse_register`); the first to run takes every registered node whose spec equals its own with the keys left out (`sift_aggspec_fusable`: same leaves, filters, aggregates, no sets, combos, joins or arms; at most four keys together) and scans once with their keys side by side and the arm as the set key, the machinery of the grouping sets above (`sift_aggspec_fuse`). Each node then returns the groups of its set. The tuples of a set are those of the node alone, in the same order, so the rows and the float sums are the ones of separate passes; `EXPLAIN ANALYZE` shows `Arms In One Pass` and the counters of the shared scan on each node.

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
| Keys | a column (dictionary, integer, date, bool), an expression of one column (`SAK_LEAF`, `SAK_LUT`), float keys (bits), numeric keys (`SAK_NUM`: the value, 128 bits), keys of a combo (`SAK_COMBO`), the grouping set number (`SAK_SET`) | |
| Aggregates | `count(*)`, `count(x)`, `count(DISTINCT row id)` of an unnest, `sum` / `avg` of integers, float8 and numeric (leaf or expression), `min` / `max` of a leaf or a numeric expression, `count(DISTINCT)` of a dictionary or integer leaf, ordered-set aggregates (`percentile_*`: the histogram of values with their counts, or the sorted pairs of a spilled one), any `FILTER` | `count(DISTINCT)` and `sum` / `avg` with `DISTINCT` of a numeric expression (a set of 128 bit values per group), mapped aggregates (`SAA_M_*`: values of a lookup table), per-order aggregates (`SAA_O_*`) |
| Shape | row filters of every kind, element filters, unnest, `sum(x * y)` programs, lookup joins and combos, grouping sets, virtual leaves (`jsonb` member by key) | two-level (per-order arms) |

A worker loads the lookup tables of the joins itself, with the leader's snapshot (a few rows each), and evaluates the slots of its combos; the output rows of a combo hold ids of the key tables, so a lane forgets them with the table. The partial table of a numeric sum carries the 128 bit state, the grouping sets are groups of the set number (an integer key), an ordered set sends its histogram as (group, value, count) runs and the leader adds the counts into one histogram (or a tuple sort once it does not fit `work_mem`), and `percentile_*` then walks the sorted values once as in a serial scan.

Generic aggregates (`SAA_GEN`, PostgreSQL's own transition and final functions) run on the leader alone: their states are not dumped or merged, so a query with one gets no workers and is never fused with other arms.

The kinds that stay serial keep their state in structures that are not dumped and have no merge defined: their scan is one item over all the superblocks into one table, with float sums in tuple order. The per-order arms hold a cell per (arm, group) while a row is read and flush it into the outer accumulators at the end of the row, which would merge like any other table, but the guard of an inner `avg(float8)` takes the tuple count and the largest input of the whole scan and S2 is already 22 times faster than the normal plan on one process (32 ms, of which a worker start is 8). A mapped aggregate keeps the values of a lookup table that its sets refer to by id. The test `aggstub_scan` checks that every parallel kind gets workers and gives the rows of the one-process scan and of the normal plan.

A scan with float sums and no workers launched adds the lanes in turn into one table: a lane's float sums are collected in the accumulator and added to the sums of the lanes before it when the lane is done, which are the additions the leader makes when it merges the tables of the lanes (`lane_fold`), so the bits are those of a scan with workers, without dumping and merging a table per lane.

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

A generic aggregate is PostgreSQL's own arithmetic over the values in scan order, so it is exact where the function is (numeric, integers, text, dates) and, for float accumulation (`stddev`, `corr`, `sum(float4)`), differs from a normal plan that sorts before aggregating by the rounding of the reordered additions (the comparator's tolerance); the normal plan's own order is not fixed either. Its errors are the function's, raised at the same value.

Float sums are not the normal plan's, and they are not the same between a serial scan and one with workers: a serial scan adds in tuple order into the 16 lanes, workers each fill their own lanes over the units they took and the leader adds the lanes in order, so the last digits can differ between the two and from the normal plan. It is the class of difference PostgreSQL's own parallel float sums have (a parallel plan adds partial sums in the order the workers finish); every other aggregate is exact. Within one mode the result does not change with the SIMD level.

The order of the groups is not stable between a serial and a parallel run (the workers merge their tables in launch order and a hash table lists its groups by slot): a query that needs an order has an `ORDER BY`, as with the normal plan, whose group order is the hash table's too.

PostgreSQL's float8 aggregates raise errors that a plain double sum would hide. `sum(float8)` raises the overflow error when a group's sum is infinite and none of its inputs was. `avg(float8)` keeps a sum of squares (`float8_accum`) that overflows first; the scan does not keep it and raises the same error when the inputs are so large that it could have (`8 * n * max(x)^2 >= DBL_MAX`). The one input where the normal plan does not fail although the bound is met (all values equal and above 1e150) fails here too; set `sift.enable_agg` off for it. Overflow errors inside a float8 expression argument are not reproduced (the result is infinite).

A value the index cannot hold (a jsonb member that does not fit the declared type, text over its declared `text(N)`: the shredder's overflow) makes the scan raise `0A000` with a hint instead of the cast error the normal plan raises on such a value (`22P02`): both fail, the codes differ. The planner refuses leaves the engine marks lossy.

## SIMD

Kernels are chosen at run time through the `sift_cpu` rebind registry and `sift.simd`; the scalar table is the specification and the AVX2 and AVX-512 tables are bit-identical to it, so every result is the same at every level (`docs/KERNELS.md`, "Aggregation kernels"). `sift_aggkern_for(level)` returns a level's table for the differential test. `scripts/coldiff.sh` runs the corpus at `sift.simd = scalar` and `auto`.

### Grouping sets and float keys

- **Sets**: the scan keeps one group table per set over the same key ids; the set is a key of the groups, so a group of the empty set is one group and a data `NULL` is not the `NULL` of a rolled up key. `GROUPING(a, b)` is the bit mask of the sets (the first argument is the highest bit); the empty set returns its one row over no input too. A query of N sets is priced as N group updates per tuple; the filters, leaf reads and unnest are paid once.
- **Float keys**: a float4/float8 column or expression is a key through its bits, with `-0` as `0` and every NaN as one NaN (`float8_eq` and `hashfloat8`); the index stores `-0` with its sign (the scan treats it as `0`) and a value it cannot hold raises `0A000` as above, and the group is emitted as the type's value (`float4` stays `float4`).

## Cost

`agg_cost` prices the scan from the source's statistics: a leaf read, a predicate, a tuple that passes the filters, a key id and an aggregate update, each in `cpu_operator_cost` (about 3 ns here: the constants were fitted on whole table runs of one to three keys and aggregates, 17 ns per unnested element all in). The scan reads every unit that the zone tests leave (above, "Zone pruning"; none on a column the heap order does not follow), so a selective query is priced by its table scan against the btree plan's selected rows: the measured crossover keeps the normal plan for the 10 and 400 store sets and takes the scan for the 4000 store set and the whole table. The grouping sets, the mapped aggregates and the outer WHERE add the update cost of their sets and accumulators only: no constant changed. Heap vetting of the rows on pages that are not all-visible is added per page. Elements per row of an array default to 3 when the source has no statistic.

Two terms keep the selective queries on the normal plan, where the scan reads every unit and the btree plan reads the selected rows. The scan pays a start-up of 140000 operator costs (about 0.4 ms: opening the source, the lookup tables, the first units), which a plan on a few dozen rows does not. A scan of an unnest is charged the square root of the excess of the unnested rows the normal plan expects (100 per call of `jsonb_to_recordset`, `jsonb_array_elements`, `jsonb_each`, less what its filters cut) over the elements the selected orders hold (about 6 for an unfiltered unnest of 3 elements per row, 1 when an element filter cuts the expectation: G2), because its sort, join and aggregate costs grow with that excess though the data has a handful per row. The estimates of the two plans then cross where the times do. A metric of a free key object looked up by key (`metrics->>'overall'`) is charged 9 operator costs per element of the object and key (`AGG_VLEAF_ELEM`): a float key of it at K400 was 0.3 times before. S3 at K400 (0.6 to 0.85 times before) and the summed-`numeric` shape without a path flag at K400 keep the btree plan; S1 and S2 at K400 and every shape at K4000 keep the scan. The fixtures `p_cost_selective_unnest`, `p_cost_selective_numeric` and `p_cost_selective_metric_key` assert it (`expect: plain`).

A path with workers pays `AGG_SETUP_FACTOR` (110) times `parallel_setup_cost`: starting four workers and opening the source in each takes about 8 ms, and in the estimates of this path (the unnest excess included) a cost unit is worth about 0.1 us, so the default 1000 would charge 0.1 ms. At 110 the scans of under 15 ms (G1 to G6, S1, S2, S4, S6 and S7 at K4000) keep the leader alone, which is what the measurements ask for (W=4 not above W=0 beyond noise), and S3 (241000 units) and the percentile scan (G7, 185000 units) take the workers and gain. The ordered-set aggregates add the merge of their histograms, 1.5 operators per value (`AGG_ORDERED_MERGE`).

The rows the node returns are bounded by the groups its tuples can make (`rows_out` in `agg_cost`: the sets times the unnested rows it expects to aggregate). PostgreSQL's group estimate of an unnest is its default 200 distinct values times the 100 rows it assumes per call, 768500 groups for S5 (20334 exist), and the sort and window above the node were priced for them: the node's plan was estimated above the plain parallel plan (261889 against 252983) though it runs five times faster, and the plain plan is cheaper by its pathkeys. With the bound the plan above the node is priced for the groups that can exist (S5: 117883 against 350917 plain at every worker count).

A plan with a SiftAgg node is not JIT compiled (`sift_planner`, `src/sift_aggcte.c`): its cost, which stands for 15 to 40 ms of scanning, is above `jit_above_cost`, and compiling the projections of the nodes above (a window, a union) took 3.5 to 5 ms of those queries (S3 -17 percent, S5 -14, S4 -13).

## Results

The analytics corpus (`bench/sql/analytics`) on 300000 orders (about 960000 items, `sift_col` index), `scripts/coldiff.sh --suite analytics --rows 300000 --bench --runs 7 --bench-workers N`: wall ms, median of 7 runs of each query in one session (`\timing`), on a machine shared with other sessions (load 14 to 16 on 8 cores during these runs: two runs agree to about 10 to 20 percent, W=2 ran in the busiest minutes). Plain is the normal plan, serial (the reference of every W, measured in the same run); SiftAgg is the node at `max_parallel_workers_per_gather` 0, 2 and 4 with the real parallel costs; "before" is the library of the start of this work (W=0). The speedup is plain over SiftAgg of the same run. `sift.enable_agg` off at W=4 is PostgreSQL's own parallel plan, which is no faster than the serial one for these queries (S1 224, S3 85, G1 95, G3 169 ms).

| Query | Plain (W=0 run) | SiftAgg W=0 before | W=0 | W=2 | W=4 | Speedup W=0 / W=2 / W=4 | Note |
|---|---:|---:|---:|---:|---:|---:|---|
| G1 count, one item filter | 432 | 3.8 | 3.6 | 5.3 | 6.3 | 121 / 102 / 112 | |
| G2 avg/sum/count, three filters | 165 | 5.5 | 5.0 | 5.8 | 8.3 | 33 / 32 / 29 | |
| G3 two keys, avg | 549 | 10.6 | 10.2 | 10.3 | 12.5 | 54 / 55 / 62 | |
| G4 count(DISTINCT order id) | 267 | 6.2 | 6.3 | 6.8 | 8.7 | 42 / 43 / 41 | |
| G5 group by store | 284 | 5.7 | 5.7 | 5.5 | 8.8 | 49 / 58 / 55 | |
| G6 month and language | 514 | 13.9 | 9.1 | 10.2 | 15.0 | 56 / 53 / 46 | |
| G7 percentile | 589 | 47.0 | 41.9 | 46.8 | 41.8 | 14 / 14 / 22 | ordered sets in the workers (W=4) |
| G8 group by order (231151 groups) | 292 | 17.7 | 17.9 | 20.9 | 19.2 | 16 / 15 / 17 | |
| S1@K4000 | 295 | 13.0 | 9.3 | 9.9 | 12.9 | 32 / 31 / 30 | lookup join, multi-leaf key |
| S1@K400 | 15.3 | plain kept | 7.2 | 9.1 | plain kept | 2.1 / 1.8 / 1.0 | scan at W=0 and W=2 (the cost crosses within noise), W=4 charges the start of its workers and keeps the plan |
| S2@K4000 | 700 | 33.0 | 28.7 | 44.3 | 29.2 | 24 / 21 / 30 | per-order aggregates, three arms; serial only (W=2: the busiest run, plain 934) |
| S2@K400 | 38.6 | 20.4 | 12.2 | 13.8 | 20.9 | 3.2 / 3.0 / 3.7 | |
| S3@K4000 | 242 | 18.8 | 14.1 | 19.4 | 18.0 | 17 / 19 / 14 | lookup join, free-key member, unnest |
| S4@K4000 | 275 | 47.4 | 16.6 | 22.6 | 17.5 | 17 / 17 / 18 | shared CTE planned inline, the four arms in one pass |
| S5@K4000 | 93.4 | 35.3 | 19.0 | 21.9 | 22.5 | 4.9 / 5.1 / 4.7 | count(DISTINCT customer) per group; the plan above the node is priced for its groups (Cost): at W=2 and W=4 it was 0.9 |
| S6@K4000 | 35.9 | 6.8 | 6.3 | 7.7 | 6.1 | 5.7 / 5.9 / 6.3 | `jsonb_path_exists` flag and `sum` of a `numeric` column: matched |
| S7@K4000 | 49.6 | 8.6 | 7.2 | 7.7 | 8.0 | 6.9 / 7.4 / 6.5 | `EXISTS` of three conditions on one item |
| S1 to S7 at K10; S3 to S7 at K400 | | | | | | 0.9 to 1.0 | normal plan kept (btree); no regression beyond noise: the plan is PostgreSQL's, and where the table shows 0.5 to 0.8 (S5@K400 at W=4: 16.0 against 8.2 in the sweep, 9.9 to 12.0 in three repeats against 9.7 to 10.8 of the same plan without the node) it is PostgreSQL's own parallel plan against the serial reference |

No shape is slower than the serial plain plan at any W. The W=0 column of S1, S3 and S4 is lower than "before" also because of the changes below.

What each change bought (A/B of two builds on the same cluster, cycles or median wall ms at K4000, W=0 unless said):

| Change | Gain |
|---|---|
| No JIT for a plan with a SiftAgg node | S3 -17%, S5 -14%, S4 -13% (3.5 to 5 ms of compiled projections) |
| Existential element predicates (`jsonb_path_exists`, `EXISTS`) as a branch-free pass over the rows: the 56 bits of the element bitmap from the first element of a row | S6 -11% cycles (the per-row scan with its mispredicted branches was 23 percent of the query), S7 -3% |
| Free-key member (`metrics->>'overall'`) as a range pass over the keys plus the last element of the row, the copy sized at compile time | S3 -12% cycles |
| The 16 lanes of a float sum scanned in turn into one table (no dump and merge of a table per lane) | S5 50 -> 37 ms, G6 20 -> 15 ms, S4 -14% |
| Ordered-set inserts prefetched in blocks of 64 | G7 W=0 44.3 -> 40.3 ms |
| Parallel kinds: ordered sets, numeric sums and keys, float keys, grouping sets, lookup joins and combos, free-key members | G7 W=4 42 -> 28 ms; S3 W=4 15.9 -> 10.5; S1 forced to workers 12 -> 9.5 (the cost keeps S1 on the leader: its scan is under the 8 ms of a worker start) |
| Worker start priced at 110 times `parallel_setup_cost` | G3, G6, G8, S4, S6 and S7 stay on the leader (G3 13.3 and G6 15.2 ms with workers against 10.2 and 13.4 without, before) |

Reading the table. The scans that are cheap serially (G1 to G6, S1, S4, S6, S7: 3 to 36 ms) do not gain from workers on this machine: starting four processes costs about 8 ms here, in a session that has just run a long query (PostgreSQL's own Gather over a 300000 row table goes from 3.5 to 10.5 ms after the same plain query in the same session), and the cost model charges that start (above), so those plans keep the leader and W=4 equals W=0. S3 (14 ms on the leader) and G7 (41 ms) take the workers: at 10.5 and 28 ms on a quiet machine, but in the sweep above the load was twice the cores and the workers wait for a core (S3 18 to 19 ms, G7 42 to 47), so the table shows them level with the leader or behind it, never behind the plain plan. S2 keeps its per-order arms on the leader; S4 runs its arms in one pass.

S5@K4000 at W=2 and W=4 lost to PostgreSQL's parallel plan by estimate before the bound on the returned rows (Cost): the window above the node needs its groups sorted by the group keys, and the sort of the 768500 groups that PostgreSQL expects (20334 exist: the statistics of an unnest of `jsonb_to_recordset` are its default 200 distinct values) was charged to the node's plan only. The plan is now priced at 117883 against 350917 for the plain one at every W.

What is left to the 20 times of the K4000 shapes, and why. S5 (4.7): 19.6 ms against 91 ms plain, of which PostgreSQL's planning (1.8 ms) and the sort and window above the node are a third, and the node's own distinct sets (`dset_add`, `hash_group`, 25 ns a tuple) most of the rest; a top 3 per store inside the node would be needed. S3 (14 to 19), S4 (17), S6 (6) and S7 (7) are bound by what the column engine decodes: `read_positions`, `col_read`, `nd_i64`, `nd_u32`, `unit_page` and `expand_parents` are 25 to 40 percent of the cycles of each, the rest is flat (profile after the changes above: the executor's own first function is `any_elem_rows` with 12 percent of S6, the element predicate included; in S3, S4 and S7 none is above 5 percent); S6 and S7 are 5 to 7 ms of which 1.2 to 1.9 ms is PostgreSQL's planning, so 20 times would be under 2 ms. S4 now decodes once for its four arms (47 to 36 to 16.6 ms): what is left is the decode, the filters and four group updates per tuple, in a hash table of the product of the keys (the fused pass reads each tuple once but updates one group per set). G7 and G8 are at 20 and 17: G8 is 231151 groups.

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

Joins, gates, grouping sets and the general arm HAVING (fixtures on 300000 orders of `test/col/fixtures`, about 960000 items, median of 7, `--bench --bench-workers 0` and `4`; the times include the temporary table of the result and the machine was shared with other sessions, so two runs agree to about 15 percent). Joins, arms and sets are serial-only kinds (no workers are planned, so W=4 is W=0 up to noise); the flat gate is a parallel kind and pays the launch of workers like G1 to G5:

| Fixture | Query | Plain ms | W=0 ms | W=4 ms | Speedup W=0 / W=4 |
|---|---|---:|---:|---:|---|
| `j_semi_exists` | `EXISTS` on a lookup table, count by category | 503 | 10.0 | 12.8 | 50 / 41 |
| `j_semi_in_subquery` | `IN (SELECT ..)` | 252 | 5.1 | 5.9 | 49 / 49 |
| `j_anti_not_exists` | `NOT EXISTS` | 536 | 12.3 | 12.9 | 44 / 54 |
| `j_key_expr` | key `small.k = lower(leaf)` | 783 | 11.0 | 11.7 | 71 / 79 |
| `j_on_filter` | filter in the `ON` of a `LEFT` join | 480 | 14.7 | 14.6 | 33 / 35 |
| `j_where_left` | `WHERE` on the joined column of a `LEFT` join | 696 | 16.0 | 16.9 | 44 / 48 |
| `j_inner_filter` | filter over leaves of both sides, evaluated per distinct tuple | 98 | 31.0 | 41.0 | 3.2 / 2.7 |
| `t_mapped_agg_pg` | `sum`/`avg` of int2/int4/int8/interval/float4 of a joined column | 524 | 47.5 | 44.5 | 11 / 15 |
| `t_mapped_agg_pg_inner` | the same under `ROLLUP` | 540 | 48.2 | 58.0 | 11 / 10.5 |
| `f_gate_stable` | a qual with no column of the table (`now() > ..`, a scalar sub-select) | 110 | 2.7 | 4.0 | 41 / 33 |
| `s2_per_order_union` | per-order aggregates, three arms (the form without the changes) | 1539 | 82.4 | 132.9 | 19 / 17 |
| `s2_per_order_gate` | the same with a gate in the arms | 1447 | 74.0 | 91.2 | 20 / 18 |
| `s2_per_order_union_sets` | arms with `GROUPING SETS` in the aggregating query | 1457 | 117.1 | 137.4 | 12 / 16 |
| `t_two_level_where_exprs` | `OR` and expressions of several inner aggregates as the arm's HAVING | 800 | 149.4 | 188.8 | 5.4 / 5.4 |
| `s2_per_order_rollup` | `ROLLUP` over per-order aggregates, 3 levels, one arm | 1018 | 569 | 401 | 1.8 / 2.2 |
| `s2_per_order_sets_hash` | sets whose outer groups overflow the direct array (hash groups) | 4319 | 3819 | 3987 | 1.1 / 1.0 |

The last two are bound by the cells: a hash of (row, group) per set and per cell, in one process, so the scan is as fast as PostgreSQL's sort and aggregate; they are chosen by cost and are not slower. The analytics corpus at 300000 orders after these changes (same method, `--bench --runs 7`, W=0): G1 to G8 30 to 137 times, S1@K4000 22, S2@K4000 22, S3@K4000 13, S4@K4000 5.7, S5@K4000 2.5, S6@K4000 5.1, S7@K4000 6.4; S1@K400 2.2 and S2@K400 1.9 on the scan; S1, S2 at K10 and S3 to S7 at K10 and K400 on the normal plan, 0.7 to 1.2 times (the 0.7 of S2 and S4 at K10 is the planning time of the matcher). All 29 queries equal.

The comparator found no difference in the 145 corpus runs, 880 fixture runs and the 13 DML/maintenance scenarios (insert, update, delete, abort, vacuum, HOT, TID recycling, held writers and snapshots, reindex, promote), on the `sift_col` index and on the test provider, at `sift.simd` scalar and auto, with 0 and 2 workers (the plain kinds run in the workers: `on-par` plans them with free parallel costs). The one error code that differs (above) is tagged `-- error: either` in its fixture.

## Stays on the normal plan

What the planner refuses among the joins and the per-order form, each once with the reason. The traps with a fixture (`t_not_in_subquery`, `t_key_expr_volatile`, `t_two_level_where_*`) must keep the plan and equal `sift.enable_agg = off`; the others are listed from the planner's refusals.

| Shape | Reason |
|---|---|
| `FULL JOIN` to a table | the unmatched rows of the table would be output rows with no source row; the scan only emits tuples of source rows |
| `RIGHT JOIN` | PostgreSQL turns it into a `LEFT` join of the other side, so the table is the preserved side: the same reason as `FULL` |
| `NOT IN (SELECT ..)` | a hashed `SubPlan` with the NULL rule of `NOT IN` (one NULL member turns every answer into NULL); it is no anti join. `NOT EXISTS` is (pushed) |
| join to a sub-select with `GROUP BY`/`LIMIT`/`DISTINCT`, a CTE that is read more than once, `VALUES`, a function in `FROM` | PostgreSQL does not pull them up to a base table, so the right side is a plan, not a table the scan can read at start; running a child plan under the node is not done |
| join with no equality between leaves and columns of the table (a range or inequality join, `ON true`) | nothing to hash the table by; each distinct tuple would scan the table |
| more than 3 keys, more than 4 joined tables, more than 8 combos | the limits of the spec (`SIFT_AGG_MAX_JOINS`, `SIFT_AGG_MAX_COMBOS`) |
| key or filter that is volatile (`random()`, `clock_timestamp()`) | PostgreSQL evaluates it per row; the combo cache evaluates it once per distinct tuple |
| the same leaf with two types in one combo (`varchar` leaf compared with a `text` column and `IS NULL` of the leaf as `varchar`) | the combo types each leaf once; the relabelled use is another type |
| join key of text with a nondeterministic collation | the equality of the collation is not byte equality of the dictionary entries |
| `sum`/`avg`/`min`/`max` of a joined `money` column | no hash opclass for the distinct-values table of mapped aggregates |
| `sum(float4)` of a joined column | float4 addition is order dependent: the distinct-values form adds `value * count` |
| `sum`/`avg` of other types of a joined column (`numeric` and `float8` are matched; `int2`/`int4`/`int8`, `interval` and `avg(float4)` through the PostgreSQL functions) | the aggregate is not in the allowlist of `mapped_pg_agg`; a function whose transition state is not a copyable datum cannot run per distinct value |
| volatile outer qual above the arms (`WHERE random() < 0.5`) | PostgreSQL evaluates it once per inner group; the scan would evaluate it per cell, in another order; fixture `t_two_level_where_volatile` |
| outer qual above the arms that mixes a group column of an arm with an inner aggregate (`avg_price > 100 OR kind = ..`) | the column belongs to the cell's outer group, which is known only when the cell is attributed; fixture `t_two_level_where_mixed` (the inner aggregate may still run in the scan) |
| outer qual that reads the columns of several arms of the `UNION ALL` (it stays as a filter of the append) | a cell belongs to one arm; the qual would need the other arms' cells of the same row, which the scan has not seen yet |
| `count(DISTINCT id)` alone over the arms, without a key | priced on the normal plan (not investigated: the cost of the two plans decides) |

## Requests to the column engine

Not done here (the engine files are not part of this track). In order of what they would buy:

1. **Lookup read through the seam.** `map_load` reads the joined table from the heap with the query's snapshot at executor start (a `sift_table` or `sift_col` table is read through the heap AM). A `sift_colsrc` call that returns the rows of a table with an index as dictionary codes and integers (`sift_colsrc_lookup`) would give the same snapshot rules without the tuple deforming, and let the same keys be hashed once for the parallel kinds.
2. **Stable dictionary ids across units.** A combo evaluates PostgreSQL's expressions once per distinct tuple of leaf values; the cache is keyed by the values of the unit's dictionary, so a column with a dictionary per unit re-evaluates at every unit. A global (per index) id for each value, or the per-unit remap table kept by the engine, would turn those re-evaluations into lookups.
3. **Partial tables for the serial-only kinds.** Joins, arms, sets, mapped aggregates: the state is a `dset` of (group, value) pairs, a cell table and per-set group tables. A dump and a merge by value (documented in "Parallel scan") for each would give them workers; the engine side needs a worker-shared view of the units after the pruning and per-unit row-id ranges for arms (a row never spans two workers).
4. **`ndistinct`, `min`, `max`, `elems_per_row`, `zone_width` of the "Statistics request".** The cost of the lookup joins and the arms uses a default of 3 elements per row and no key cardinality; with the numbers the cost could price the combo cache (distinct tuples) and the cell table (groups per row) instead of a flat charge.
5. **`money` leaf.** `money` is an `int8` with scale 2 (`cash_*` functions); a leaf of that type would make the column a key and an aggregate of the main table, and a joined `money` column hashable through it.

## Requests to the column engine (generic aggregates)

What the generic aggregates would gain from the engine (`src/sift_col*`, not changed here), in the order of what they save:

- **Datum-ready text and numeric reads**: `sift_colsrc_read` gives codes into a per-unit dictionary and scaled int64; every generic aggregate over text builds a `text` Datum per row from the dictionary bytes, and over a numeric builds a `numeric` from the integer (`int64_to_numeric`). A read that returns the varlena of each distinct code once per unit (a `Datum[]` over the dictionary, built lazily) and a `numeric` Datum per distinct scaled value would take that out of the per-row path for text keys and for low-cardinality numbers.
- **More scalar leaf types**: the generic aggregates read bool, int2/4/8, date, timestamp(tz), float4/8, text, varchar and numeric(p <= 18). `uuid`, `time`, `interval`, `bytea`, `char(n)` and `numeric(p > 18)` columns are refused at `CREATE INDEX` or by the planner, so `min(uuid)`, `array_agg(interval)` and the like keep the plan.
- **The sign of a float zero**: the index stores `-0` as `0`. A generic aggregate whose result shows it (`array_agg(f8)`, `string_agg(f8::text)`, `jsonb_agg`) is refused for that reason; keeping the sign bit (a float leaf that round-trips the bits) would let them match.
- **Row order**: a unit gives its rows in TID order and the units come in superblock order, which is the heap order the order-sensitive aggregates (`array_agg`, `string_agg` without `ORDER BY`) see in a sequential scan. After a rewrite of the index (merge, promote) that order is what the source documents; a statement in `docs/COL.md` that the read order is the TID order of the table would make it a guarantee.
- **Per-dictionary order**: `DISTINCT` and ordered sets over text sort by the type's comparison function on decoded values. A dictionary that is sorted in the collation's order (or a rank per code) would make `min`/`max`/`DISTINCT`/`percentile_disc` of a text leaf integer work.
- **Distinct counts**: `ndistinct` of the leaves (`SiftSrcStats`) would size the `DISTINCT` hash and the groups of a generic aggregate (today a flat price per group).

## Open

- The fused pass takes arms that differ only in their keys and read the same leaves; arms that read a leaf the others do not (a key on its own column) keep their own pass. A fused spec with the union of the leaves would take them too.
- A top-k per group inside the node (S5), and decode-bound shapes (S3, S6, S7 at K4000): fewer bytes per element (the requests to the column engine below).
- Partial tables for the per-order arms and the mapped aggregates (above, why), and for `count(DISTINCT)` of a numeric expression.
- The statistics of the "Statistics request" above.
- Generic aggregates in parallel: the partial states of the aggregates that have a combine function (`stddev`, `sum`, `bool_*`, `array_agg`, ...) dumped as datums and merged with it; `DISTINCT` and ordered sets need the values moved, not the states.
- `ORDER BY` inside the call (`string_agg(x, ',' ORDER BY y)`, `array_agg(x ORDER BY y)`): a tuplesort per group on the order keys, then the transition function; hypothetical-set and variadic aggregates; an expression of a joined column.
- Generic aggregate memory: the `DISTINCT` pairs of a group are kept until the scan ends and are not bounded by `work_mem` (the ordered sets are).
- `count(DISTINCT)` is a hash set per aggregate (about 25 ns per tuple): a per-group bitmap or sort-based form for dense domains.
- A lookup-table expression (a key or a filter that is not a plain comparison) over an integer column whose values span more than 2^22 raises an error with a hint to switch `sift.enable_agg` off; plain group keys, comparisons and `IN` lists over such columns work (hash ids, binary search).

### Requests to the column engine

Dependencies on the column engine. The shapes still under 20 times (S3 14 to 19, S4 17, S5 4.7, S6 6, S7 7) wait for the decode work of the core branch, which is a separate task that follows it; the call sites in the executor are written against local helpers. S5 also needs a top-k per group inside the node. The dependencies are the five items below plus a statistic: (1) `read_positions` once per unit, shared by its leaves; (2) `col_read` decoding straight to the vector type of the leaf; (3) `expand_parents` storing one entry per row; (4) a page table per unit for `nd_i64`, `nd_u32` and `unit_page`; (5) the `ndistinct` statistics from the providers (`sift_aggcol.c` has `nrows`, `nunits` and `allvisible` only), so that the group tables and the unnest cost are sized from data, not defaults.

What the profiles of the decode-bound shapes (S3, S4, S6, S7 at K4000, flat otherwise) put in the engine's functions, as a share of the cycles of the query:

- `read_positions` (5 to 13 percent: S7 13, S3 10, S4 6, S6 5) decodes the row positions of a unit for each leaf the executor reads; one decode per unit shared by its leaves would pay once.
- `col_read` (5 to 6 percent in all four) narrows an int64 leaf to the int32 the executor wants and copies it; a decode straight to the vector type of the leaf, aligned for the kernels, would drop the second pass.
- `expand_parents` (kernel, 4 percent of S3, 6 of S4) stores one entry per element; one store of the row ordinal per row (the next row overwrites the tail) is a pass over the rows, not the elements.
- `nd_i64` / `nd_u32` (the null expansion of a decoded leaf: 6 to 8 percent in S3, S4, S7) and `unit_page` (3 to 5 percent in S4 and S6, the page of a row in the unit) are called per leaf and per row batch; a page table built once per unit would serve every leaf.
