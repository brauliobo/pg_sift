# Aggregate pushdown

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
| `-- isolation: serializable` | the file runs in a transaction of that isolation level |

The sections `--@ pg`, `--@ flat`, `--@ ch` of an analytics file are cut: only `pg` is run. `-- tol:` is ignored: the contract above is the
tolerance.

The fixture corpus (60 files) has the shapes of the report queries (S1 summary of an object with dynamic keys by key, mapped key and period;
S2 summary of an array of objects with per-order pre-aggregation, the three rollup levels as `UNION ALL` and as `GROUPING SETS`; S4 time
series at day, week, month and quarter, with a date spine; S5 per-store rows and top-N; S6 `jsonb_path_exists` on an array of objects; S7 the
same-element filter as `EXISTS`, `@>` and `jsonb_path_exists`), flat aggregates and quals (`IN`, `NOT IN`, `BETWEEN`, `LIKE`, `OR`, `NOT`,
`CASE`, `IS [NOT] NULL`, `FILTER`, `HAVING`, grouping by expressions), NULL-heavy documents and empty sets, a BM25 match with an aggregate,
a prepared statement with a generic plan, and the traps: `random()` in the qual and in the aggregate, `sum(int4)` and `sum(int8)` past
the type, integer, float and cast errors, NaN, infinities and `-0`, `sum(float4)`, text `min`/`max` under a non-C collation, JSON values
that are not numbers, `ORDER BY` in an aggregate, window functions, `DISTINCT ON`, outer joins, a set-returning function in the select
list, `TABLESAMPLE`, `FOR UPDATE`, a partitioned table and `SERIALIZABLE`.

### Scenarios

After the fresh run, the fixture subset (`-- scenario: yes`) is compared again after DML that leaves the heap in every state the column
index must answer for (`--scenarios`, default all). They are cumulative and only equality is checked:

| scenario | heap state |
|---|---|
| `insert` | 20% more rows without a `VACUUM` (pages not all-visible) |
| `update` | an UPDATE of indexed columns (new tuple versions) and of the jsonb documents |
| `delete` | hard deletes and soft deletes (`deleted_at` set) |
| `abort` | a rolled-back INSERT, UPDATE and DELETE (dead tuples before any `VACUUM`) |
| `vacuum` | `VACUUM` after all that |
| `hot` | UPDATE of columns no index holds: HOT updates (the count is printed) |
| `recycle` | DELETE, `VACUUM (TRUNCATE off)`, then INSERT: new rows on the TIDs of deleted ones (the count is printed) |
| `held` | a second session with an uncommitted INSERT, UPDATE and DELETE next to the corpus, at READ COMMITTED and inside a REPEATABLE READ transaction; then a REPEATABLE READ snapshot taken before a writer commits |
| `maint` | `REINDEX`, then `sift.col_promote` and `sift.col_merge` when they exist |

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
