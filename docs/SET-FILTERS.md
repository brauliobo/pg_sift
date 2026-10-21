# Set filters: array parameters, lookup tables and dimension registries (3.2)

A set filter keeps the rows of a fact table whose key is in a set: the ids of a group, the ids of the entities that have an attribute. This document
is how the aggregate pushdown (`Custom Scan (SiftAgg)`, [docs/AGG.md](AGG.md)) answers them, what each way costs, and the settings that choose between them.
Every figure is from a stored log under [docs/results/](results/) on the synthetic data of [bench/sets/](../bench/sets/README.md) (below, "the dataset").

## 1. The dataset and the method

`bench/sets/gen.sql` and `build.sql` make, deterministically (`setseed`), a fact table `events` (3,000,000 rows, 173 MB heap, `sift_col` index 19 MB, 87 units,
a `deleted_at` on 1.5% of the rows, a date from 2021-01 to 2026-10 with a backfill tail, a zipf distribution of events over 200,000 entities: p50 5, p90 20, p99 135,
at most 84,637 events per entity, 30 channels), a dimension table `entities` (200,000 rows: `city_id`, `country_id` (60), `category_id` (12), `tier`, `chain_id`,
`region_id`) and a membership table `group_members` (60 groups of 100 entities, 20 of 1,000, 4 of 2,000, 4 of 5,000, 8 of 10,000, 2 of 100,000; 334,000 rows, soft-deleted by `deleted_at`).
Sets of 1,000, 2,000 and 5,000 entities are the sizes this document is about (the larger sets of an application today); 10,000 and 100,000 are headroom rows.
The query of a set filter is `count(*)` and `avg(score)` per channel of the events of the last 12 months that are not deleted, restricted to the set.

Runs: PostgreSQL 18.6, `bench/sets/run.sh` (a `systemd-run --user --scope` with `MemoryMax=12G`, `CPUQuota=300%`, the host lock `/tmp/sift-perf.lock`),
`shared_buffers = 2GB`, `jit = off`, `max_parallel_workers_per_gather = 2`, one warm-up and 5 timed runs per cell interleaved in random order, the median is
reported, client side (`\timing`: planning and execution). The host is shared (load 4 to 13 on 16 CPUs while they ran; the start and end load is in the first line of every log, and
the ratios between plans of one log are the result, not the absolute milliseconds).
"normal plan" is `sift.enable_agg = off`. Every cell's answer is compared with the normal plan's (`result_ok` is `True` in every row of every log).
The real distributions of a deployment move the absolute numbers, the ratios between the plans are the result.

## 2. The set as one array parameter (`col = ANY($1::int4[])`)

The set is resolved by the application and sent as one `int4[]` parameter of a prepared statement on a generic plan. PostgreSQL does not plan the
elements (the text of a literal `IN (...)` list is 6.4 KB at 1,000 ids, 64 KB at 10,000 and 644 KB at 100,000, and the planner works through it), and `SiftAgg` turns
the array into a byte table when the scan starts, so the cost is nearly independent of the size of the set.

The planner cannot see how long a parameter array is: it assumes 10 elements, prices a bitmap scan for them, and picks it over `SiftAgg` at any size. So that
the generic plan takes `SiftAgg` without the application switching index and bitmap scans off, the paths that fetch the rows of such a set one at a time are costed
for `sift.agg_param_set_size` elements (default 1000; 0 gives the planner's own guess back), and only when the query can run as `Custom Scan (SiftAgg)`
(a query this node cannot take keeps the plan it had; a custom plan, which sees the array, is costed as before).

| set size | normal plan | literal `IN` list (SiftAgg) | array, generic plan, before (planner guess) | array, generic plan, 3.2 default | custom plan |
|---|---|---|---|---|---|
| 100 | 3.0 ms | 3.1 ms | 2.7 ms (bitmap) | 5.0 ms | 3.0 ms (bitmap) |
| **1,000** | 10.0 ms | 6.0 ms | 10.3 ms (bitmap) | **4.8 ms** | 6.0 ms |
| **2,000** | 15.0 ms | 7.4 ms | 15.0 ms (bitmap) | **5.1 ms** | 6.9 ms |
| **5,000** | 30.1 ms | 11.4 ms | 30.2 ms (bitmap) | **5.5 ms** | 10.6 ms |
| 10,000 | 33.9 ms | 17.8 ms | 32.4 ms (bitmap) | 5.6 ms | 15.8 ms |
| 100,000 | 103.6 ms | 129.8 ms | 100.5 ms (bitmap) | 10.4 ms | 116.2 ms |

(`docs/results/sets-array-param.tsv`: medians of 5 runs; "normal plan" is the generic array statement with `sift.enable_agg = off`; the literal list with `sift.enable_agg = off` takes 2.6 / 9.6 / 65.1 / 65.3 / 79.1 / 155.3 ms;
the literal list and custom plan columns are planned with the values.) At 1,000, 2,000 and 5,000 members the 3.2 default is 2.1, 2.9 and 5.5 times faster than the normal plan (and than the generic plan without
the setting, which keeps the bitmap scan), and its time hardly moves with the size of the set (4.8 to 5.5 ms): the array becomes a byte table once, where the bitmap scan fetches the rows of every member.
The generic plan with the 3.1 workaround (`sift.agg_param_set_size = 0, enable_indexscan = off, enable_bitmapscan = off`) gives 5.3 / 5.0 / 4.8 / 5.2 ms at 100 / 1,000 / 2,000 / 5,000: the same plan. The crossover is
between 100 and 1,000 members: at 100 the normal plan is faster by 2 ms (3.0 against 5.0 ms), and such a set is better sent with a custom plan (`plan_cache_mode = force_custom_plan`, the
default `auto` for the first 5 executions) or `SET sift.agg_param_set_size = 10`.

### Key types of the array

The array is the type of the column (`ANY($1::int8[])`, `ANY($1::text[])`, `ANY($1::date[])`). The same query, 2,000,000 rows with about 200,000 distinct keys in every key column, a generic plan, the pushed plan
against the normal plan (`docs/results/sets-array-types.tsv`, `bench/sets/types.sql` and `types.py`; every answer equal):

| key column | 1,000 elements: normal / pushed | 2,000 | 5,000 | how the array is tested |
|---|---|---|---|---|
| `int2` | 35.3 / 2.8 ms | 52.0 / 3.1 ms | 89.8 / 3.8 ms | byte table over the range |
| `int4` | 17.6 / 3.5 ms | 23.2 / 3.6 ms | 40.1 / 4.3 ms | byte table over the range |
| `int8` (bigint) | 18.5 / 5.7 ms | 24.7 / 5.7 ms | 44.6 / 6.5 ms | byte table over the range, else a hash table |
| `date` | 46.6 / 4.2 ms | 67.3 / 4.6 ms | 114.0 / 5.6 ms | byte table over the range |
| `timestamp` | 17.2 / 15.6 ms | 26.1 / 19.5 ms | 41.0 / 16.9 ms | hash table of the values (microsecond values are too far apart for a byte table) |
| `timestamptz` | 18.1 / 16.6 ms | 26.6 / 19.2 ms | 42.8 / 17.1 ms | hash table of the values |
| `text` | 18.4 / 24.0 ms | 28.7 / 29.6 ms | 45.7 / 27.4 ms | PostgreSQL evaluates `= ANY` once per distinct value, with a hash table of the array |
| `uuid`, `bytea` | | | | not supported: `CREATE INDEX ... USING sift_col` refuses these columns |

The integer, `int8` and date keys cost 3 to 6 ms at any size (a byte table: one byte read per row), `timestamp(tz)` 16 to 19 ms (a hash probe per row). Before this branch an `int8`, `text` or `timestamp` key was a PostgreSQL expression that searched the array
linearly for each distinct value of the column: 355 ms for 1,000 `int8` keys and 3,099 ms for 1,000 `text` keys (1,664 and 18,403 ms for 5,000), 20 to 450 times slower than the normal plan
(`docs/results/sets-array-types-linear.tsv`). Hashing the array once gave 37 to 39 ms for `int8` and 40 to 49 ms for `timestamp(tz)`, and about 25 ms for `text`
(`docs/results/sets-array-types-expression.tsv`); the native tests above are for `int8` and `timestamp(tz)`. A `text` key still costs one PostgreSQL evaluation per distinct value of the
column and unit (about 25 ms here), so a `text` set is faster than the normal plan only from about 2,000 elements (0.8x at 1,000, 1.0x at 2,000, 1.7x at 5,000); `timestamp(tz)` keys are 1.1 to 2.5x
faster. Send ids, not names, as the set.

### Binding the array

The statement must be a server-side prepared statement or a bound one, and its plan generic: `SET plan_cache_mode = force_generic_plan` for the transaction
(`SET LOCAL`, so that connection poolers in transaction mode keep it). A client that quotes the ids into the SQL text sends a literal list.

```c
/* libpq: one text parameter of type int4[] (OID 1007), the array in its text form */
Oid         types[1] = {1007};
const char *vals[1] = {"{101,102,103}"};
PQexec(conn, "BEGIN");
PQexec(conn, "SET LOCAL plan_cache_mode = force_generic_plan");
PGresult *r = PQexecParams(conn,
    "SELECT channel_id, count(*), avg(score) FROM events "
    "WHERE deleted_at IS NULL AND event_date >= '2025-10-01' AND entity_id = ANY($1::int4[]) GROUP BY 1",
    1, types, vals, NULL, NULL, 0);
```

```ruby
# Sequel (the pg_array extension binds the array as one parameter of a server-side prepared statement)
DB.extension :pg_array
totals = DB[:events].where(deleted_at: nil).where { event_date >= Date.new(2025, 10, 1) }
           .where(Sequel.lit('entity_id = ANY(?::int4[])', :$ids))
           .select_group(:channel_id).select_append { [count.function.*, avg(:score).as(:avg_score)] }
           .prepare(:select, :set_totals)
DB.transaction do
  DB.run 'SET LOCAL plan_cache_mode = force_generic_plan'
  totals.call(ids: Sequel.pg_array(ids, :int4))
end
```

`NULL` elements, an empty array and duplicates keep the meaning they have in SQL (`x = ANY('{}')` is false, `x <> ALL('{}')` is true for every row including a NULL `x`,
a NULL element makes `<> ALL` never true); an element outside the range of the column matches nothing. `test/sql/agg_set_array.sql` compares them with the normal plan.
A set whose values span more than 4 million (2^22) is searched in a sorted array instead of the byte table (slower, same answers).

## 3. The set as a table: `IN (SELECT ..)`, `EXISTS`, `NOT EXISTS` (`sift.agg_semi_lut`)

A set kept in a table (a membership table, the entities that have an attribute) is a semi join of the fact table, and `NOT EXISTS` an anti join.
`Custom Scan (SiftAgg)` loads the table when the scan starts, and when the join compares one integer or date leaf with one key column and gives no column
back, it makes from the keys a byte table over their range. Every tuple then costs one byte read, where the generic lookup join makes a cache entry per
distinct value of the leaf (a hash lookup of the rows of the table each). The table's own conditions (`group_id = 3 AND deleted_at IS NULL`, `country_id = 7`)
are tested once per row of the table, not once per probe.

| query (3M events, 12 months, `count`, `avg` per channel) | normal plan | SiftAgg, generic lookup join | SiftAgg, byte table (default) |
|---|---|---|---|
| `IN`, group of 100 members | 3.1 ms (index scan) | 21.3 ms | 6.1 ms |
| **`IN`, group of 1,000 members** | 66.0 ms (hash join) | 27.0 ms | **6.0 ms** |
| **`IN`, group of 2,000 members** | 68.9 ms | 24.5 ms | **6.1 ms** |
| **`IN`, group of 5,000 members** | 62.7 ms | 26.6 ms | **7.0 ms** |
| `IN`, group of 10,000 members | 72.4 ms | 35.6 ms | 7.7 ms |
| `IN`, group of 100,000 members | 99.4 ms | 68.8 ms | 31.0 ms |
| `EXISTS`, group of 1,000 | 70.6 ms | 25.9 ms | 5.6 ms |
| `NOT EXISTS`, group of 1,000 | 85.2 ms | 32.2 ms | 9.7 ms |
| `IN`, attribute with 3,300 entities (a country) | 68.4 ms | 26.1 ms | 10.0 ms |
| `NOT EXISTS`, the same | 90.8 ms | 35.6 ms | 13.3 ms |
| `IN`, attribute with 17,000 entities (a category) | 78.2 ms | 31.0 ms | 11.7 ms |

(`docs/results/sets-semi-lut.tsv`, 19 queries by 4 modes: `normal`, the shipped `default`, and `SiftAgg` forced with the join methods off, `siftagg-generic-join` (the semi table off) and `siftagg-semi-table`
(the columns above); the medians of 5 runs, every answer equal to the normal plan's. The byte table column includes the lookup load of section 4, on by default in this branch.) The byte table is 1.9 to 4.6 times faster than the generic lookup join on all 19 queries
(3.5 to 4.5 times for the groups of 100 to 5,000 members), and the default plan is 3.0 to 11.5 times faster than the normal plan on the 16 queries where `SiftAgg` is the plan. The planner picks the faster plan in all 19: it keeps the
index scan for a group of 100 members (3.0 ms against 6.1 ms) and the bitmap scan for an attribute that selects few entities (`tier = 2 AND region_id < 4`, `IN` and `EXISTS`: 8.2 and 6.8 ms against 9.5 and 10.2 ms for the forced `SiftAgg`).
`NOT EXISTS` of a group of 1,000 keeps 99% of the events; the default takes `SiftAgg` (10.4 ms against 85.2 ms).

What the plan costs: the planner prices the semi and anti joins of a `SiftAgg` path by the rows PostgreSQL expects the join to keep (before, every row that passed
the leaf filters was priced as a tuple of the groups), so a selective set is not charged for tuples the join drops. `EXPLAIN ANALYZE` shows `Semi Lookup Tables`
and `Semi Lookup Table Size`.

Limits and fallbacks, each answered by the generic lookup join with the same rows (`test/sql/agg_semi_lut.sql` compares both and the normal plan; the `j_semi_lut_*`
fixtures of `scripts/coldiff.sh` do it in every heap state):

- the keys span more than `sift.agg_semi_lut_size` (default 4 MB, one byte per value of the range; a key outside the range of the leaf's type is ignored, it matches no row);
- the leaf is not an integer, date or `int2` column in the index (text keys, `int8` leaves, array elements), a join has two or more keys, or the table's own condition reads a column of the fact row;
- `NULL` keys: a `NULL` in the table matches nothing, a `NULL` leaf matches nothing (dropped by `IN` and `EXISTS`, kept by `NOT EXISTS`), as in SQL;
- `NOT IN (SELECT ..)` is not a join: PostgreSQL plans it as a hashed subplan (a `NULL` in the table makes the whole answer empty), and the scan asks that subplan for each distinct value of the column (`docs/AGG.md`, "Uncorrelated sub-selects"), not this table;
- each worker of a parallel `SiftAgg` builds its own table, and a rescan keeps it (the groups are replayed). The table is built from the rows visible to the scan's snapshot.

Parallel plans: the semi joins that PostgreSQL pulls up from a sub-select made the whole query count as parallel restricted for `SiftAgg`, and it planned no workers. It now uses the same
per-relation flags PostgreSQL's own grouping does, so these queries get `Workers Planned` like the others (`agg_semi_lut` runs them with workers).

## 4. The lookup table loads only the rows it can match (`sift.agg_lookup_pushdown`)

Before the scan, `SiftAgg` reads the joined table (the membership table, the attribute table) into a lookup table. It used to read every row and apply the table's own conditions
afterwards. Now the planner collects the `column = value` conditions of the join (`group_id = 7`, `country_id = 7`, a parameter included, and `country_id IN (3, 17, 41)` when the statistics expect the list to keep a small share of the table: a larger one is read faster by the heap scan) and the executor evaluates their values once; it reads the longest matching
prefix of a btree index of the table (an index scan, `Lookup Index Scans` in `EXPLAIN ANALYZE`), or, on a table without such an index, passes the equalities to the heap scan as scan keys, and applies the other
conditions (`deleted_at IS NULL`, a boolean) to each row as it is read, so the table keeps only the rows that can match (`Lookup Rows Read`, `Lookup Rows Loaded`, `Lookup Load Time` with `TIMING`). A `NULL`
value gives an empty table. The conditions stay in the join's filter, so a scan key that a table access method ignores (only heap tables get scan keys) changes the cost, not the rows. The planner prices the load by
the rows it expects the conditions to keep when a btree can give them, instead of every row of the table. `SET sift.agg_lookup_pushdown = off` loads the whole table as before.

| query (3M events) | normal plan | lookup join, whole table | lookup join, matching rows only | byte table, whole table | byte table, matching rows only (default) |
|---|---|---|---|---|---|
| `IN`, group of 100 | 3.0 ms | 76.7 ms | 19.9 ms | 25.0 ms | 5.4 ms |
| **`IN`, group of 1,000** | 60.2 ms | 70.8 ms | 22.9 ms | 24.8 ms | **5.5 ms** |
| **`IN`, group of 2,000** | 56.8 ms | 62.4 ms | 24.4 ms | 25.8 ms | **5.8 ms** |
| **`IN`, group of 5,000** | 61.6 ms | 67.6 ms | 24.5 ms | 25.1 ms | **6.7 ms** |
| `IN`, group of 10,000 | 61.6 ms | 75.8 ms | 25.6 ms | 25.2 ms | 7.1 ms |
| `IN`, group of 100,000 | 88.7 ms | 72.3 ms | 55.3 ms | 30.4 ms | 31.3 ms |
| `IN`, a country (3,300 entities) | 62.7 ms | 44.5 ms | 25.9 ms | 16.7 ms | 9.2 ms |
| `IN`, a category (17,000) | 65.4 ms | 40.1 ms | 29.5 ms | 18.2 ms | 10.8 ms |
| inner join to `entities`, a country, grouped by `region_id` | 61.1 ms | 47.4 ms | 26.2 ms | 44.8 ms | 27.8 ms |
| inner join to `entities`, a category, grouped by `region_id` | 64.7 ms | 49.8 ms | 30.6 ms | 49.3 ms | 30.6 ms |

(`docs/results/sets-lookup-load.tsv`, 12 queries by 5 modes, the medians of 5 runs, load 5 to 10; every answer equal to the normal plan's. The first `SiftAgg` columns force the plan with the join methods off.)
Reading the whole membership table (334,000 rows) is the part that the byte table could not remove: on top of the byte table, reading only the matching rows (a btree on `(group_id, entity_id)`) takes the semi join of a group of 1,000 from 24.8 to 5.5 ms (4.5 times), 2,000 from 25.8 to 5.8 ms,
5,000 from 25.1 to 6.7 ms and the attribute filters from 16.7 and 18.2 to 9.2 and 10.8 ms. The byte table with the pushdown is 9 to 11 times faster than the normal plan for the groups of 1,000 to 5,000 and 6 to 7 times for the attribute filters.
It does not help a set that is most of the table (100,000 members: 30.4 against 31.3 ms) nor an attribute query that the normal plan answers from a btree in 6 ms (`tier = 2 AND region_id < 4`: the planner keeps the bitmap scan).
`test/sql/agg_lookup_load.sql` compares the rows with and without the pushdown and with the normal plan (indexed table, table without index, `NULL` and absent parameter values, a group with a `NULL` key and a duplicate).

## 5. Joins that bring a value back, and the external-dimension registry (not built)

A join that returns a column of the joined table (`JOIN entities e ON e.id = events.entity_id ... GROUP BY e.region_id`) cannot be a byte table of presence: every fact row needs the value of its entity. With section 4 the
lookup join of the same query runs in 26 to 31 ms against 61 to 65 ms for the normal plan (2.1 to 2.3 times, last two rows above); without it 47 to 50 ms. Reading the joined table is no longer the cost: the remaining time is
the generic lookup join, which makes a cache entry per distinct entity in the scan (about 200,000) and finds its rows through a hash table.

The registry planned for this step (entity id to attribute codes, loaded once per database or per backend, usable as a filter table and as a group-by code) would remove that join cost (the research figure is 43 to 45 ms down to 8 to 10 ms),
but it carries a staleness problem that the rest of this document does not have: each scan reads the rows visible to its own snapshot, a registry is a copy. The safe default would have to refuse a registry older than the
table's changes (a modification counter or xmin horizon per registered table, `sift.refresh_dimension()`, a maximum age as an opt-in), keep a per-backend or shared-memory copy with a size cap and repeat the load
in every backend that uses it. It was not built: for the sets of 1,000 to 5,000 entities sections 2 to 4 already give 5 to 7 ms with no copy to keep fresh, and the value join is 2.1 to 2.3 times faster than the normal plan without it.
If a value join at 8 to 10 ms is needed, the next step with no staleness is a direct-address table of the codes built from the joined table at the start of each scan (as the byte table of section 3 is, with one more column), which the lookup
table of section 4 already makes cheap to fill. It was not attempted here.

## 6. The attribute copied into the fact table, and clustering by it

The other way to answer "events of the entities of a country" is to have the country in the event row: `country_id` becomes a column of the fact table's `sift_col` index and the filter is `country_id = 7`, an
ordinary comparison with zone pruning. `bench/sets/denorm.sql` builds the two copies measured here, `events_dn_u` (insertion order) and `events_dn` (stored in `(country_id, event_date, id)` order, a `CREATE TABLE ... AS SELECT ... ORDER BY`),
with `sift_col` indexes over `(entity_id, channel_id, lang_id, signal, event_date, score, deleted_at, country_id, category_id)` (23 MB and 16 MB, against 19 MB for the table without the two columns).

| query (3M events) | normal plan | semi join to `entities` (section 3) | `country_id = 7`, insertion order | `country_id = 7`, clustered |
|---|---|---|---|---|
| a country (3,300 entities) | 41.4 ms (seq scan) | 9.1 ms | 3.8 ms | **1.9 ms** |
| a category (17,000 entities) | 44.1 ms | 10.9 ms | 6.0 ms | 6.8 ms |

(`docs/results/sets-denorm.tsv`, the medians of 5 runs; the normal plan of the semi join is 61.1 and 64.9 ms.) The denormalised column is 2.4 times faster than the semi join in insertion order and 4.8 times faster when the
table is clustered by it (the clustered copy prunes 106 of 113 units, `Units Pruned`; a category is spread over the sort key, so clustering by country does not help it). It is a 2.4 and 4.8 times gain for the country (1.8 and 1.6 times for the category) on a query that already runs in 9 to 11 ms, paid on the write path
(`bench/sets/denorm.py`, `docs/results/sets-denorm-writes.tsv`, all statements rolled back, WAL in bytes with full-page images):

- an entity changes country: updating the 4,844 rows of `entities` of that country writes 7.5 MB of WAL; the copy in the events of those entities (66,168 rows) writes 358 MB in the insertion-order table (48 times) and 75 MB in the clustered table (10 times: the rows are on a few pages);
- an insert of 1,000 events writes 6.29 MB without the copy and 6.32 MB with it: the extra two columns are not a measurable cost per inserted row;
- clustering decays: 3% late rows (90,585 events in insertion order, appended to the clustered copy and vacuumed) put the country in 9 units where it was in 7 (2 more to read, 106 still pruned), and every later batch adds its own tail until the table is rewritten (`CREATE TABLE ... AS SELECT ... ORDER BY` and a swap).

The recipe, then: keep the attribute in the dimension table, and copy it into the fact table only for a filter that a section-3 semi join cannot make fast enough, accepting that a change of the attribute costs a rewrite of every event of the entity (10 to 48 times the WAL of changing the dimension row here).
A `sift_table` option that keeps its rows in the order of a sort key was not built: the gain measured here is at most 2 times over the unclustered column (3.8 to 1.9 ms) and only for a filter on the sort key itself, it would need
a format change (docs/FORMATS.md) and a rewrite of the table to keep the order, and the decay above comes back with the first late batch; the `ORDER BY` of the recipe gives the same layout on a `sift_col` heap table today.
