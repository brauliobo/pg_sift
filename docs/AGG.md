# Aggregate pushdown

## Joins to plans

The right side of a lookup join of `Custom Scan (SiftAgg)` is a table, or a plan: a `VALUES` list, a function (`generate_series`) or a sub-select that
PostgreSQL keeps (`GROUP BY`, `LIMIT`, `DISTINCT`, a set operation, a ranked scan with `LIMIT`) in an inner, `LEFT`, `IN` or `NOT EXISTS` join.
PostgreSQL's plan of the right side is the child of the node (`EXPLAIN` shows it under `Lookup Joins: s (subquery)`); the scan runs it once when it
starts, with the query's snapshot, and its rows are the lookup table, so duplicate keys multiply the rows, a NULL key matches nothing, and the result is the
one of the normal plan (`EXPLAIN ANALYZE` has `Lookup Rows Read`). A rescan replays the groups; a prepared statement's parameters in the right side are read
by the child plan as in any plan.

The normal plan stays when the right side is lateral or reads a volatile function, the join has no equality with the table (`ON true`, `<`), the right side is
estimated above `sift.agg_lookup_max_rows` rows, a condition on a column nothing else reads was pushed into the sub-select (its plan outputs NULL there), the
table is on the nullable side of an outer join. A scan with such a join runs without workers.
Fixtures `pt_*`, test `agg_plan_tables`.

## Partitioned tables

A partitioned table is one source of `Custom Scan (SiftAgg)` when every partition left after PostgreSQL's pruning (plan-time, at every level) has a
`sift_col` index of the same layout: the same columns and expressions in the same order, the same `paths`, and the same pre-aggregate options. The
attribute numbers may differ (a dropped column), and a sub-partitioned branch, a default partition and an unlogged partition are partitions like the rest.
The query is compiled once against the partitioned table's columns; the scan reads the units (segment by superblock) of each partition's index one
after the other, each with its own dictionaries, so the groups, `DISTINCT` sets and the parallel workers (which take units across all the partitions)
work as on one table, and the result is the normal plan's. `EXPLAIN` shows `Index: (partitions)` and `Partitions: a, b, ..`; the cost is the sum of the
partitions' pages and rows. Pre-aggregates answer per unit as on a plain table. Privileges and row security are checked on the partitioned table, as in
PostgreSQL (a role without access to the partitions reads through the parent).

The normal plan stays when a partition has no `sift_col` index, has an index with other columns or options, is not a heap table (`sift_table`, a foreign
table), is read with `ONLY`, or the table is an inheritance parent (a mixed tree too); when a parameter of the query compares the partition key and so
prunes partitions when the plan runs (`$1` in a generic plan: the normal plan prunes, the scan would read every partition); and for the shapes the scan
reads one index for (a ranked search with `sift.score`, a top-N, a time series of unions). A plan is invalidated when a partition or its index is
attached, detached or dropped. `CREATE INDEX` on the parent makes one per partition (also for partitions attached later).
Fixtures `pa_*` and `t_partitioned`, test `agg_partitions`.

## Uncorrelated sub-selects

`col NOT IN (SELECT ...)`, and `col IN (SELECT ...)` under `OR` or `NOT`, are hashed `SubPlan`s in PostgreSQL (a semi join is not possible for them), and
`Custom Scan (SiftAgg)` takes them as a lookup table over the leaf: PostgreSQL's own `SubPlan` answers once for every distinct value of the column, so the NULL rule
of `IN` and `NOT IN` is PostgreSQL's (one NULL in the sub-select makes every `NOT IN` NULL, no row; an empty sub-select makes it true for a NULL value too) and the
test composes with `AND`, `OR`, `NOT`, `CASE` and `FILTER`. The sub-select runs once, when the scan starts, with the query's snapshot, and `EXPLAIN` shows it as
`SubPlan` under the node; an integer column whose values span more than 4 million (the statistics may not show it) is tested row by row instead of through a table; a rescan keeps the answers, and a prepared statement's parameters are read by the sub-select as in any plan.

The normal plan stays when the sub-select is correlated or reads a parameter of the outer query, tests two columns (`(a, b) NOT IN (SELECT x, y ...)`), is not
hashed (too large for `work_mem` and `hash_mem_multiplier`), reads a volatile function, locks rows (`FOR UPDATE`), holds a sub-select of its own or an expression
that may raise (a division, a cast through text) or the tested column is not one the lookup table holds (an `int8` column). A scan with such a test runs without
parallel workers: the leader runs the sub-plans. Fixtures `t_not_in_subquery*`, `t_in_subquery_or`, test `agg_subplan_lut`.

## ctid ranges

`ctid` compared with a constant TID (`=`, `<`, `<=`, `>`, `>=`, either side, and `BETWEEN`) is a predicate of the scan when it is a conjunct of the top level of
the `WHERE`, on a `sift_col` or `sift_table`. The conjuncts make one half-open range of TIDs (keys `block << 16 | offset`, ordered as PostgreSQL orders TIDs). The
block part is a range of superblocks (256 pages, or about 12000 rows of a `sift_table`), so a unit outside it is not read (`Units:` of `EXPLAIN ANALYZE`) and a
unit inside it is read whole; the rows of the first and last block are tested one by one against the range. A heap index stores the TID of the root of a HOT
chain while `ctid` is the visible version's, so those rows are looked up in the heap through the snapshot (HOT keeps the versions in their page, so the
other blocks need no lookup). An empty or crossed range reads no unit, a range beyond the end of the table reads none, and an invalid TID constant is the
PostgreSQL error. The cost keeps the fraction of the pages of the range.

The normal plan stays for `<>`, `ctid` in an expression, in `GROUP BY` or compared with a column, an `OR`, `IN`/`= ANY`, a parameter of a generic plan
(a custom plan folds it), a partitioned table (TIDs are per partition) and the top-N shapes (their probes read zones of units outside the range). PostgreSQL
plans a tid range scan for a narrow range (`enable_tidscan = off` shows the scan). Fixtures `tr_*`, test `agg_tid_range`.

## Workload shapes

The shapes of a check analytics workload (an item table with jsonb metrics_a, a document with an array of parts, entity and mapping tables) are
the synthetic generator `test/col/workload.sql` and the files `bench/sql/workload/shapes/g*.sql` (`bench/sql/workload/run.sh build|plans|time`;
the status of each shape is the first line of its file). Regress test `agg_workload_shapes`, fixtures `wl_*`, coverage rows `WL01` to `WL09`.
Times are the serial plan at 300,000 items, best of 3, with `sift.enable_agg` off and on.

| Shape | Status | Off | On |
| --- | --- | --- | --- |
| g8 `EXISTS` over `jsonb_to_recordset(expr -> 'parts')` of an index expression (was a plan error, `invalid attnum`) | pushed | 1.64 s | 3 ms |
| g8 nested jsonpath `$.parts[*] ? (@.category == "x" && @.tone == "y")` | pushed | 1.52 s | 3 ms |
| g9 / g10 `x = ANY (ARRAY (SELECT ...))`, eight keys | pushed | 116 ms | 2 ms |
| g5 per-item arms, `avg(n::float8)` over a text leaf, count of the items | fused | 2.83 s | 0.07 s |
| g4 a second `count(DISTINCT)` over another column of the row | arms one by one |  | follow-up |
| g7 fused arms for a selective scan (one entity) | not offered |  | follow-up |
| g2 lookup keyed by a column of another joined table | normal plan |  | follow-up |
| g1 `jsonb_exists` / `?` on an object | normal plan |  | follow-up |
| g3 `(doc -> 'k')::int` | normal plan |  | follow-up |
| g6 one string in a typed leaf flags its unit | normal plan, leaf named at `DEBUG1` |  | design note |

What the pushed shapes do:

- **Text-valued leaves (`astext`) in paths and recordsets.** `jsonb_to_recordset(expr -> 'a') AS o(k text)` reads the leaf of a list of objects whose
  member is declared `astext`; a text literal tests it for equality (`=`, `IN`) only when no number or boolean prints as that text (it does not start with a digit or `-` and is not `true` or `false`), so the leaf's text of the number 7 is never taken for the string `'7'`.
  A number or boolean literal against such a leaf, a strict path and a NOT of a test that may be NULL keep the normal plan (plain traps in `wl_path_parts_*`).
- **`EXISTS` over an index expression's array.** The planner deparses the expression with the query's own range table; the same expression in a
  sub-select's range table is what raised `invalid attnum`. The members of one element are tested together, as for a column.
- **`x = ANY (ARRAY (SELECT ...))`.** An uncorrelated, stable sub-select that cannot raise is an InitPlan; the scan runs it once when it starts and the
  rows are the lookup table of the leaf (as for a hashed sub-plan, so no parallel workers). A sub-select that reads the row, is volatile or may raise
  keeps the normal plan (`wl_any_array_subselect_*`).
- **Fused arms over a text leaf.** The inner `sum` / `avg` of a float8 expression the scan evaluates (a cast, `CASE`) is accumulated per row like the
  typed ones. The expression runs for every item the scan reads: the arms must share one WHERE (it is the scan's) and have no inner `FILTER`; a WHERE of
  each arm's own keeps the arms one by one. The plain cast raises for a score that is no number, in the scan as in the heap plan.

## Differential suite

`scripts/coldiff.sh` is the referee of the aggregate pushdown (`Custom Scan (SiftAgg)`): it runs a corpus of plain SQL aggregate queries
in one session with `sift.enable_agg = off`, which is the plan PostgreSQL has without sift, and again with the pushdown on, compares the
two results inside the database, and reads the plans to report which queries really used `Custom Scan (SiftAgg)`. Equality of results
and the shape of the plan are reported separately, so the harness is useful before the planner exists (both runs are then the same
plan: `scripts/coldiff.sh --no-index` is the self-check of the harness).

```sh
scripts/pgtest.sh install && scripts/pgtest.sh start       # a cluster (SIFT_TMP, SIFT_PORT)
scripts/coldiff.sh                                         # both suites, the scenarios, every variant
scripts/coldiff.sh --suite fixtures --corpus '^s7_'        # the fixture queries whose name matches
scripts/coldiff.sh --suite analytics --rows 200000         # the corpus of bench/sql/analytics on a larger table
scripts/coldiff.sh --bench --suite analytics --rows 1200000 --runs 5
scripts/coldiff.sh --selftest                              # the harness against injected differences
```

`make check` runs it as the jobs `difftest-col` (small fixtures, a subset of the variants and scenarios) and `difftest-col-selftest`
(`scripts/ci.sh`); the full tier uses larger tables and every scenario. Without the column index (no `sift_col` access method, or no
`sift_colstub`, the stub of the planner track, in a build with `SIFT_TESTING`; or a failing `CREATE INDEX`) the suites print
`coldiff: SKIP: ...` with the reason and exit 0.

### Suites

| suite | data | queries |
|---|---|---|
| `fixtures` | `test/col/fixture.sql`: an `orders` table of `--fixture-rows` rows (default 4000, `--seed`), hash generated, with NULLs, skews and a fill factor that allows HOT updates; helper tables; edge-case tables (`t_int`, `t_float`, `t_text`, `orders_odd`, `orders_part`) | `test/col/fixtures/*.sql` |
| `analytics` | the generator of `bench/sql/analytics` (`SIFT_ANALYTICS_DIR`, `--rows`, `--stores`), its indexes and `col_index.sql` | its `queries/*.sql` (`--queries GLOB`), the `pg` section of each file |

Each suite has a database of its own (`sift_coldiff_fix`, `sift_coldiff_ana`; `COLDIFF_DB` is the prefix, `--keep` keeps them) with the
conventional indexes (primary key, btree, GIN, and a BM25 index on the fixture's `memo` column), then the column index, then
`VACUUM (FREEZE, ANALYZE)`: every heap page is all-visible. The index DDL of the fixtures is in `scripts/coldiff.sh` (`--col-am`,
`--col-paths`); the analytics index is the statement of its `col_index.sql` (`--col-index FILE`), with `USING sift_col` replaced by
`--col-am` when the stub is the only access method.

### Variants

The reference run is `off`: `sift.enable_agg = off`, `max_parallel_workers_per_gather = 0`. Every other run is compared with it (`--variants`):

| variant | `sift.enable_agg` | parallel workers | `sift.simd` |
|---|---|---|---|
| `off-par` | off | 2 (free parallel costs) | auto |
| `on` | on | 0 | auto |
| `on-par` | on | 2 | auto |
| `on-scalar` | on | 0 | scalar |
| `on-par-scalar` | on | 2 | scalar |

`off-par` checks the tolerance below against PostgreSQL's own parallel plans. A variant is also run through `EXPLAIN (COSTS OFF)`: the plan
has `Custom Scan (SiftAgg)` or not, and `Gather` or not.

### The comparison contract

Each result is materialized in a temporary table and compared by `coldiff.compare` (`test/col/run.sql`):

- the columns (names, types, typmods) and the number of rows are equal; a query must have a total `ORDER BY` and its rows are compared by
  position. A file of the analytics corpus has no `ORDER BY` but a `-- key:` line: both results are sorted by those columns, then by every
  column as text;
- every type but the floats is equal as text: integers, dates, text, booleans, numerics (the scale counts: `1.50` is not `1.5`), jsonb;
- `float4` and `float8`: `|x - y| <= 1e-12 + 1e-9 * max(|x|, |y|)` (`--abs`, `--rel`). Parallel workers and SIMD lanes add in another order, so
  a sum or an average of floats differs in its last digits. NaN equals NaN, an infinity only itself, `-0` equals `0`, NULL only NULL.
  The tolerance is not a license for float4: `sum(float4)` adds in float4 in PostgreSQL, so a plan that adds in double differs by about
  1e-7 relative and fails (the query `t_float4_sum` is there for it);
- a query that raises an error (a cast, an integer or float overflow) must raise an error with the **same SQLSTATE** in every variant (the
  message may name another row's value); such a file says `-- error: yes`, and a reference run that fails otherwise is a failure.

A per-order aggregate (`avg(x)`, `sum(x)` of a float under an outer aggregate) adds the elements of one order from the first to the last, in
double, and divides by their number: the value PostgreSQL gets when its plan feeds the rows of an order in the order of the array (a hash
aggregate over the unnest). PostgreSQL's own plans do not agree among themselves on the last digits: a plan that sorts by the order and the key
adds equal keys in the order of its sort, which is not stable. A condition on the average (`avg >= 9`) that holds exactly for a sum of decimals
(8.7 + 9.0 + 9.3 over three) can then count a few more or fewer orders in one plan than in another (three of 100 000 orders, between the two
arms of one query on the normal plan), whatever the scan does: the scan's count is the one of the first-to-last order. A threshold that must
be reproducible does not rest on the last digit of a float sum: the values are integers (their sums are exact) or the condition
leaves a margin.

A result that differs always fails (exit 1) and prints the first differing rows of both. Nothing is silenced: a query without a result
(an error the runner did not expect) fails too.

### The shape expectation

A file says what the plan should be: `-- expect: agg` (the pushdown must happen once it is implemented), `plain` (it must not happen:
volatile functions, unsupported or risky semantics) or `any`. Only `on` is checked for `agg`; every `on*` variant for `plain`. A mismatch
is a `NOTE` in the last column of the coverage table and in the summary line; it fails only with `--strict-shape`. The coverage table
has, per query and set: the shape, the expectation, `equal` or `DIFF`, whether `on` used SiftAgg, the time of the reference and of
`on` in ms and the speedup.

### Corpus format

One query per file, as in `bench/sql/analytics/queries`: header lines, then the query. Header lines that are not a tag below are the title.

| tag | meaning |
|---|---|
| `-- shape: S1` | the shape of the query (the name of an analytics file of the form `S1` or `G3` is its shape) |
| `-- expect: agg\ | plain\ | any` | the shape expectation (default `any`) |
| `-- sets: K10 K400 K4000` | the query runs once per set of `analytics_sets` (`{stores}`, `{sources}`, `{period(col)}`, `{range(col)}` are expanded as `bench/steps/analytics.py` does); `-` or absent: once |
| `-- key: 1 2` | sort both results by these columns first; `-` compares by position (the query has a total `ORDER BY`); absent: sort by every column |
| `-- scenario: yes` | part of the subset that the scenarios re-run |
| `-- guc: name = value` | a setting for every run of the file (repeatable) |
| `-- prepare: (int[], date)` and `-- args: ...` | the body is a prepared statement with `$1`...; the runs are `EXECUTE`s (with `plan_cache_mode` in a `guc` for a generic plan) |
| `-- requires: SELECT ...` | the file runs only if the statement returns true (repeatable), e.g. a collation that exists |
| `-- wrap: subquery` | materialize `SELECT * FROM (query)`, for a statement that `CREATE TABLE AS` does not take (`FOR UPDATE`) |
| `-- error: yes` | the query raises an error in the reference run |
| `-- error: either` | the query raises an error in the reference run and in the scan; their SQLSTATE may differ (a value the index cannot store: `0A000` against the cast error `22P02`) |
| `-- isolation: serializable` | the file runs in a transaction of that isolation level |

The sections `--@ pg`, `--@ flat`, `--@ ch` of an analytics file are cut: only `pg` is run. `-- tol:` is ignored: the contract above is the
tolerance.

The fixture corpus (about 180 files; lookup joins `j_*` with semi, anti, key expressions and ON filters, per-order arms with sets, gates and general HAVING `s2_per_order_*` / `t_two_level_*`, mapped aggregates `t_mapped_agg_*`) has the shapes of the report queries (S1 summary of an object with dynamic keys by key, mapped key and period;
S2 summary of an array of objects with per-order pre-aggregation, the three rollup levels as `UNION ALL` and as `GROUPING SETS`; S4 time
series at day, week, month and quarter, with a date spine; S5 per-store rows and top-N; S6 `jsonb_path_exists` on an array of objects; S7 the
same-element filter as `EXISTS`, `@>` and `jsonb_path_exists`), flat aggregates and quals (`IN`, `NOT IN`, `BETWEEN`, `LIKE`, `OR`, `NOT`,
`CASE`, `IS [NOT] NULL`, `FILTER`, `HAVING`, grouping by expressions), NULL-heavy documents and empty sets, a BM25 match with an aggregate,
a prepared statement with a generic plan, `numeric(p, s)` columns (predicates against constants of every precision, NaN and the infinities, keys, expressions, distinct counts), group keys up to the limit of 8, rollups and cubes of up to 7 columns (`k_keys_*`, `k_rollup_*`, `k_cube_*`) and the plain traps one past each limit (9 keys, 9 joined tables, 17 combos), lookup joins of up to 8 tables and 16 combos (`j_joins_*`, `j_combos_*`), generic aggregates (`g_*`: the variance family, `DISTINCT` forms over one or several arguments and with a spill, `ORDER BY` in the call, ordered and hypothetical sets, variadic and stable-fraction forms, `array_agg`/`string_agg`, expression arguments, joined columns, rollups, user-defined aggregates, parallel combine, `x_bool_*`: `bool_and`, `bool_or` and `every` merged from two workers) with their traps (a volatile transition function, a float read into text with `-0`), and the traps: numeric division, products past 38 digits and float8 casts, `random()` in the qual and in the aggregate, `sum(int4)` and `sum(int8)` past
the type, integer, float and cast errors, NaN, infinities and `-0`, `sum(float4)`, text `min`/`max` under a non-C collation, JSON values
that are not numbers, `DISTINCT ON`, outer joins, a set-returning function in the select
list, window functions over the aggregate (`w_*`), `TABLESAMPLE`, `FOR UPDATE` and `SERIALIZABLE`; partitioned tables (`pa_*`: pruning at every level, a dropped column before the index, a default and an unlogged partition, `ONLY`, partitions without the index or with other options, inheritance, run-time pruning) run DML through the parent in the scenarios.

### Scenarios

After the fresh run, the fixture subset (`-- scenario: yes`) is compared again after DML that leaves the heap in every state the column
index must answer for (`--scenarios`, default all). They are cumulative and only equality is checked:

| scenario | heap state |
|---|---|
| `insert` | 20% more rows (also of `t_jp` and `t_eo`) without a `VACUUM` (pages not all-visible) |
| `update` | an UPDATE of indexed columns (new tuple versions) and of the jsonb documents (also of `t_jp` and `t_eo`) |
| `delete` | hard deletes (also of `t_jp` and `t_eo`) and soft deletes (`deleted_at` set) |
| `abort` | a rolled-back INSERT, UPDATE and DELETE (dead tuples before any `VACUUM`) |
| `vacuum` | `VACUUM` after all that |
| `hot` | UPDATE of columns no index holds: HOT updates (the count is printed) |
| `recycle` | DELETE, `VACUUM (TRUNCATE off)`, then INSERT: new rows on the TIDs of deleted ones (the count is printed) |
| `held` | a second session with an uncommitted INSERT, UPDATE and DELETE next to the corpus, at READ COMMITTED and inside a REPEATABLE READ transaction; then a REPEATABLE READ snapshot taken before a writer commits |
| `maint` | `REINDEX`, then `sift.col_promote` and `sift.col_merge` when they exist |

### jsonpath truth tables

The fixtures `jp_*` read `t_jp`, whose members `s` (text), `n` (int4) and `b` (boolean) take every value of their leaves (`1` and `1.0`, the
empty string, non-ASCII text, `2147483647`), a JSON null and a missing key. The table is the comparison of an element `e` with a literal `L`
of the member's type; a scalar or null element, and a missing key, are the same as a missing member:

| member of `e` | `@.k == L` | `@.k != L` | `!(@.k == L)` | `!(@.k != L)` |
|---|---|---|---|---|
| a value equal to `L` | true | false | false | true |
| another value of the type | false | true | true | false |
| JSON null | false | true | true | false |
| missing | false | false | true | true |
| a value of another JSON type | unknown | unknown | unknown | unknown |

Against a `null` literal: `== null` is true for a JSON null only, `!= null` for every present member that is not null, and under `!()` the
other rows. Each form pushed down is equal to the heap in every variant and scenario; a form that is not provable (the last row of the
table, a number against a float leaf, strict `!()`, `jsonb_path_match`) is a `plain` fixture. A member the leaf cannot hold (an array or
an object in a leaf of numbers, a number out of range) raises the error of the pushdown, so no `jp_*` fixture can hold one.

### An element against a column of its row

The fixtures `eo_*` read `t_eo`: the members `q` (int4), `q8` (int8) and `f` (float8) of its array against the columns `r4`, `r8` and `rf` of the same row,
with NULL columns, a NaN and a `-0`, a JSON null and a missing member, empty arrays and a NULL document. Element `j` of row `i` holds the value that
`r4` has in the row `i + j - 1`, so every row has elements equal to the columns of its neighbours and the rows that must not match are there. An
element is compared with the column of its own row (`= <> < <= > >=`, either side first), NULL on either side is unknown, and the other tests of the
same element (`AND`, `OR`, `IS NULL`, a literal) are tested on that element. An expression of the column, a column of another type class (an int4 member
against a float8 column), `LIMIT` that is not a constant, `OFFSET` and a second array keep the normal plan (`eo_trap_*`).

### Bench mode

`--bench [--runs N]` runs only the first pass (no scenarios) with the median of N runs (default 5, after a warm run) of the reference and of
`on`, and prints the go/no-go lines: the K4000 queries that must push down (`expect: agg`, and the S shapes of the analytics corpus) at
least 20 times faster than the reference, the K10 and K400 ones that push down not slower (0.95). A target that is missed is printed;
it fails the run only with `--strict-shape`. The times include the temporary table of the result.

### Self-test

`scripts/coldiff.sh --selftest` runs a subset of the fixture corpus plain against plain (must pass) and then with `--mutate KIND`, which perturbs
every `on*` result as a bug of the pushdown would. `int` (+1), `float` (relative 1e-6, beyond the tolerance), `text`, `null`, `rows` (the last
row lost), `dup` (the first row twice), `order` (rows 1 and 2 swapped, seen by a query compared by position), `type` (a wider column type)
and `cols` (an extra column) must fail; `float-tiny` (relative 1e-11, inside the tolerance) must pass; a mutation that applied to no result
fails the self-test.
