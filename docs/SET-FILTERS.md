# Set filters: array parameters, lookup tables and dimension registries (3.2)

A set filter keeps the rows of a fact table whose key is in a set (the ids of a group, the entities with an attribute). This page covers how the
aggregate pushdown (`Custom Scan (SiftAgg)`, [docs/AGG.md](AGG.md)) answers them, what each way costs, and the settings that choose between them.

## 1. The dataset and the method

`bench/sets/gen.sql` and `build.sql` make, deterministically (`setseed`):

- a fact table `events`: 3,000,000 rows, 173 MB heap, `sift_col` index 19 MB, 87 units, a `deleted_at` on 1.5% of the rows, a date from 2021-01 to
  2026-10 with a backfill tail, and a zipf distribution of events over 200,000 entities (p50 5, p90 20, p99 135, at most 84,637 events per entity), 30 channels;
- a dimension table `entities`: 200,000 rows (`city_id`, `country_id` (60), `category_id` (12), `tier`, `chain_id`, `region_id`);
- a membership table `group_members`: 60 groups of 100 entities, 20 of 1,000, 4 of 2,000, 4 of 5,000, 8 of 10,000, 2 of 100,000; 334,000 rows, soft-deleted by `deleted_at`.

Query: `count(*)` and `avg(score)` per channel of the not-deleted events of the last 12 months, restricted to the set. Sets of 1,000, 2,000 and 5,000
entities are the sizes of interest; 10,000 and 100,000 are headroom rows.

Method: PostgreSQL 18.6, `bench/sets/run.sh` (`systemd-run --user --scope`, `MemoryMax=12G`, `CPUQuota=300%`), `shared_buffers = 2GB`, `jit = off`,
`max_parallel_workers_per_gather = 2`; one warm-up and 5 timed runs per cell in random order, median of client-side `\timing` (planning and execution).
Compare ratios within a log, not absolute times.
"normal plan" is `sift.enable_agg = off`; every answer equals the normal plan's (`result_ok` is `True` in every row of every log).

## 2. The set as one array parameter (`col = ANY($1::int4[])`)

The set is sent as one `int4[]` parameter of a prepared statement on a generic plan. The planner does not process the elements (the text of a
literal `IN (...)` list is 6.4 KB at 1,000 ids, 64 KB at 10,000 and 644 KB at 100,000), and `SiftAgg` turns the array into a byte table when the scan
starts, so the cost is nearly independent of the set size.

The planner cannot see the length of a parameter array: it assumes 10 elements, prices a bitmap scan for them, and picks it over `SiftAgg` at any
size. So that the generic plan takes `SiftAgg` without switching index and bitmap scans off, the paths that fetch the rows of such a set one at a time
are costed for `sift.agg_param_set_size` elements (default 1000; 0 gives the planner's own guess back), and only when the query can run as
`Custom Scan (SiftAgg)`; other queries and custom plans (which see the array) are costed as before.

| set size | normal plan | literal `IN` list (SiftAgg) | array, generic plan, before (planner guess) | array, generic plan, 3.2 default | custom plan |
|---|---|---|---|---|---|
| 100 | 3.0 ms | 3.1 ms | 2.7 ms (bitmap) | 5.0 ms | 3.0 ms (bitmap) |
| **1,000** | 10.0 ms | 6.0 ms | 10.3 ms (bitmap) | **4.8 ms** | 6.0 ms |
| **2,000** | 15.0 ms | 7.4 ms | 15.0 ms (bitmap) | **5.1 ms** | 6.9 ms |
| **5,000** | 30.1 ms | 11.4 ms | 30.2 ms (bitmap) | **5.5 ms** | 10.6 ms |
| 10,000 | 33.9 ms | 17.8 ms | 32.4 ms (bitmap) | 5.6 ms | 15.8 ms |
| 100,000 | 103.6 ms | 129.8 ms | 100.5 ms (bitmap) | 10.4 ms | 116.2 ms |

`sift.enable_agg = off` takes 2.6 / 9.6 / 65.1 / 65.3 / 79.1 / 155.3 ms; the literal list and custom plan columns are planned with the values.)

- At 1,000, 2,000 and 5,000 members the 3.2 default is 2.1, 2.9 and 5.5 times faster than the normal plan (and than the generic plan without the
  setting, which keeps the bitmap scan), and its time hardly moves with the set size (4.8 to 5.5 ms).
- The 3.1 workaround (`sift.agg_param_set_size = 0, enable_indexscan = off, enable_bitmapscan = off`) gives 5.3 / 5.0 / 4.8 / 5.2 ms at 100 / 1,000 /
  2,000 / 5,000: the same plan.
- The crossover is between 100 and 1,000 members: at 100 the normal plan is faster by 2 ms (3.0 against 5.0 ms). Send such a set with a custom plan
  (`plan_cache_mode = force_custom_plan`, the default `auto` for the first 5 executions) or `SET sift.agg_param_set_size = 10`.

### Key types of the array

The array has the type of the column (`ANY($1::int8[])`, `ANY($1::text[])`, `ANY($1::date[])`). Same query, 2,000,000 rows with about 200,000 distinct
keys per key column, generic plan, normal plan / pushed plan:

| key column | 1,000 elements: normal / pushed | 2,000 | 5,000 | how the array is tested |
|---|---|---|---|---|
| `int2` | 35.3 / 2.8 ms | 52.0 / 3.1 ms | 89.8 / 3.8 ms | byte table over the range |
| `int4` | 17.6 / 3.5 ms | 23.2 / 3.6 ms | 40.1 / 4.3 ms | byte table over the range |
| `int8` (bigint) | 18.5 / 5.7 ms | 24.7 / 5.7 ms | 44.6 / 6.5 ms | byte table over the range, else a hash table |
| `date` | 46.6 / 4.2 ms | 67.3 / 4.6 ms | 114.0 / 5.6 ms | byte table over the range |
| `timestamp` | 17.2 / 15.6 ms | 26.1 / 19.5 ms | 41.0 / 16.9 ms | hash table of the values (microsecond values are too far apart for a byte table) |
| `timestamptz` | 18.1 / 16.6 ms | 26.6 / 19.2 ms | 42.8 / 17.1 ms | hash table of the values |
| `text` | 18.4 / 24.0 ms | 28.7 / 29.6 ms | 45.7 / 27.4 ms | PostgreSQL evaluates `= ANY` once per distinct value, with a hash table of the array |
| `uuid`, `bytea` |  |  |  | not supported: `CREATE INDEX ... USING sift_col` refuses these columns |

- The planner prices the test of the list by the table that makes it: 1 ns a row for a byte table in the cache, 2 for a bit table, 10 for a byte table wider than the cache and 5 (200 values) to 10 (1,000) for a hash table of spread values, so a short list over a wide key keeps the btree plan that reads its few rows (a scan of 1,000,000 rows takes 8 to 11 ms for 300 to 1,000 spread values, the btree plan under 1 ms).
- Integer, `int8` and date keys cost 3 to 6 ms at any size (a byte table: one byte read per row); `timestamp(tz)` 16 to 19 ms (a hash probe per row).
- Before 3.2 an `int8`, `text` or `timestamp` key searched the array linearly for each distinct value of the column: 355 ms for 1,000 `int8` keys and
  3,099 ms for 1,000 `text` keys (1,664 and 18,403 ms for 5,000), 20 to 450 times slower than the normal plan.
  Hashing the array once gave 37 to 39 ms for `int8`, 40 to 49 ms for `timestamp(tz)` and about 25 ms for `text`
; `int8` and `timestamp(tz)` now have the native tests above.
- A `text` key still costs one PostgreSQL evaluation per distinct value of the column and unit (about 25 ms here): a `text` set beats the normal plan
  only from about 2,000 elements (0.8x at 1,000, 1.0x at 2,000, 1.7x at 5,000); `timestamp(tz)` keys are 1.1 to 2.5x faster. Send ids, not names.

### Binding the array

Use a server-side prepared or bound statement with a generic plan: `SET plan_cache_mode = force_generic_plan` for the transaction (`SET LOCAL`, so transaction-mode poolers keep it). A client that quotes the ids into the SQL text sends a literal list.

```c
/* libpq: one text parameter of type int4[] (OID 1007), the array in its text form */
Oid         types[1] = {1007};
const char *vals[1] = {"{101,102,103}"};
PQexec(conn, "BEGIN");
PQexec(conn, "SET LOCAL plan_cache_mode = force_generic_plan");
PGresult *r = PQexecParams(conn,
    "SELECT channel_id, count(*), avg(score) FROM events "
    "WHERE deleted_at IS NULL AND event_date >= '2025-10-01' AND dim_a_id = ANY($1::int4[]) GROUP BY 1",
    1, types, vals, NULL, NULL, 0);
```

```ruby

# Sequel (the pg_array extension binds the array as one parameter of a server-side prepared statement)
DB.extension :pg_array
totals = DB[:events].where(deleted_at: nil).where { event_date >= Date.new(2025, 10, 1) }
           .where(Sequel.lit('dim_a_id = ANY(?::int4[])', :$ids))
           .select_group(:channel_id).select_append { [count.function.*, avg(:score).as(:avg_score)] }
           .prepare(:select, :set_totals)
DB.transaction do
  DB.run 'SET LOCAL plan_cache_mode = force_generic_plan'
  totals.call(ids: Sequel.pg_array(ids, :int4))
end
```

`NULL` elements, an empty array and duplicates keep their SQL meaning (`x = ANY('{}')` is false, `x <> ALL('{}')` is true for every row including a NULL `x`, a NULL element makes `<> ALL` never true); an element outside the column's range matches nothing (`test/sql/agg_set_array.sql` compares with the normal plan). A set spanning more than 4 million values (2^22) is searched in a sorted array instead of the byte table (slower, same answers).

## 3. The set as a table: `IN (SELECT ..)`, `EXISTS`, `NOT EXISTS` (`sift.agg_semi_lut`)

A set kept in a table (a membership table, the entities with an attribute) is a semi join of the fact table; `NOT EXISTS` is an anti join. `Custom Scan (SiftAgg)` loads the table when the scan starts, and when the join compares one integer or date leaf with one key column and returns no column, it builds a byte table over the key range: one byte read per tuple, where the generic lookup join makes a cache entry (a hash lookup) per distinct leaf value. The table's own conditions (`group_id = 3 AND deleted_at IS NULL`, `country_id = 7`) are tested once per table row, not per probe.

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

- The byte table is 1.9 to 4.6 times faster than the generic lookup join on all 19 queries (3.5 to 4.5 times for groups of 100 to 5,000 members); the
  default plan is 3.0 to 11.5 times faster than the normal plan on the 16 queries where `SiftAgg` is the plan.
- The planner picks the faster plan in all 19: the index scan for a group of 100 members (3.0 ms against 6.1 ms) and the bitmap scan for an attribute
  that selects few entities (`tier = 2 AND region_id < 4`, `IN` and `EXISTS`: 8.2 and 6.8 ms against 9.5 and 10.2 ms for the forced `SiftAgg`).
- `NOT EXISTS` of a group of 1,000 keeps 99% of the events; the default takes `SiftAgg` (10.4 ms against 85.2 ms).

Costing: semi and anti joins of a `SiftAgg` path are priced by the rows PostgreSQL expects the join to keep (before 3.2, every row passing the leaf filters), so a selective set is not charged for tuples the join drops. `EXPLAIN ANALYZE` shows `Semi Lookup Tables` and `Semi Lookup Table Size`.

Limits and fallbacks (each falls back to the generic lookup join with the same rows; `test/sql/agg_semi_lut.sql` and the `j_semi_lut_*` fixtures of `scripts/coldiff.sh` compare both and the normal plan in every heap state):

- the keys span more than `sift.agg_semi_lut_size` (default 4 MB, one byte per value of the range; a key outside the range of the leaf's type is ignored, it matches no row);
- the leaf is not an integer, date or `int2` column in the index (text keys, `int8` leaves, array elements), a join has two or more keys, or the table's own condition reads a column of the fact row;
- `NULL` keys: a `NULL` in the table matches nothing, a `NULL` leaf matches nothing (dropped by `IN` and `EXISTS`, kept by `NOT EXISTS`), as in SQL;
- `NOT IN (SELECT ..)` is not a join: PostgreSQL plans it as a hashed subplan (a `NULL` in the table makes the whole answer empty), and the scan asks that subplan for each distinct value of the column (`docs/AGG.md`, "Uncorrelated sub-selects"), not this table;
- each worker of a parallel `SiftAgg` builds its own table, and a rescan keeps it (the groups are replayed). The table is built from the rows visible to the scan's snapshot.

Parallel plans: semi joins pulled up from a sub-select make these queries get `Workers Planned` like the others (`agg_semi_lut` runs them with workers);
before 3.2 they counted as parallel restricted and planned no workers.

## 4. The lookup table loads only the rows it can match (`sift.agg_lookup_pushdown`)

Before the scan, `SiftAgg` reads the joined table (membership or attribute table) into a lookup table. It used to read every row and filter afterwards; now:

- the planner collects the join's `column = value` conditions (`group_id = 7`, `country_id = 7`, parameters included, and `country_id IN (3, 17, 41)` when statistics expect the list to keep a small share of the table: a larger one is read faster by the heap scan) and the executor evaluates their values once;
- it reads the longest matching prefix of a btree index of the table (`Lookup Index Scans` in `EXPLAIN ANALYZE`) or, without such an index, passes the equalities to the heap scan as scan keys; the other conditions (`deleted_at IS NULL`, a boolean) apply per row as read, so the table keeps only rows that can match (`Lookup Rows Read`, `Lookup Rows Loaded`, `Lookup Load Time` with `TIMING`). A `NULL` value gives an empty table;
- the conditions stay in the join filter, so a scan key that a table access method ignores (only heap tables get scan keys) changes the cost, not the rows;
- the load is priced by the rows the conditions are expected to keep when a btree can give them, instead of every row.

`SET sift.agg_lookup_pushdown = off` loads the whole table.

The same index loads a table by the keys of another lookup. A table that a later inner or semi join keys by one of its columns alone (the request's ids, a CTE or sub-select of them, joined to the table of their rows), or that a CTE of ids joins on the same leaf of the scan, makes a tuple only with those keys. The loader reads them first and probes a btree that starts with the column, once for each key (`Lookup Index Scans`, and `Lookup Rows Read` is the keys' rows), while that costs less than the pages of the table at the planner's page cost (two random pages a key against `Min(seq_page_cost, random_page_cost)` a page, the price `join_load_rows` charges the read of the whole table); more keys than that read the table whole, as before, and the planner prices the probes the same way. Measured on an owners table of 90,000 rows and 1,500,000 events (a CTE of the request's owner ids joined to the owners table; the table read whole against probes, best of five): 1 owner 83.6 to 1.1 ms, 10 owners 86.4 to 3.1, 100 owners 86.7 to 10.1, 300 owners 96.8 to 16.3, 1,000 owners 101.8 to 30.5 (stock page costs); with `seq_page_cost` 100 and `random_page_cost` 4, 3,000 owners 113.2 to 46.7 and 6,000 owners 112.5 to 55.7; 12,000 owners read the table whole in both (the same plan), and at 10,000 to 100,000 events the probe is a tie to 3x faster. The cost grows with the table's pages when it is read whole and with the keys when it is probed, so the gain is larger the larger the lookup table. The probe adds no index and no memory beyond the keys' rows; it applies to any such lookup table with a btree on the key column. The rule counts the rows a key has (the statistics' distinct values of the column; a unique index when there are none): a key of many rows reads a page for each, so a request of fewer keys pays than for a unique key. `sift.agg_lookup_pushdown = off` reads the table whole. 
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

The first `SiftAgg`
columns force the plan with the join methods off.)

- Reading the whole membership table (334,000 rows) is the part the byte table could not remove. On top of the byte table, reading only the matching rows
  (a btree on `(group_id, dim_a_id)`) takes the semi join of a group of 1,000 from 24.8 to 5.5 ms (4.5 times), 2,000 from 25.8 to 5.8 ms, 5,000 from 25.1 to
  6.7 ms and the attribute filters from 16.7 and 18.2 to 9.2 and 10.8 ms.
- The byte table with the pushdown is 9 to 11 times faster than the normal plan for the groups of 1,000 to 5,000 and 6 to 7 times for the attribute filters.
- It does not help a set that is most of the table (100,000 members: 30.4 against 31.3 ms) nor an attribute query that the normal plan answers from a btree
  in 6 ms (`tier = 2 AND region_id < 4`: the planner keeps the bitmap scan).

`test/sql/agg_lookup_load.sql` compares the rows with and without the pushdown and with the normal plan (indexed table, table without index, `NULL` and
absent parameter values, a group with a `NULL` key and a duplicate).

## 5. Joins that bring a value back, and the external-dimension registry (not built)

A join that returns a column of the joined table (`JOIN entities e ON e.id = events.dim_a_id ... GROUP BY e.region_id`) cannot be a byte table of
presence: every fact row needs the value of its entity. With section 4 its lookup join runs in 26 to 31 ms against 61 to 65 ms for the normal plan
(2.1 to 2.3 times, last two rows above); without it 47 to 50 ms. The remaining cost is the generic lookup join: a cache entry per distinct entity in
the scan (about 200,000) and a hash lookup of its rows.

A registry (entity id to attribute codes, loaded once per database or backend, usable as a filter table and as a group-by code) would cut that to 8
to 10 ms (research figure: 43 to 45 ms before), but it is a copy while each scan reads the rows visible to its own snapshot. A safe default would need
a modification counter or xmin horizon per registered table, `sift.refresh_dimension()`, an opt-in maximum age, a size-capped per-backend or
shared-memory copy and a load in every backend that uses it.

It is not built: sets of 1,000 to 5,000 entities already take 5 to 7 ms with no copy to keep fresh, and the value join is 2.1 to 2.3 times faster
than the normal plan without it. The next step with no staleness would be a direct-address table of the codes built from the joined table at the
start of each scan (the byte table of section 3 with one more column), which the lookup table of section 4 makes cheap to fill.

## 6. The attribute copied into the fact table, and clustering by it

The alternative is to put the attribute in the event row: `country_id` becomes a column of the fact table's `sift_col` index and the filter is
`country_id = 7`, an ordinary comparison with zone pruning. `bench/sets/denorm.sql` builds the two copies measured: `events_dn_u` (insertion order)
and `events_dn` (stored in `(country_id, event_date, id)` order, a `CREATE TABLE ... AS SELECT ... ORDER BY`), with `sift_col` indexes over
`(dim_a_id, channel_id, lang_id, signal, event_date, score, deleted_at, country_id, category_id)` (23 MB and 16 MB, against 19 MB without the two
columns).

| query (3M events) | normal plan | semi join to `entities` (section 3) | `country_id = 7`, insertion order | `country_id = 7`, clustered |
|---|---|---|---|---|
| a country (3,300 entities) | 41.4 ms (seq scan) | 9.1 ms | 3.8 ms | **1.9 ms** |
| a category (17,000 entities) | 44.1 ms | 10.9 ms | 6.0 ms | 6.8 ms |

 For a country the column is 2.4 times
faster than the semi join in insertion order and 4.8 times when clustered (106 of 113 units pruned, `Units Pruned`); a category is spread over the
sort key, so clustering by country does not help it (1.8 and 1.6 times). The query already runs in 9 to 11 ms, and the gain is paid on the write path
:

- an entity changes country: updating the 4,844 rows of `entities` of that country writes 7.5 MB of WAL; the copy in the events of those entities
  (66,168 rows) writes 358 MB in the insertion-order table (48 times) and 75 MB in the clustered table (10 times: the rows are on a few pages);
- an insert of 1,000 events writes 6.29 MB without the copy and 6.32 MB with it: the two extra columns are not a measurable cost per inserted row;
- clustering decays: 3% late rows (90,585 events in insertion order, appended to the clustered copy and vacuumed) put the country in 9 units where it
  was in 7 (2 more to read, 106 still pruned), and every later batch adds its own tail until the table is rewritten (`CREATE TABLE ... AS SELECT ...
  ORDER BY` and a swap).

Recipe: keep the attribute in the dimension table and copy it into the fact table only for a filter a section-3 semi join cannot make fast enough,
accepting that a change of the attribute rewrites every event of the entity (10 to 48 times the WAL of changing the dimension row here).

A `sift_table` option that keeps its rows in the order of a sort key is not built: the gain here is at most 2 times over the unclustered column (3.8 to
1.9 ms) and only for a filter on the sort key, it needs a format change (docs/FORMATS.md) and a table rewrite to keep the order, and the decay above
returns with the first late batch. The `ORDER BY` of the recipe gives the same layout on a `sift_col` heap table today.
