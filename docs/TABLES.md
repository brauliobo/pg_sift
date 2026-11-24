# Native Columnar Tables (`sift_table`)

`sift_table` is a PostgreSQL Table Access Method (TAM) that stores table data directly in Format Major 2 compressed columns — with no separate heap copy. In `pg_sift` 6.0, `sift_table` and `sift_col` are two thin skins over **one Format Major 2 storage engine**: every compression codec, skip index, scope, pre-aggregate, and `SiftAgg` execution path of a `sift_col` index works identically on a `sift_table`, with no adapter layer and 10 documented options (section 3).

## 1. Choosing Between `sift_table` and `sift_col`

`pg_sift` has three access methods (`sift_col` index, `sift_table` table, `sift` BM25 index): see the table in [ANALYTICS.md](ANALYTICS.md#which-access-method). Pick `sift_table` when the table itself is the analytical copy; pick a `sift_col` index when you keep an OLTP heap.

| Criterion | `sift_table` (`USING sift_table`) | `sift_col` (`USING sift_col` on heap) |
|---|---|---|
| Storage footprint | Single compressed columnar copy | Heap table + compressed columnar index |
| Primary-key updates / HOT | Full MVCC, no HOT updates | Heap HOT updates on non-indexed columns |
| `SiftAgg` analytics | Full parity (8,192-row granules, pre-aggs, scopes, joins, Top-N, windows) | Full parity (same Format Major 2 engine) |
| Change tracking | Native MVCC; no `track_changes` trigger and no option | `track_changes = 'auto'` row trigger on the heap ([ANALYTICS.md](ANALYTICS.md#9-change-tracking-track_changes)) |
| Hybrid BM25 + columns | `sift` index shares the table's addresses: no heap fetch, no HOT chains | `sift` index on the heap; rows vetted in the heap |
| Rebuild on corruption | Restore from backup / replica | `REINDEX INDEX CONCURRENTLY` from heap |
| Best fit | Append-heavy events, telemetry, logs, analytical tables | Live OLTP tables with heavy single-row updates |

A `sift_col` index cannot be created on a `sift_table`: the table already stores Format Major 2 columns and runs `SiftAgg` natively. Configure its sort order, scopes, and the other options with `sift.table_set_option(...)` (section 3). B-tree, `UNIQUE`, `EXCLUDE`, GIN, GiST, and `sift` (BM25) indexes attach to a `sift_table` normally. A `sift_table` keeps a visibility map fork (`VISIBILITYMAP_FORKNUM`) that PostgreSQL's `Index Only Scan` reads (section 6); aggregates over its columns run through `SiftAgg` without row materialization.

### What Works on a `sift_table`

Everything below runs on a `sift_table` exactly as on a `sift_col` index, with the same grammar and the same defaults:

| Area | Capability on `sift_table` |
|---|---|
| Storage | Typed `jsonb` shredding, part-shared string dictionaries (FSST, ZSTD), automatic float codecs, four-tier compression, secondary clustering inside equal sort keys, up to 1,024 columns |
| Skipping | Time-bound pruning on up to two `date` / `timestamp` columns, zone maps, automatic dictionary, bitset, exact-set, and Bloom skip indexes |
| Analytics | Direct-on-compressed filtering, pre-aggregates, `GROUPING SETS` / `ROLLUP` / `CUBE`, approximate sketches, windows, `DISTINCT ON`, Top-N, star and snowflake joins with runtime join filters, parallel scans |
| Scopes | Entity-Once `scope` over integer pairs, primary-parent clustering, Top-K rings ([SCOPES.md](SCOPES.md)) |
| Search | `SiftHybridScan`: a `sift` BM25 index and a `sift_table` answer one query together, in either direction, with no heap fetch |
| Projections | `SIFT_PROJ_SORT` and `SIFT_PROJ_AGG` secondary projections in the same relation, chosen by `SiftAgg` (section 8) |
| Tiering and TTL | Cold parts in `file://`, `s3://`, or `r2://` object storage and `O(1)` part-drop TTL (section 9) |
| Table AM | `ALTER TABLE ADD / DROP / ALTER TYPE / RENAME COLUMN` (no rewrite for `ADD`, `DROP`, `RENAME`), `INSERT ... ON CONFLICT`, row locks, `SERIALIZABLE`, `TABLESAMPLE`, `TRUNCATE`, 2PC, logical replication, values up to 1 GB stored out of line |

## 2. Creating and Loading a `sift_table`

```sql
CREATE TABLE events_col (
  id         bigint,
  tenant_id  int,
  event_date date,
  kind       text,
  amount     numeric(12,2),
  payload    jsonb
) USING sift_table;

COPY events_col FROM '/data/events.csv' WITH (FORMAT csv);

SELECT kind, count(*), sum(amount)
FROM events_col
WHERE tenant_id = 42 AND event_date >= '2026-01-01'
GROUP BY kind;
```

A `sift_table` needs no option calls to start; most tables never set one. Every option has an automatic default:

| Option | Automatic behavior |
|---|---|
| `key_order` | From the primary key (up to 8 columns), or from the leading foreign key or low-cardinality grouping key plus the first time column once rows arrive; rows with equal keys are also clustered for compression |
| `paths` | `auto` for `jsonb` columns: the keys the documents share are shredded into typed leaves; `sift.suggest_paths` shows what a column would get |
| `compression` | Four tiers (`L0` slab, `L0`, warm `L1`, cold `L2`) with the codec chosen per column |
| `preagg`, `skip_index` | Per-granule pre-aggregates and the zero-bloat skip indexes, capped at about 1% of the table |
| `connect` | Foreign keys between `sift_table`s are discovered from `pg_constraint` |
| `SiftHybridScan` | Active whenever a `sift` (BM25) index exists on the table |

Override any of them online with `sift.table_set_option` (section 3), for example `SELECT sift.table_set_option('events_col', 'key_order', 'tenant_id, event_date');`.

`CREATE TABLE ... USING sift_table WITH (key_order = '...')` is not available: PostgreSQL rejects reloptions on table access methods, so per-table settings use the SQL functions below.

### Tables Created by 5.x

A `sift_table` created by 5.x (Format Major 1) keeps working unchanged on 6.0: reads, writes, `VACUUM`, secondary indexes, and `SiftAgg` all run on it, and no migration is needed to upgrade the extension. To adopt Format Major 2 (smaller footprint, faster scans, automatic `key_order` and `paths`), rewrite it once, whenever convenient:

```sql
SELECT sift.upgrade_table('events_col');   -- or: VACUUM FULL events_col; or CLUSTER events_col;
```

`sift.upgrade_table` streams the live rows into a parallel Format Major 2 build with no temporary files and rebuilds the secondary indexes. It returns `bool`: `true` after it rewrote the table (with one `NOTICE` summary of rows, size before and after, and elapsed time), or `false` with a `NOTICE` when the table is already Format 2.0 or empty, so a second call is harmless. It needs the `MAINTAIN` privilege on the table. A Format Major 1 table is never an error or a health `blocker`: `sift.indexes` shows `format = '1.x'` with `state = 'ready'`, and `sift.health()` reports one `info` row, `format_upgrade_available`.

New tables and `ALTER TABLE ... SET ACCESS METHOD sift_table` conversions always write Format Major 2. `sift.indexes.format` shows `1.x` or `2.0` per table.

### Converting Existing Heap Tables

```sql
ALTER TABLE events_archive SET ACCESS METHOD sift_table;
```

`ALTER TABLE ... SET ACCESS METHOD` rewrites the relation once under `AccessExclusiveLock` and rebuilds secondary indexes automatically.

## 3. Per-Table Options (10 Options)

Because PostgreSQL table access methods do not yet support `WITH (...)` reloptions, per-table settings live in `sift.table_option_values` and are managed via SQL functions (preserved across `pg_dump` / `pg_restore`):

```sql
SELECT sift.table_set_option('events_col', 'compression', 'zstd');
SELECT sift.table_get_option('events_col', 'compression');
SELECT sift.table_reset_option('events_col', 'compression');
SELECT * FROM sift.table_options('events_col');   -- every option: value in force, source, built-in default
```

Ten options are documented: 5 core storage options, and 5 analytical options that share their names, grammar, and parser with the `sift_col` reloptions of the same name, so there is nothing new to learn. 5.1 had 17; the rest are automatic in 6.0. Projections, tiering, and TTL add **no options**: they are `sift.*` functions (sections 8 and 9).

**Core storage options**

| Option | Default | Values | Purpose |
|---|---|---|---|
| `compression` | `''` (auto) | `auto`, `lz4`, `zstd`, `none`, `col=codec` | Compression codec override for L1/L2 parts |
| `key_order` | `''` (auto) | `auto`, `off`, or 1..8 columns (`col [ASC\|DESC]`) | Online physical sort and partition key |
| `paths` | `''` (auto) | `auto`, `off`, or path spec | JSONB shredding paths for the JSONB columns of the table |
| `dead_rewrite_pct` | `0` (inherit) | `0..100` | Dead-row percentage triggering partition compaction (`0` inherits `sift.dead_rewrite_pct`, default `15`) |
| `auto_defrag` | `on` | `true` / `false` | Enables background compaction (inherits `sift.async_maintenance`) |

**Shared analytical options** (same grammar as `sift_col`)

| Option | Default | Values | Purpose |
|---|---|---|---|
| `scope` | `''` | `'name: fk -> rel(member, key) [\| rel2(via, key) via rel3(via, member)]'` | Entity-Once tenant and hierarchy scopes ([SCOPES.md](SCOPES.md)) |
| `connect` | `''` (auto) | `'fk -> [schema.]parent(key)'` | Parent-child connection for a soft foreign key; real foreign keys are discovered automatically |
| `preagg` | `''` (auto) | `auto`, `off`, `cube(...); rollup(...); calendar(...)` | Granule pre-aggregates ([ANALYTICS.md](ANALYTICS.md#3-pre-aggregates-preagg)) |
| `sketches` | `''` | `'hll(col, ...); kll(col, ...)'` | Opt-in approximate sketches: `hll` for `sift.approx_count_distinct()`, `kll` for `sift.approx_percentile()` and `sift.approx_quantiles()` ([ANALYTICS.md](ANALYTICS.md#8-approximate-distinct-counts-siftapprox_count_distinct)) |
| `skip_index` | `''` | `'col:bloom, col:trigram'` | Opt-in high-cardinality skip indexes; the others are automatic |

```sql
SELECT sift.table_set_option('events_col', 'scope', 'tenant: member_id -> tenant_members(member_id, tenant_id)');
SELECT sift.table_set_option('events_col', 'sketches', 'hll(user_id)');
```

`sift.table_set_paths(tbl, col, spec)` is the deprecated spelling of the `paths` option for one `jsonb` column; it still works. `track_changes`, `projection`, and `vacuum_truncate` are `sift_col` reloptions with no `sift_table` counterpart: the table tracks its own changes natively, and `VACUUM` truncates reclaimed tail blocks by default. The 13 retired 5.1 options (`write_buffer_limit`, `wide_rows`, `inline_promote`, `async_promote`, `backpressure`, `hot_update`, `bulk_groups`, `dict_lookup`, `key_table`, and the four `auto_defrag_*` tuning options) are hidden and deprecated: `pg_dump` / `pg_restore` and `sift.table_reset_option` accept them silently, `sift.table_set_option` accepts them with a deprecation `NOTICE` so 5.1 scripts keep running, a Format Major 1 table keeps applying its stored values, and a Format Major 2 table ignores them. Any other unknown name raises an error that lists the 10 documented options.

Changing an option on a populated table is online: existing parts adopt it as compaction rewrites them, and setting `key_order` prints a `NOTICE` saying so. `SELECT * FROM sift.maintain('events_col');` rewrites them now.

## 4. Online `key_order` and Partition Management

Setting `key_order` on a `sift_table` is completely online — no `VACUUM FULL` or `AccessExclusiveLock` is required:

1. Incoming `INSERT`, `UPDATE`, `COPY`, and `CREATE TABLE AS` rows are routed through the cached 2-level `PartitionDirectory` and written as sorted L0 micro-parts (batches ≥ 64 rows/partition) or appended to partition-local `SIFT_PT_L0_SLAB` pages (small OLTP batches).
2. Existing unkeyed parts remain 100% readable while background workers incrementally repartition and sort them into keyed L1 parts.
3. Partitions split automatically as they grow and adjacent partitions merge while they hold less than 48 MB together, so key-equality scans keep reading few granules under steady churn. This native range partitioning replaces the 5.x `key_cut` option.

## 5. Zero-Refusal `COPY` and Bulk Loading

In `pg_sift` 6.0, `COPY`, `INSERT ... SELECT`, and `CREATE TABLE AS` all flow through the unified columnar `SiftTxnStage` pipeline with **zero refusal cliffs**:

- **Works unconditionally** with `UNIQUE` and `EXCLUSION` indexes, foreign keys, `BEFORE` and `AFTER` triggers, `DOMAIN` columns, UDF defaults/checks, `IDENTITY` sequences, secondary B-tree / `sift` / `sift_col` indexes, and logical replication publications.
- **Strictly bounded memory:** Staging buffers track real allocations through `SiftMemTracker` and flush sorted L0 micro-parts automatically whenever `work_mem` pressure reaches the soft threshold.
- **Zero WAL on rollback:** Subtransaction or statement aborts (such as a `UNIQUE` conflict) discard staged rows in memory before flush.

### Direct-to-`COLD_L2` Bulk Load

A bulk call (`COPY`, `INSERT ... SELECT`, `CREATE TABLE AS`) of at least 64 rows per `multi_insert` batch into a table that is not published and has no row triggers skips the `L0` micro-parts. Rows are packed into a reusable cell array and string arena (reset every 1,024 rows), and flushed in multiples of 8,192 rows (16,384 to 65,536 rows per flush) straight into cold `L2` parts in one encoding pass: ZSTD, part-shared FSST and ZSTD dictionaries, ALP, key marks, and column statistics, with one write per page. Blocks past the end of the relation are written through bulk smgr writes; the publication check runs once per call, not per row. Smaller batches, published tables, and tables with row triggers keep the staged `L0` path, with identical results.

- **Atomic:** the flushed parts become visible only through the copy-on-write manifest commit that ends the statement, so an aborted statement or subtransaction leaves no reachable part and its extents are recycled.
- **No option:** the route is chosen from the batch; there is nothing to set. `sift.parts('events_col')` shows `L2` parts straight after a `COPY`.

## 6. MVCC, Row Locks, Secondary Indexes, and `VACUUM`

- **O(1) MVCC visibility:** Each part stores `[min_xid, max_xid]`. Once `max_xid` precedes `OldestXmin`, the part is marked all-frozen with zero per-row visibility checks (`SEC_VERS` is reclaimed on compaction).
- **Visibility map and `Index Only Scan`:** `VACUUM` marks a virtual block all-visible (and all-frozen) in the visibility map fork when every part that covers it is settled and no deletion vector, slab row, or column delta names it; `DELETE` and `UPDATE` clear the bits before the change is staged. The map is indexed by virtual block, not by a page of the main fork, and is logged through the custom resource manager `pg_sift_table_vm`, so it exists only while `pg_sift` is in `shared_preload_libraries` (a standby replays those records only if it preloads the library as well). Without the preload nothing sets a bit and an index-only scan fetches from the table. A table with a TTL, a temporary table, and an unlogged table's init fork have no map. `pg_class.relallvisible` and the planner's `allvisfrac` come from it. Byte layout: [FORMAT.md](FORMAT.md#sift_table-visibility-map).
- **Single-column `UPDATE`:** An `UPDATE` of up to 16 non-indexed, non-key columns of a row keeps the row's address, touches no secondary index (a summarizing index such as BRIN is told of the new values), and appends a compact per-column delta (`SIFT_PT_COL_DELTA`) to the partition instead of rewriting the row. A chain longer than 4 pages gets a directory (`SIFT_PT_COL_DELTA_DIR`, up to 250 entries a page) so a reader that wants one part, a few granules, or a few columns skips the pages that cannot hold them; a chain of 256 pages asks the maintenance worker for a fold, and `UPDATE` is refused only at 60,000 pages. Compaction and `VACUUM` fold committed deltas into the base columns. It is automatic: there is no option ([FORMAT.md](FORMAT.md#column-delta-chain-and-its-directory-spec-83-ws-83-3)).
- **Point fetch (`SEC_ROW_PACK`):** A part whose columns are wide enough also keeps its rows in 64-row LZ4 micro-blocks (`SEC_ROW_PACK`; the pack is kept when its first blocks take at most 40% of the bytes of the granules), so fetching one row by address decodes one block instead of the granules of every column. Analytical scans never read it. A column delta of the part is laid over the fetched row ([FORMAT.md](FORMAT.md#sec_row_pack)).
- **Native strings:** A table made by 6.0 stores `text`, `varchar`, `char(n)`, `bytea`, and `name` as bare bytes (no varlena header) and `uuid` as 16 bytes, exactly as a `sift_col` index does, so both read and compare the same column images. The table records it in its metapage (`SIFT_COMPAT_NATIVE_STRINGS`); there is no option.
- **Row locks:** `SELECT ... FOR UPDATE`, `FOR NO KEY UPDATE`, `FOR SHARE`, and `FOR KEY SHARE`, with `NOWAIT` and `SKIP LOCKED`, are supported via an in-memory lock table that writes no WAL and never modifies immutable columnar pages. A `READ COMMITTED` statement that meets a row another session just updated follows the update chain (`EvalPlanQual`), including across a crash or a lock-table overflow.
- **Isolation:** Every PostgreSQL snapshot type is honored. `SERIALIZABLE` takes a relation predicate lock on full scans and a per-virtual-block lock on point fetches, and checks conflicts once per distinct transaction rather than per row.
- **Secondary indexes:** Standard B-tree, `UNIQUE`, `EXCLUDE`, GIN, GiST, `sift` (BM25), and `sift_col` indexes attach to a `sift_table` via dense 48-bit logical TIDs (`(vblock, offset)`) that stay stable across compaction and `VACUUM`, so a B-tree answers point and range lookups by TID, and `CREATE INDEX CONCURRENTLY` sorts in bounded memory. Each part records the TID range it holds, so a TID lookup skips the parts that cannot hold it and binary-searches the rest: point lookups cost `O(log parts)`, not a scan of every part.
- **`sift` (BM25) on a `sift_table`:** Visibility of a text hit is decided from the part's own transaction summary, with no heap fetch and no HOT chain. When a query combines a `sift` match with filters, facets, or aggregates on the table, `SiftHybridScan` runs both indexes together: the column filter prunes the BM25 candidates, or the BM25 hits prune the table's granules, and the rows project straight from the granules.
- **`ALTER TABLE`:** `ADD COLUMN` (including a constant default), `DROP COLUMN`, and `RENAME COLUMN` are `O(1)` metadata changes that rewrite no part and touch no file. A part written before an `ADD COLUMN` reads the new column as its `DEFAULT` (or `NULL`) through the optional `SEC_SCHEMA_DEFAULTS` section and the catalog; new parts record the defaults in force when they were written. A dropped column is skipped on read and reclaimed as compaction rewrites each part. `ALTER COLUMN TYPE` handles binary-coercible and widening changes in place.
- **Virtual block reuse & `VACUUM`:** `VACUUM` reclaims dead extents through `SiftFsm`, compacts high-dead-ratio partitions, removes dead rows from the secondary indexes first (as heap `VACUUM` does), recycles freed virtual block ranges, and advances `relfrozenxid` and `relminmxid`. Monitor remaining capacity via `sift.space('events_col').vblocks_left` (a `WARNING` fires below 10%). `VACUUM (VERBOSE)` prints one `INFO` line ([MESSAGES.md](MESSAGES.md)).
- **Maintenance on demand:** `SELECT * FROM sift.maintain('events_col');` runs one maintenance step now, in the session (promote closed buffers, merge parts, clear dead rows, return the free end of the file) and returns `busy = true` with nothing done when another session holds the lock; background maintenance and `VACUUM` do the same by themselves. `sift.table_maintain(tbl)` is its deprecated alias.
- **`VACUUM FULL` / `CLUSTER`:** Streams live rows into the same parallel Format Major 2 build as `sift.upgrade_table`, writing frozen, sorted L1 parts on a new relfilenode with no temporary files.

## 7. Replication

- **Physical replication:** All Format Major 2 pages use PostgreSQL generic WAL (`GenericXLog` / `log_newpage_range`), so streaming standbys and PITR work with no `shared_preload_libraries`. The one exception is the visibility map of a `sift_table` (section 6): it is logged by a custom resource manager that is registered only while the library is preloaded, so preload `pg_sift` on every node of a replicated set that preloads it on the primary. A table whose map has set bits refuses `DELETE` and `UPDATE` on a server that does not preload the library.
- **Logical replication:** Publishing a `sift_table` (`CREATE PUBLICATION ... FOR TABLE events_col`) requires `shared_preload_libraries = 'pg_sift'` on the publisher (and its physical standbys) so PostgreSQL's logical decoder can decode `sift_table` custom WAL records. Subscribers need no preload.

## 8. Secondary Projections (`sift.create_projection`)

A projection is a second physical copy of a table, kept inside the same Format Major 2 relation and maintained by the same transaction that changes the base rows. `SiftAgg` picks the cheapest exact source for each query (the base table or a projection) with its cost model, and `EXPLAIN (ANALYZE, SIFT)` prints `Projection: <name>` when one served the query. There are two kinds, and neither adds a setting or an option:

| Kind | Grammar of `spec` | Serves |
|---|---|---|
| `SIFT_PROJ_SORT` | `ORDER BY col1 [DESC], col2 [INCLUDE (c3, c4)]` | Filters on a different leading key than `key_order`, read through that projection's own key marks (`INCLUDE` omitted: all columns) |
| `SIFT_PROJ_AGG` | `GROUP BY k1, date_trunc('month', ts) MEASURES count(*), sum(x), min(y), max(y)` | Rollups answered from pre-aggregated cells in `O(1)` per matching part |

```sql
SELECT sift.create_projection('events_col', 'p_by_user', 'ORDER BY user_id, event_date');
SELECT sift.create_projection('events_col', 'p_by_month',
       'GROUP BY date_trunc(''month'', event_date) MEASURES count(*), sum(amount)');
SELECT name, kind, state, rows, bytes FROM sift.secondary_projections;
SELECT sift.drop_projection('events_col', 'p_by_user');
```

- **API:** `sift.create_projection(rel regclass, name text, spec text) RETURNS bool` and `sift.drop_projection(rel regclass, name text) RETURNS bool` need the `MAINTAIN` privilege on the table; the view `sift.secondary_projections` lists `relation, name, kind, state, spec, rows, bytes, synced_version` (`BUILDING`, `READY`, or `STALE`) and needs `SELECT`. Definitions live in `sift.projection_defs`, which `pg_dump` keeps. They are unrelated to the 5.x named projections of a `sift_col` index (the `sift.projections` table and `sift.projection_create`, [REFERENCE.md](REFERENCE.md)), which keep their names.
- **Exact under change:** a projection is derived part by part from the base parts and linked to the base part generation and deletion-vector generation it reflects. A base part that was rewritten, dropped, or given another deletion vector no longer matches its link, so it is read from the base: staleness is never a flag, and a projection never returns rows the base would not. Base commits, flushes, compaction, and `VACUUM` refresh projections in the same copy-on-write manifest commit, under the same memory and WAL budgets.
- **Aggregate cells:** `count`, `sum`, and `avg` over integer, date, timestamp, and `numeric` columns are kept as exact 128-bit sums, and settled deletes are subtracted exactly. `min` and `max` are kept only when the part has no dead row. A `float` sum is refused at create because its value depends on row order, and `min`/`max` of text is not kept (collation).
- **When it is read (mixed serving):** a projection need not cover every base part. A statement reads the projection for each part it matches exactly (an aggregate cell also serves a part whose settled deletes can be subtracted) and reads the base parts, the write slabs, and the partitions with pending deletes from the index or table as before, in the same snapshot; a projection that serves no part, or costs more than the base, is not used. Aggregate cells answer filters on integer, date, timestamp, text, `uuid`, `numeric`, and boolean columns (comparisons, `IN` lists, and `<>`) with a plain `GROUP BY`; a `SIFT_PROJ_SORT` projection answers `ORDER BY ... LIMIT k` and key-range row queries by merging its parts, read lazily in their own order, with the rows of the rest. `sift_col` heap parts are read from a projection only when the visibility map shows every block of the part's address range.
- **Drop:** dropping a projection releases its extents through the free-space map after the snapshot horizon.

## 9. Cold Object-Storage Tiering and TTL

Format Major 2 parts are immutable and CRC32C-verified, so an old part can move to object storage and back into reads without changing a query. Tiering and TTL are for `sift_table`, are set per table with SQL functions, and add no setting or option:

```sql
SELECT sift.set_tier_policy('events_col', 'file:///mnt/cold/events', '30 days');
SELECT sift.set_tier_policy('events_col', 's3://my-bucket/events', '30 days', 'https://s3.example.com');
SELECT sift.set_ttl('events_col', 'event_date', '400 days');   -- NULL clears
SELECT * FROM sift.drop_parts_before('events_col', now() - interval '400 days');
SELECT * FROM sift.tier_status();
SELECT sift.clear_tier_policy('events_col');
```

- **Backends:** `file://<dir>` uses plain POSIX reads and atomic renames (a cold NVMe or NFS tier); `s3://<bucket>/<prefix>` and `r2://<bucket>/<prefix>` sign requests with AWS Signature V4 over `libcurl.so.4`, opened with `dlopen` on first use, so there is no link-time dependency: without `libcurl`, `s3://` and `r2://` raise a clear error and `file://` is unaffected. Credentials come from the server environment (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION`, `AWS_SESSION_TOKEN`) and are never written to a table, log, error, or `sift.tier_status()`.
- **What moves:** `VACUUM`, autovacuum, and `sift.maintain()` upload up to 8 eligible parts per run: sealed cold `L2` parts that are all-visible and frozen, have no deletion vector or column delta, and whose newest time-column value is older than `move_after`. The object is written and verified (size, `ETag`, CRC32C) first; one copy-on-write manifest commit then marks the part remote and releases its local blocks. A crash before the commit leaves an orphan object that a later sweep deletes; an unreachable store is a `WARNING` and `VACUUM` goes on.
- **Reads:** scans, `SiftAgg`, and standbys open a remote part through a bounded local range cache (1 MiB chunks, CRC-checked, least recently used, sized by the `cache_mb` column of `sift.tier_policies`, default 1,024). Key marks, zone maps, and the granule directory are cached, so a selective query fetches only the byte ranges of surviving granules. Remote parts are not rewritten by compaction.
- **TTL:** `sift.set_ttl(rel, col, retention)` names a time column of the table. A part whose newest value of `col` is older than `retention`, and whose rows are settled (all-visible, frozen, older than the horizon), leaves the manifest in one `O(1)` commit with its blocks, deletion vectors, and packed pages, without reading a row (`ttl_only_drop_parts`). A part with one row inside the retention stays whole. A standby conflict is raised before the commit releases the extents, and a dropped remote object is deleted only after `max(1 h, max_standby_streaming_delay)`. Indexes on other access methods keep stale entries until `REINDEX`.
- **Imperative retention:** `sift.drop_parts_before(rel, cutoff [, col])` drops the sealed parts whose newest value of `col` (a `NOT NULL` date or timestamp time column; default the TTL column, else the first time column) is strictly older than `cutoff`, in one commit per partition and without reading or deleting a row. It returns `parts_dropped, rows_dropped, bytes_reclaimed, parts_partial, parts_unsettled`: a part that spans the cutoff stays whole (`parts_partial`), and a part that is wholly older but not settled yet, or whose partition has a column delta on `col`, waits for the next call (`parts_unsettled`). Needs `MAINTAIN`.
- **Rules:** a policy is keyed by the qualified table name (a rename detaches it), its location cannot change while parts are remote, and `DROP TABLE` leaves the objects and the policy row. `sift.tier_status()` returns `relation, part_id, part_gen, location, byte_len, tiered_at, expires_at`; every function needs `MAINTAIN` (`tier_status`: `SELECT`) and takes `SHARE UPDATE EXCLUSIVE`, so it waits for a running `VACUUM`.

## 10. Inspecting a `sift_table`

The same functions inspect a `sift_table` and a Format Major 2 `sift_col` index (`sift.index_choices()` and `sift.col_sections()` answer for both and neither raises `0A000` or quarantines the relation):

```sql
SELECT * FROM sift.status('events_col');                -- format, state, rows, dead rows, footprint, parts by level, scopes, maintenance
SELECT * FROM sift.space('events_col');                 -- blocks, free extents, parts, dead rows, vblocks_left
SELECT * FROM sift.parts('events_col');                 -- one row per part: level, rows, bytes, granules, deletion vector, pre-aggregate bytes
SELECT * FROM sift.col_granules('events_col') LIMIT 20; -- one row per granule: rows, dead rows, bytes, frozen, pre-aggregates, time stride
SELECT * FROM sift.col_sections('events_col');          -- section, column and bytes, summed over the parts
SELECT * FROM sift.index_size_breakdown('events_col');  -- where the bytes are, by category; adds up to pg_relation_size
SELECT choice, kind, value, why, off FROM sift.index_choices('events_col');
```

`sift.index_choices()` reads the file and reports what the table chose: the layout, the tier of each level, which sections the parts carry (`zone_maps`, `skip_index`, `value_sets`, `preaggregates`, `sketches`, `runs_and_keys`, `point_directory`, `address_index`, `row_pack`, `time_strides`, `key_marks`, `column_stats`, `dictionaries`) with their bytes, and the encodings of each column; the `off` column holds the command that turns a choice off where one exists. `sift.col_sections()` is the same section list by column.
