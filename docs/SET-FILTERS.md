# Set filters: array parameters, lookup tables and dimension registries (3.2)

A set filter keeps the rows of a fact table whose key is in a set: the ids of a group, the ids of the entities that have an attribute. This document
is how the aggregate pushdown (`Custom Scan (SiftAgg)`, [docs/AGG.md](AGG.md)) answers them, what each way costs, and the settings that choose between them.

## 1. The dataset and the method

`bench/sets/gen.sql` and `build.sql` make, deterministically (`setseed`), a fact table `events` (3,000,000 rows, 173 MB heap, `sift_col` index 19 MB, 87 units,
a `deleted_at` on 1.5% of the rows, a date from 2021-01 to 2026-10 with a backfill tail, a zipf distribution of events over 200,000 entities: p50 5, p90 20, p99 135,
at most 84,637 events per entity, 30 channels), a dimension table `entities` (200,000 rows: `city_id`, `country_id` (60), `category_id` (12), `tier`, `chain_id`,
`region_id`) and a membership table `group_members` (60 groups of 100 entities, 20 of 1,000, 8 of 10,000, 2 of 100,000; 306,000 rows, soft-deleted by `deleted_at`).
The query of a set filter is `count(*)` and `avg(score)` per channel of the events of the last 12 months that are not deleted, restricted to the set.

Runs: PostgreSQL 18.6, `bench/sets/run.sh` (a `systemd-run --user --scope` with `MemoryMax=12G`, `CPUQuota=300%`, the host lock `/tmp/sift-perf.lock`),
`shared_buffers = 2GB`, `jit = off`, `max_parallel_workers_per_gather = 2`, one warm-up and 5 timed runs per cell interleaved in random order, the median is
reported, client side (`\timing`: planning and execution).
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
| 100 | 2.3 ms | 2.6 ms | 2.7 ms (bitmap) | 4.8 ms | 2.6 ms (bitmap) |
| 1,000 | 8.5 ms | 5.8 ms | 8.8 ms (bitmap) | 4.6 ms | 5.5 ms |
| 10,000 | 31.6 ms | 16.9 ms | 32.4 ms (bitmap) | 5.2 ms | 15.5 ms |
| 100,000 | 95.9 ms | 128.2 ms | 98.5 ms (bitmap) | 10.2 ms | 116.4 ms |

columns are planned with the values.) The 3.2 default is 6 and 10 times faster than the generic plan without the setting at 10,000 and 100,000 members (and 31.6 / 95.9 ms against the normal plan), and the
generic plan with the 3.1 workaround (`sift.agg_param_set_size = 0, enable_indexscan = off, enable_bitmapscan = off`) gives 4.8 / 4.6 / 5.0 / 10.1 ms: the same plan. The crossover is between 100 and 1,000 members: at 100 the
normal plan is faster by 2.5 ms (2.3 against 4.8 ms), and such a set is better sent with a custom plan (`plan_cache_mode = force_custom_plan`, the
default `auto` for the first 5 executions) or `SET sift.agg_param_set_size = 10`.

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

`NULL` elements, an empty array and duplicates keep the meaning they have in SQL (`x = ANY('{}')` is false, `x <> ALL('{}')` is true for every row including a NULL `x`,
a NULL element makes `<> ALL` never true); an element outside the range of the column matches nothing. `test/sql/agg_set_array.sql` compares them with the normal plan.
A set whose values span more than 4 million (2^22) is searched in a sorted array instead of the byte table (slower, same answers).

## 3. The set as a table: `IN (SELECT ..)`, `EXISTS`, `NOT EXISTS` (`sift.agg_semi_lut`)

A set kept in a table (a membership table, the entities that have an attribute) is a semi join of the fact table, and `NOT EXISTS` an anti join.
`Custom Scan (SiftAgg)` loads the table when the scan starts, and when the join compares one integer or date leaf with one key column and gives no column
back, it makes from the keys a byte table over their range. Every tuple then costs one byte read, where the generic lookup join makes a cache entry per
distinct value of the leaf (a hash lookup of the rows of the table each). The table's own conditions (`group_id = 3 AND deleted_at IS NULL`, `country_id = 7`)
are tested once per row of the table, not once per probe.

| query (3M events, 12 months, `count`, `avg` per channel) | normal plan | SiftAgg, generic lookup join | SiftAgg, byte table |
|---|---|---|---|
| `IN`, group of 1,000 members | 55.6 ms (hash join) | 59.0 ms | 22.2 ms |
| `IN`, group of 100,000 members | 78.8 ms | 68.8 ms | 27.9 ms |
| `EXISTS`, group of 1,000 | 56.5 ms | 59.3 ms | 22.5 ms |
| `NOT EXISTS`, group of 1,000 | 73.2 ms | 66.9 ms | 26.7 ms |
| `IN`, attribute with 3,300 entities (a country) | 60.7 ms | 39.9 ms | 15.7 ms |
| `NOT EXISTS`, the same | 76.6 ms | 52.0 ms | 19.3 ms |
| `IN`, attribute with 17,000 entities (a category) | 62.4 ms | 39.7 ms | 16.6 ms |
| `IN`, group of 100 members | 2.7 ms (index scan) | 55.1 ms | 22.8 ms |

every answer equal to the normal plan's.) The byte table is 2.0 to 3.3 times faster than the generic lookup join on all 17 queries, and 2.5 to 4.0 times faster than the normal plan on the 14 where
`SiftAgg` is the better plan. Three are not: a group of 100 members (2.7 ms with an index scan, against 22.8 ms: the table is loaded whole) and an attribute that selects few entities
(`tier = 2 AND region_id < 4`, `IN` and `EXISTS`: 6 ms with a bitmap scan against 18 ms). The shipped planner chooses the faster plan in 16 of the 17 queries (`default` rows). It misses one:
`NOT EXISTS` of a group of 1,000 keeps 99% of the events, the planner prices the hash anti join below `SiftAgg` and takes it (72.7 ms against 26.7 ms with `SET enable_hashjoin = off`);
the same query on a group of 100,000 takes `SiftAgg`.

What the plan costs: the planner prices the semi and anti joins of a `SiftAgg` path by the rows PostgreSQL expects the join to keep (before, every row that passed
the leaf filters was priced as a tuple of the groups), so a selective set is not charged for tuples the join drops. `EXPLAIN ANALYZE` shows `Semi Lookup Tables`
and `Semi Lookup Table Size`.

Limits and fallbacks, each answered by the generic lookup join with the same rows (`test/sql/agg_semi_lut.sql` compares both and the normal plan; the `j_semi_lut_*`
fixtures of `scripts/coldiff.sh` do it in every heap state):

- the keys span more than `sift.agg_semi_lut_size` (default 4 MB, one byte per value of the range; a key outside the range of the leaf's type is ignored, it matches no row);
- the leaf is not an integer, date or `int2` column in the index (text keys, `int8` leaves, array elements), a join has two or more keys, or the table's own condition reads a column of the fact row;
- `NULL` keys: a `NULL` in the table matches nothing, a `NULL` leaf matches nothing (dropped by `IN` and `EXISTS`, kept by `NOT EXISTS`), as in SQL;
- `NOT IN (SELECT ..)` is not a join: PostgreSQL plans it as a hashed subplan (a `NULL` in the table makes the whole answer empty), and the query keeps the normal plan. Write `NOT EXISTS`;
- each worker of a parallel `SiftAgg` builds its own table, and a rescan keeps it (the groups are replayed). The table is built from the rows visible to the scan's snapshot.

Parallel plans: the semi joins that PostgreSQL pulls up from a sub-select made the whole query count as parallel restricted for `SiftAgg`, and it planned no workers. It now uses the same
per-relation flags PostgreSQL's own grouping does, so these queries get `Workers Planned` like the others (`agg_semi_lut` runs them with workers).
