# Columnar Analytics (`sift_col`)

A `sift_col` index stores a compressed columnar copy of selected columns beside an ordinary PostgreSQL heap table. Keep your primary keys, foreign keys, MVCC semantics, and existing SQL — PostgreSQL's planner runs aggregations, filtered group-bys, top-N, and JSONB analytics through a `Custom Scan` of the `SiftAgg` family: `SiftAggV6` and its operators for Format Major 2 (section 11), `SiftAgg` for the Format Major 1 indexes of 5.x.

## 1. Designing a `sift_col` Index

List the columns your analytical queries filter, group, join, or aggregate on. A single `sift_col` index holds up to **1,024 leaf columns** (including shredded JSONB leaves).

```sql
CREATE INDEX orders_col ON orders
  USING sift_col (customer_id, country, category, status, amount, created_at, payload);
```

That statement is the whole setup. With no `WITH (...)` clause and no `SET`, the index infers `key_order = 'auto'` (section 2) and `paths = 'auto'` for the `jsonb` column (section 4), keeps `preagg = 'auto'` summaries (section 3), and runs parallel scans, aggregations, and builds (`sift.parallel_scan = on`). Every automatic choice is reported once as a `NOTICE`; set an option explicitly only to override it:

```sql
CREATE INDEX orders_col ON orders
  USING sift_col (customer_id, country, category, status, amount, created_at, payload)
  WITH (
    key_order = 'created_at',
    paths     = 'payload.channel text, payload.discount float8'
  );
```

### Indexes Created by 5.x

A `sift_col` index created by 5.x (Format Major 1) keeps serving queries and taking writes on 6.0 with no change and no downtime. New indexes and every `REINDEX` use Format Major 2; to move an existing index onto it (and onto the automatic `key_order` and `paths` of this page) run `REINDEX INDEX CONCURRENTLY orders_col;`, which keeps the old index in service until the new one is swapped in. See [UPGRADING.md](UPGRADING.md).

### Which Access Method?

| Need | Access method | Statement | Storage |
|---|---|---|---|
| Fast analytics on an existing OLTP table | `sift_col` *(index)* | `CREATE INDEX ... USING sift_col (...)` | Compressed columnar copy beside the heap |
| Native columnar table, no heap copy | `sift_table` *(table)* | `CREATE TABLE ... USING sift_table` | Compressed columns only ([TABLES.md](TABLES.md)) |
| Exact BM25 full-text search | `sift` *(index)* | `CREATE INDEX ... USING sift (body)` | Inverted index ([SEARCH.md](SEARCH.md)) |

A `sift` index and a `sift_col` index on the same heap table cooperate automatically (`SiftHybridScan`), so a text match and an aggregate run in one plan. The same holds on a `sift_table`, where the `sift` index shares the table's own row addresses and the combined plan needs no heap fetch. A `sift_col` index on a `sift_table` is refused: the table already runs `SiftAgg` natively, with every option of this page (`key_order`, `paths`, `preagg`, `connect`, `scope`, `sketches`, `skip_index`) set through `sift.table_set_option` ([TABLES.md](TABLES.md#3-per-table-options-10-options)).

- **Keep B-trees for primary-key and OLTP point lookups.** `sift_col` excels at scans, aggregates, and multi-column filters; B-trees remain faster for single-row `WHERE id = $1` lookups.
- **Omit high-churn, unqueried columns.** Updating a column outside `sift_col` stays a PostgreSQL HOT update with zero index write cost.
- **Use `session_preload_libraries = 'pg_sift'`** per database (`ALTER DATABASE mydb SET session_preload_libraries = 'pg_sift';`) so the planner hook is active on the first query of every connection without requiring `shared_preload_libraries` or a restart.
- **Verify exactness anytime:** Run `SET sift.enable_agg = off;` to compare against PostgreSQL's native plan. Set `SET sift.explain_refusals = on;` or use `EXPLAIN (SIFT)` (PG 18+) to print a `Sift:` line with each skipped path, its reason, and the fix. On any supported version, `SELECT arm, used, slug, reason, action FROM sift.explain_why('<query>');` returns the same answer with a one-line `action` per refusal.
- **Check the fleet:** `SELECT * FROM sift.health();` returns zero rows when nothing needs attention; `sift.indexes` and `sift.status()` show size, parts, and maintenance state (see [OPERATIONS.md](OPERATIONS.md#3-observability-four-surfaces)).

## 2. Physical Sort and Partitioning (`key_order`)

`key_order` sorts rows inside Format Major 2 parts and partitions ranges across the 2-level manifest. It accepts **1 to 8 columns** (`col [ASC|DESC] [NULLS FIRST|LAST]`), `'auto'`, or `'off'`. This native range partitioning replaces the 5.x `key_cut` option, with no separate setting: a key lookup or range reads only the partitions and granules whose key range can match.

**Omitting `key_order` means `'auto'`.** The build picks up to two sort columns: for a `scope` with `cluster`, the primary parent followed by the member and time columns; otherwise the leading low-to-medium-cardinality grouping key (from statistics and the primary and foreign keys, skipping a monotonic surrogate key when a better grouping or time column exists) followed by the first date or timestamp column; otherwise the first date or timestamp column. The choice is stored as `'auto: col1, col2'` (so `REINDEX` and `pg_dump` reproduce it) and reported in a `NOTICE`. `key_order = 'off'` builds an unsorted index.

```sql
CREATE INDEX events_col ON events
  USING sift_col (tenant_id, event_date, kind, score)
  WITH (key_order = 'tenant_id, event_date DESC');
```

- **Time-series dashboards:** Put the timestamp or date column first (`key_order = 'created_at'`). Date-range queries prune entire partitions and 8,192-row granules via zone maps.
- **Tenant-partitioned queries:** Lead with the filter key followed by time (`key_order = 'tenant_id, event_date'`). Single-tenant queries touch only that tenant's partitions and granules.
- **Top-N acceleration:** Queries matching `key_order` (`ORDER BY created_at DESC LIMIT 50`) read granules from the relevant end and stop once the top-N bound is met.

### Composite Key Marks

Every keyed part of Format Major 2 carries an optional `SEC_KEY_MARKS` section: the first and last key of each 8,192-row granule as an order-preserving, `memcmp`-comparable image of the whole `key_order` tuple (1 to 8 columns, `ASC` or `DESC`, `NULLS FIRST` or `LAST`; `int2/4/8`, `bool`, `float4/8`, `date`, `timestamp`, `timestamptz`, `uuid`, `numeric`, and text under a deterministic collation). A predicate set that binds an equality prefix of the key plus an optional trailing range, `BETWEEN`, or short `IN` list (up to 16 points) is answered by binary search of the marks, in memory, with no buffer read per probe: for `key_order = 'country, created_at'`, `WHERE country = 'DE' AND created_at >= '2026-01-01'` reads only the granules of that range, not every granule of `country = 'DE'`. A mark is a bounded prefix of the key, so pruning on it never skips a granule that could match. About 80 bytes per granule (well under 0.01 B/row). Parts written before 6.0 or without marks are pruned with zone maps as before; there is nothing to configure.

### Ordered Scans and Keyset Pagination

When the `key_order` of a Format Major 2 index is the order of its columns (`key_order = 'k'`, `'a, b DESC'`, with the matching `DESC` and `NULLS FIRST` / `NULLS LAST`), the planner offers the index as an ordered `Index Scan` that returns rows in key order, forward and backward, on one column or several, with no `Sort` above it. The scan merges the parts by their key marks, so rows in the L0 slab, a transaction's own changes, deletes, and updates come out in the same order the heap would give. Merge joins that mark and restore the position of the scan, and cursors that scroll over it, work. A cursor that scrolls over a Format Major 1 index still works.

A keyset page, `WHERE (a, b) > ($1, $2) ORDER BY a, b LIMIT 50`, reaches the scan as **one range of the key**, so it reads from the key on instead of from the start of the index. PostgreSQL offers a row comparison to B-trees only, so `pg_sift` offers it itself:

- the row comparison names up to the first 3 key columns, each of type `int2`, `int4`, `int8`, `date`, `timestamp`, or `timestamptz`, compared with the operator of the column's family and with parameters or stable expressions on the right;
- the operator classes of the typed families carry the two-argument comparison of their B-tree family as `FUNCTION 1`, which the executor needs for a row comparison, on a fresh install and after `ALTER EXTENSION pg_sift UPDATE`; an index made before keeps the class it was made with;
- `SiftAggV6` expands `(a, b) > (x, y)` into `a > x OR (a = x AND b > y)` so that zone maps, summaries, and kernels test each comparison, and a keyset page of an aggregate reads the granules from the key on.

### Adaptive `PREWHERE` and Lazy Column Reads

For a filtered scan, `SiftAgg` ranks the conjuncts of the `WHERE` clause by `(cpu + io) / (1 - pass rate)`: seeded from the per-column `SEC_COL_STATS` section (compressed bytes, distinct estimate, dominant encoding) before the first granule, then refined by an exponentially weighted moving average of the observed pass rate. `SiftLazyGranule` then reads and tests only the chunks of the cheapest, most selective filter column first; a conjunct the zone maps already decided is not read at all, payload columns are read only for a granule with a surviving row, and a sparse selection decodes only its own rows. The conjuncts run on the stored form (dictionary codes, runs, frame-of-reference, delta, bit-packed booleans, ALP, FSST) before any decoding, and error-raising conjuncts keep their SQL evaluation order. A scan whose predicates are on key columns alone reads the next granules ahead through the server read stream (`io_uring` on PostgreSQL 18), so a cold scan keeps the device queue full instead of stalling on serial 8 KB reads.

### Substring Search Pushdown (`LIKE '%...%'`)

With `skip_index = 'col:trigram'`, `LIKE '%needle%'` and `ILIKE '%needle%'` first probe the trigram skip index to drop chunks that cannot match, then scan the survivors with a vectorized substring search (AVX2 or NEON, scalar gives the same answers; 9 to 25 GB/s per core on dictionary text). `ILIKE` is decided on the stored bytes under the `C` collation only; every other collation stays with PostgreSQL's own evaluation.

## 3. Pre-Aggregates (`preagg`)

Besides the automatic per-granule pre-aggregates, a `sift_col` index or `sift_table` can carry named secondary projections (`SIFT_PROJ_AGG` rollups and `SIFT_PROJ_SORT` orders), created with `sift.create_projection` ([TABLES.md](TABLES.md#8-secondary-projections-siftcreate_projection)).

Format Major 2 stores granule-level summaries (`SEC_PREAGG` v2 cubes, with `SEC_PREAGG_SUPER`, `SEC_PREAGG_ROLLUP`, and `SEC_PREAGG_EXCEPT` beside them) in every part of level 1 and up, and answers `count`, `sum`, `avg`, `min`, and `max` from them without decoding the rows. A cube cell holds, for one value of an integer dimension, the count, the sum, the sum of squares, the least, and the greatest of an integer measure; a granule whose dimension exceeds the cardinality or size budget is marked skipped and is scanned normally. The number of dimensions, rollups, and measures an explicit definition may name is in [LIMITS.md](LIMITS.md); a definition past it is rejected at `CREATE` or `ALTER INDEX`. `sift.parts()` shows the bytes of the pre-aggregates of each part and `sift.index_size_breakdown()` their share of the index. A part also keeps, for each map column declared as `col.* type` in `paths`, a cube of its members (the count and the sum, least and greatest of the values of each key in each granule), which answers `GROUP BY key` over `jsonb_each` and `jsonb_each_text` of the map without decoding the granules that are clean.

```sql
-- Default: automatic low-cardinality cube and calendar summaries
CREATE INDEX orders_col ON orders
  USING sift_col (country, category, amount, created_at)
  WITH (preagg = 'auto');

-- Explicit cube, rollup, and calendar definitions
CREATE INDEX orders_custom ON orders
  USING sift_col (country, category, amount, created_at)
  WITH (preagg = 'cube(country, category); rollup(country, category); calendar(created_at)');
```

| `preagg` value | Behavior |
|---|---|
| `'auto'` (default) | Builds compact per-granule summaries when cardinality fits the size budget |
| `'off'` | Disables pre-aggregates to minimize build time and disk footprint |
| `'cube(...); rollup(...); calendar(...)'` | Declares explicit cube dimensions, multi-column rollups, and date buckets |

A granule with unvacuumed deletes still uses its pre-aggregates through **additive delta subtraction**: when at most 5% of its rows and at most 64 rows (128 when the only rows to take out are those one `col IS NULL` predicate excludes) are dead, those rows are read from the Copy-on-Write deletion vector and subtracted from the cells they fall in, which costs the dead rows and not the granule. A dead row that is the least or the greatest of its cell cannot be subtracted from the extremes, and a granule with more dead rows than the bound is scanned with its deletion vector.

The same option is read by `sift.table_set_option(tbl, 'preagg', ...)` on a Format 2.0 `sift_table`, whose parts keep the cubes the index would keep; a column that is not in the table, or a `cube(` that is not closed, is refused when the option is set. `cube(dim[: m1, m2]; dim2 ...)` names the dimensions (up to 4) and the measures each cube holds (up to 8); the clauses can be joined by `,` or `;`. `rollup(...)` and `calendar(...)` are accepted and keep nothing in a part of Format Major 2. A table text column is lossy in the part, so a table cube serves integer keys.

The workers of a parallel aggregate over a `sift_col` index answer granules from the cubes as the serial scan does. A table part answers once it is sealed (`VACUUM (FREEZE)` or maintenance), under a snapshot of a transaction that is not serializable. A granule with at most 64 dead rows (and under 5% of its rows) from deletes, updates, pending deletes or staged deletes is corrected from its cube; one with more, one patched by a column delta, a float sum over dead rows and a `count(DISTINCT)` over dead rows are scanned in the same plan. A parallel aggregate over a `sift_table` is not planned.

## 4. Shredding `JSONB` Columns (`paths`)

Declare typed paths inside `JSONB` columns so `SiftAgg` evaluates `payload->>'channel'`, `(payload->>'discount')::float8`, `@>`, `?`, and `jsonb_array_elements()` directly on compressed columns.

```sql
-- Inspect a sample of documents to generate a paths specification:
SELECT sift.suggest_paths('orders', 'payload', sample => 2000);   -- 2,000 documents is the default; up to 100,000

-- Zero-config: with a jsonb column and no paths option, paths = 'auto' is inferred
-- (persisted as 'auto: ...' for REINDEX and pg_dump):
CREATE INDEX orders_json_col ON orders USING sift_col (created_at, payload);
```

`paths = 'auto'` samples 2,000 rows during the build (16 evenly spaced stretches of the heap), leaves out a key found in fewer than 1% of the documents, shreds the rest into typed leaves (up to 256, plus key-presence and the variant exception stream) and reports `sift_col index "orders_json_col": auto-shredded N jsonb paths (override with WITH (paths = '...') or 'off').` On an empty table it is accepted with no leaves and inferred at the first compaction. Pass `WITH (paths = 'off')` to keep a `jsonb` column unshredded, or an explicit spec to choose the leaves yourself.

### `paths` Grammar

```text
spec  := item { ',' item }
item  := path ( type | '{' item { ',' item } '}' )
path  := step { ('.' | '->') step }
step  := key | key '[]' { '[]' } | '*'
type  := bool | int2 | int4 | int8 | float4 | float8 | text[(N)] | astext[(N)]
```

- **Nested arrays and objects:** Use `items[]{sku text, qty int4, price float8}` to shred correlated arrays of objects; `tags[] text` shreds scalar arrays; `metrics.* float4` shreds dynamic object maps.
- **Limits:** Up to 1,024 shredded leaves, 256 repeated groups, and 16 path steps per index ([LIMITS.md](LIMITS.md)).
- **Zero-heap-I/O exceptions:** If a document holds a mismatched type or out-of-range value on a declared path (for example, a string inside an `int4` path), Format Major 2 stores it in an inline variant exception stream (`SEC_VARIANT_EXC`). Queries handle these values without fetching heap pages.

## 5. Joins, Set Filters, and `connect`

### Star-Schema Broadcast Lookup Joins

`SiftAgg` pushes inner, left, semi, and anti joins against dimension tables directly into the columnar scan when the dimension fits the lookup budget (`work_mem / 2`). Dimension lookup tables are shared across parallel workers and cached inside `sift.shared_cache_kb`.

```sql
SELECT p.brand, count(*), sum(o.amount)
FROM orders o
JOIN products p ON p.id = o.product_id
WHERE p.active AND o.created_at >= '2026-01-01'
GROUP BY p.brand;
```

### Large `IN` and `= ANY($1)` Set Filters

Parameterized array filters (`WHERE customer_id = ANY($1)`) and `IN (...)` lists execute as a single vectorized set test per granule with zone-map pruning, rather than repeated probe loops. Memory is strictly bounded by `work_mem`.

### Connected Parent-Child Indexes (`connect`)

When a child table frequently joins a parent table that also has a `sift_col` index, declare `connect` (up to 8 connections per index) so the child scan can resolve parent columns directly from the parent's `sift_col` leaves without touching the parent heap:

```sql
CREATE INDEX customers_col ON customers USING sift_col (id, tier, country);

CREATE INDEX orders_col ON orders
  USING sift_col (customer_id, amount, created_at)
  WITH (connect = 'customer_id -> customers(id)');
```

A connection is made in the format of the indexes it joins: in a `6.0` database the parent and the child are both Format Major 2, and `connect` composes with `UNIQUE` and `INCLUDE`. The parent index keeps a key journal (a connection gives it the key it joins by when its `track_changes` has not chosen one); `sift.col_connection_state` shows its lineage and records, and `sift.check` verifies it. A Format 2.0 aggregate (`SiftAggV6`) keeps the dimension of a star join that is a plain scan of a parent table for the next statement of the backend, in the shared cache budget (`sift.backend_cache_kb`), and reads it again as soon as the journal does not vouch for it (a write of the parent, committed or not, DDL, another user, row level security, a filter that is not immutable); EXPLAIN ANALYZE shows `Dimension Cache: N hit, M built`. `ALTER INDEX .. SET (connect = ..)` and `RESET (connect)` change the connection of an index that holds data. A parent and a child of Format Major 1 (built before 6.0) stay together in Format Major 1, and a connection never joins two formats: `REINDEX` the parent, then the child, to move both.

Inspect active connections and staleness via `SELECT * FROM sift.col_connections;`. Between `sift_table`s, real foreign keys are connected automatically from `pg_constraint`; declare `connect` through `sift.table_set_option` only for a soft foreign key that has no constraint.

## 6. Bitmap and Index-Only Scans (`sift.col_*_ops`)

A `sift_col` index also serves row-returning `SELECT`, `UPDATE`, and `DELETE` filters as a PostgreSQL `Bitmap Index Scan`, `Index Scan`, or `Index Only Scan` when `sift.enable_col_scan = on` (default). On Format Major 2 an index-only scan returns any column the index stores losslessly, fixed-width or not: `text`, `varchar`, `uuid`, `numeric`, `bytea`, and `interval` included; a Format Major 1 index keeps to fixed-width columns.

```sql
CREATE INDEX items_col ON items
  USING sift_col (id sift.col_int4_ops, kind sift.col_text_ops, created_at sift.col_timestamptz_ops);

-- Uses Bitmap Heap Scan with no leading-column requirement:
SELECT * FROM items
WHERE kind = 'cat_a' AND created_at >= '2026-03-01' AND created_at < '2026-03-02';
```

| Operator Class | Data Types | Supported Operators |
|---|---|---|
| `sift.col_int2_ops`, `col_int4_ops`, `col_int8_ops` | `smallint`, `integer`, `bigint` | `<`, `<=`, `=`, `>=`, `>`, `= ANY`, `IS [NOT] NULL` |
| `sift.col_date_ops`, `col_timestamp_ops`, `col_timestamptz_ops` | `date`, `timestamp`, `timestamptz` | `<`, `<=`, `=`, `>=`, `>`, `= ANY`, `IS [NOT] NULL` |
| `sift.col_bool_ops`, `col_numeric_ops`, `col_float4_ops`, `col_float8_ops` | `bool`, `numeric` (any precision), `real`, `float8` | `<`, `<=`, `=`, `>=`, `>`, `= ANY`, `IS [NOT] NULL` |
| `sift.col_text_ops` | `text`, `varchar` | `=`, `LIKE 'prefix%'`, `^@`, `= ANY`, `IS [NOT] NULL` |
| `sift.col_bpchar_ops`, `col_uuid_ops` | `char(n)`, `uuid` | `=`, `= ANY`, `IS [NOT] NULL` |

The classes of the integers, floats, `numeric`, `bool`, `date`, `timestamp`, `timestamptz`, `text` (and `varchar`), `char(n)` and `bpchar`, and `uuid` are the default of their types, so `CREATE INDEX ... USING sift_col (id)` binds them and `INSERT ... ON CONFLICT (id)` finds the equality of a `UNIQUE` index with no class written. The leaf decides the keys it holds exactly (a `numeric(p, s)` with `p` up to 18, the bytes of a text under a deterministic collation, a `char(n)`); the heap decides the rest (a wider or unconstrained `numeric`, `NaN` and the infinities, a `char` without a length, a `text` under a nondeterministic collation). An index made before keeps the class it was made with. The `sift.col_ops` class supports both `SiftAgg` and row-returning bitmap filters on any column subset, and is the default of every other type.

On Format Major 2, the classes of integers, floats, booleans, text, uuid, the temporal types, and numeric also answer `col <> value` from the rows whose value is not `NULL` and differs from the constant, which is what `SiftAgg` folds without a second look. The opt-in `sift.col_array_ops` class (`CREATE INDEX ON t USING sift_col (id, tags sift.col_array_ops)`) lets the planner offer the index for `tags && ARRAY['a', 'b']`: the index keeps the rows whose array is not `NULL` and the executor tests the elements. Range and network classes keep their containment operators and have no `<>`.

## 7. Range and Network Containment

`sift_col` stores dedicated integer lanes for range types (`int4range`, `int8range`, `daterange`, `tsrange`, `tstzrange`) and network types (`inet`, `cidr`) so containment predicates run as vectorized integer comparisons with zone-map pruning.

- **Range containment (`@>`, `&&`):** Ordering by the range column (`key_order = 'valid_range'`) prunes non-overlapping granules and matches or beats GiST on wide overlap queries at a fraction of the index size.
- **Network containment (`<<=`, `>>=`, `&&`):** Broad subnet scans (`ip <<= '20.7.0.0/16'`) run fast in `sift_col`. For point-in-CIDR lookups over random prefixes (`ip >>= '20.7.1.5/32'`), keep a PostgreSQL GiST index.

## 8. Approximate Distinct Counts (`sift.approx_count_distinct`)

Standard `count(DISTINCT x)` **is exact in every plan** — `pg_sift` never substitutes an estimate unless you explicitly call `sift.approx_count_distinct(x)`.

```sql
CREATE INDEX events_col ON events
  USING sift_col (country, user_id, created_at)
  WITH (sketches = 'hll(user_id)');

SELECT country, sift.approx_count_distinct(user_id)
FROM events
GROUP BY country;
```

- **Accuracy:** Uses HyperLogLog++ sketches with a standard error of **0.81%** (16,384 registers).
- **NULL semantics:** `NULL` values are **not counted**, matching `count(DISTINCT x)`.
- **Composability:** Sketches are **mergeable** across granules, partitions, and parallel workers, and `sift.approx_count_distinct()` also runs as a standard PostgreSQL aggregate when falling back to the heap.
- **Exactness invariant:** Enabling `sketches` only stores sketches for `sift.approx_count_distinct()` (and `kll(col)` quantile sketches); `count(DISTINCT user_id)` on the same column **is exact in every plan**.
- **Skipping indexes:** Dictionary, small-integer bitset, exact-set, and medium-cardinality Bloom skipping are built automatically with under 1% storage overhead. Only high-cardinality Bloom or trigram skipping is opt-in: `WITH (skip_index = 'order_ref:bloom, memo:trigram')`.

### Approximate Quantiles (`sift.approx_percentile`, `sift.approx_quantiles`)

`kll` sketches answer percentiles from the stored granules instead of sorting the rows. They are opt-in, and standard `percentile_cont()` and `percentile_disc()` stay exact:

```sql
CREATE INDEX orders_col ON orders
  USING sift_col (country, amount, created_at)
  WITH (sketches = 'kll(amount)');

SELECT country, sift.approx_percentile(amount, 0.95) AS p95,
       sift.approx_quantiles(amount, ARRAY[0.5, 0.9, 0.99]) AS q
FROM orders
GROUP BY country;
```

- **Accuracy:** KLL sketches with `k = 200` bound the rank error to about 1.3%. Sketches merge across granules, partitions, and parallel workers (both aggregates are `PARALLEL SAFE`).
- **Types:** `float8`, `int8`, and `numeric` inputs; `approx_percentile` returns `float8` and `approx_quantiles` returns `float8[]`.
- **Dirty rows:** A sketch tolerates up to 1% of rows deleted or updated since it was built; beyond that the aggregate streams the live rows exactly, so a dead row never skews the estimate.
- **`sift_table`:** The same `sketches = 'kll(col)'` setting applies to `sift.table_set_option`.

## 9. Change Tracking (`track_changes`)

A `sift_col` index sits beside a heap that PostgreSQL updates in place, so it must learn which indexed rows were deleted or moved. `track_changes` does that, and in Format Major 2 it is `'auto'` by default and works on **any** heap table with no setting and no primary key:

```sql
CREATE INDEX events_col ON events USING sift_col (entity_id, event_date, amount);   -- tracking is already on
```

The index records each changed row by its heap address (`ctid`) from one `AFTER UPDATE OR DELETE FOR EACH ROW` trigger on the table, so every table shape works:

| Table | 5.x `track_changes` | 6.0 `track_changes = 'auto'` |
|---|---|---|
| No primary key (event log, staging table) | Not tracked; scans vetted rows in the heap | Tracked |
| Composite, `uuid`, or `text` primary key | Refused, or a key column added to the index | Tracked; no extra bytes in the index |
| Index that omits the primary key column | Not tracked | Tracked |
| Partitioned table | Triggers on every ancestor, re-made on `ATTACH` / `DETACH` | One trigger per leaf partition; nothing to re-make |
| Update of a column the index does not hold | Statement trigger copies the changed rows | HOT updates exit at once: no trigger work, no index write |

- **Nothing to baseline:** `CREATE INDEX` and `REINDEX` index live rows only, with no extra heap scan, and the trigger is installed in the same transaction so it hears every later change.
- **No reloption rewriting:** `pg_class.reloptions` is never changed; `track_changes` stays `auto` or `off`. `on`, `true`, and `ctid` are accepted as `auto`.
- **Opt out with `WITH (track_changes = 'off')`** for an immutable or append-only table: no trigger is installed, and scans check visibility in batches through the heap's visibility map instead.
- **From 5.x scripts:** `track_changes = '<column>'` is rejected with a hint that Format Major 2 tracks by heap address and needs no column. Existing Format Major 1 indexes keep their stored setting until `REINDEX INDEX CONCURRENTLY` moves them to Format Major 2 ([UPGRADING.md](UPGRADING.md)).
- **A `sift_table` needs none of this:** its changes are native MVCC rows, so it has no `track_changes` option.

## 10. ClickHouse-Class SQL in `SiftAgg`

These are answered exactly as PostgreSQL would (`SET sift.enable_agg = off` to compare) and add no setting:

| Feature | Behavior |
|---|---|
| Wide and multi-column `COUNT(DISTINCT)` | `text`, `varchar`, `uuid`, `numeric`, and multi-column keys (`COUNT(DISTINCT (a, b))`) are counted with an arena-backed exact set (no hash collisions) that spills by hash partition under `work_mem` |
| `sift.arg_max(val, key)` / `sift.arg_min(val, key)` | `anyelement` value chosen by an `anycompatible` ordering key (`arg_max(name, created_at)`); parallel safe, with a stock transition and combine function so the heap plan gives the same rows. A `NULL` key is ignored, a `NULL` value on the winning row is the result, and the first of tying rows wins. Pushed into the vectorized loop for `int4`/`int8` values and `int4`/`int8`/`float8` keys; `DISTINCT ON` without a full sort |
| Multi-`FILTER` pivots | Up to 64 `FILTER (WHERE ...)` clauses on `count`, `sum`, `avg`, `min`, `max`, and `count(DISTINCT)` evaluate in one scan with one bitmap per distinct clause |
| Grace hash join | An inner, left, semi, or anti equi-join whose build side exceeds `work_mem / 2` is radix-partitioned into 16 to 256 spillable partitions instead of returning to PostgreSQL's join |
| `ASOF` join | The `LEFT/INNER JOIN LATERAL (... WHERE d.k = f.k AND d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1) x ON true` idiom runs as a sorted merge when the dimension is ordered by `(k, ts)` (or has a matching `SIFT_PROJ_SORT` projection), and by binary search otherwise |
| `date_bin()` and windows | `date_bin(interval, ts, origin)` compiles into the vector program with the server's errors in the server's order; `SiftWindowExec` adds `FILTER`, exact `NUMERIC` `AVG`, `LAG`/`LEAD` with defaults, and `ROWS BETWEEN N PRECEDING AND M FOLLOWING` frames |

## 11. The `SiftAggV6` Engine

Every Format Major 2 relation, `sift_col` index and `sift_table` alike, is read by one vectorized engine, `SiftAggV6`. The scan accepts rows by the pruning, the filters on the compressed chunks, the deletion vectors, and the pending deletes of the Format Major 2 store; the rows are vetted against the heap in batches when the relation has one, decoded to column vectors, and folded into the groups by SIMD kernels; one tuple leaves for each group. A `sift_table` runs the same engine with no heap to vet against, and the planner prices each path with one cost model. What the engine cannot prove equal to PostgreSQL's own plan it refuses with a stable slug (`sift.explain_why()`, `sift.refusal_slugs()`) and PostgreSQL plans as usual; `SET sift.enable_agg = off` compares the two.

| `EXPLAIN` node | What it runs |
|---|---|
| `SiftAggV6` | Filtered aggregates and group-bys of one table, with fused pipelines and star-join lookups of small dimensions |
| `SiftAggV6TopN` | `ORDER BY ... LIMIT` (up to 8 sort columns): a tournament over the units of the format, then the winners' columns in order |
| `SiftAggV6Join` | A partitioned (Grace) hash join of inner, left, semi, and anti equality joins whose build side exceeds half of `work_mem`, spilling by partition |
| `SiftAggV6Asof` | The `LATERAL (... d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1)` idiom as one merge of two sorted inputs |
| `SiftAggV6Window` and `SiftAggV6Stream` | Window functions and CTE streams over the batches |
| `SiftAggV6Scope` | Scope aggregates read from the summary ([SCOPES.md](SCOPES.md)) |
| `SiftProjSortScan`, `SiftHybridScan`, `SiftHybridAgg` | Sort projections ([TABLES.md](TABLES.md#8-secondary-projections-siftcreate_projection)) and combined BM25 plus column plans ([SEARCH.md](SEARCH.md)) |

A Format Major 1 index keeps the older `SiftAgg` node with the same exactness rule. `EXPLAIN (ANALYZE, SIFT)` (PG 18+) prints the counters of whichever node ran ([ARCHITECTURE.md](ARCHITECTURE.md#3-2048-row-simd-vectorized-execution-siftagg)). The four `sift.gpu*` settings select the optional Vulkan path of `SiftAggV6` ([GPU.md](GPU.md)).

## 12. Parallel Execution

`sift.parallel_scan` is `on` by default: the planner may run a `SiftAggV6` scan under a `Gather`, with the granules of the parts handed out in morsels. The planner picks the strategy from the estimated number of groups; there is no setting for it:

- **Few groups (two-stage):** every participant aggregates its morsels into a table of its own; after one barrier the first to finish merges the others' groups and emits the result.
- **Many groups (radix):** a participant aggregates into a small cache-resident table and, when it fills, scatters the groups into hash partitions; after the barrier each participant claims whole partitions, merges them, and emits their groups itself, so no process merges all groups and none are emitted twice.
- **`count(DISTINCT)` with no keys:** the values are scattered by hash, each partition counts its values exactly, and one participant emits the row.

Partial states that outgrow their share of memory go to spill files, so nothing fails. A two-stage run that finds many groups switches to the radix strategy. Parallel `float8` sums add in any order and may differ in the last digits from the row-order sum of the heap plan; `SET sift.agg_exact_float = on` adds the values in the order of the rows in one process, so a sum repeats to the last bit. `CREATE INDEX` of a `sift_col` index (and so `REINDEX`) builds with parallel workers too, up to `max_parallel_maintenance_workers` and at most 15 besides the leader. `SET sift.parallel_scan = off` restores single-process scan plans.
