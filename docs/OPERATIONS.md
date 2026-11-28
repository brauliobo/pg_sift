# Operating and Sizing `pg_sift`

Memory budgeting, disk and WAL sizing, production monitoring, emergency switches, quarantine recovery, and crash safety for `pg_sift` 6.0. See [OPTIONS.md](OPTIONS.md) for all settings and [UPGRADING.md](UPGRADING.md) for migration procedures.

For a first look run `SELECT * FROM sift.health();` (zero rows means nothing needs attention) and `SELECT * FROM sift.indexes;`. Every monitoring view returns only relations the caller is allowed to inspect (table owner, `SELECT` or `MAINTAIN` privilege, or `pg_monitor`). Run monitoring as a `pg_monitor` role rather than superuser.

## 1. The 3-Budget O(1) Memory Model

Every allocation in `pg_sift` 6.0 belongs to one of three strictly enforced budgets tracked via `MemoryContextMemAllocated()` (`SiftMemTracker`) — no hardcoded floors override your PostgreSQL settings, and no operational path grows linearly with table size:

| Budget | Controlled By | Default | Governs |
|---|---|---:|---|
| Query execution | `work_mem`, `hash_mem_multiplier` | PG defaults | `SiftAgg` hash tables, radix spills, lookup joins, Top-N heaps, set filters |
| Maintenance & builds | `maintenance_work_mem` | PG default | `CREATE INDEX`, `REINDEX`, `VACUUM`, compaction, `sift.check(..., true)` |
| Backend cache | `sift.backend_cache_kb` | `32768` (32 MB) | Per-backend LRU across BM25 prefixes, manifests, decoded granules, dicts; the rows a transaction locks one by one count in it |
| Shared cache | `sift.shared_cache_kb` | `131072` (128 MB) | Server-wide DSM cap across manifests, parsed dictionaries, leaf stats |

### Connection Pooling Math

Because `sift.backend_cache_kb` is a hard ceiling (`-1` unlimited is removed in 6.0), worst-case extension memory is bounded linearly by active pooled connections:

- **200 pooled backends** at default `sift.backend_cache_kb = '32MB'` and `sift.shared_cache_kb = '128MB'` stay under **~6.5 GB** cache memory (`200 × 32 MB + 128 MB`), regardless of how many tables or billions of rows each backend touches.
- **High-connection pools (500+ connections):** Lower `sift.backend_cache_kb` to `4MB`–`8MB` at the database or role level and raise `sift.shared_cache_kb` to `128MB`–`256MB` so backends share dictionaries and lookup tables instead of duplicating them.

## 2. Disk, Compression, and WAL Sizing

Format Major 2 uses two-tier adaptive compression: fast L0 micro-parts for OLTP ingestion and compact L1/L2 parts (vertical bitpacking, FOR, DELTA, RLE, ALP, FSST, ZSTD/LZ4) for analytics.

| Metric (wide + JSONB workload) | Target / Typical | Notes |
|---|---:|---|
| `sift_col` / `sift_table` footprint | ≤ 33 B/row scoped index (≤ 16 B/row excluding a 16 B UUID) | Wide + JSONB schema; ~0.10–0.16× PostgreSQL heap size; ≥ 95% page utilization |
| Entity-Once `scope` overhead | ≤ 5% of entity index | ≤ 1% with `cluster` (1:1); 0 B temp files during build/CIC |
| `sift_table` bulk `COPY` (10M orders) | ≤ 14 s load, ≤ 420 MiB | Direct-to-`L2` route: one write per page, no `L0` micro-parts, no second pass ([TABLES.md](TABLES.md#direct-to-cold_l2-bulk-load)) |
| Bulk `CREATE INDEX` WAL (`replica`) | ≤ 45 B/row | `0 B/row` under `wal_level = minimal` |
| Single-row OLTP `INSERT` WAL | ≤ 220 B/row | Appends to partition-local `SIFT_PT_L0_SLAB` |
| Compaction write amplification | ≤ 2.35× steady-state | Sealed slabs flush straight to `L1`; compaction reuses untouched granules as raw bytes; a part is rewritten only past `dead_rewrite_pct` |

Throttle background maintenance I/O and WAL impact with `sift.maintenance_wal_rate` (kB/s) and `sift.maintenance_cost_delay` (ms).

**Projections cost space and write work.** Each projection of a table is a second derived copy of its parts: a `SIFT_PROJ_SORT` projection with all columns roughly doubles the footprint, an `INCLUDE` list or a `SIFT_PROJ_AGG` rollup is far smaller. Base writes, flushes, compaction, and `VACUUM` refresh every projection in the same manifest commit and under the same `sift.maintenance_wal_rate` budget. Check `bytes` in `sift.secondary_projections` against the base before adding one, and `sift.drop_projection` when a query no longer needs it.

**Cold-tier disk and network.** A tiered part leaves the data files (`sift.space()` shows the blocks released); reads that touch it use a bounded local cache directory sized by `cache_mb` in `sift.tier_policies` (default 1,024 MB), so budget that disk and the object-store request rate for cold queries.

## 3. Observability: Four Surfaces

Four surfaces answer every operational question without joining catalogs by hand. Each returns only relations the caller may inspect (table owner, `SELECT` or `MAINTAIN` privilege, or `pg_monitor`), and none of them fails on a mixed fleet: a quarantined index, an invalid concurrent build, or a locked `REINDEX` comes back as a diagnostic row.

| Surface | Kind | Answers |
|---|---|---|
| `sift.indexes` | view | What exists and is it healthy, at a glance |
| `sift.status(rel)` | function | Deep storage, part tiers, maintenance, and cache numbers per relation (metadata only, no data-block reads) |
| `sift.health(rel)` | function | What needs attention now, with the SQL that fixes it (zero rows when healthy) |
| `sift.activity()` | function | What the maintenance workers, the pacer, and the shared-memory pools are doing right now |

### 3.1 Fleet at a Glance (`sift.indexes`)

One row per `sift`, `sift_col`, and `sift_table` relation (it absorbs the legacy `col_index_status` view):

```sql
SELECT relation, am, format, state, health, rows, dead_pct,
       pg_size_pretty(size_bytes) AS size, b_per_row, compression_ratio, util_pct, parts
FROM sift.indexes
ORDER BY size_bytes DESC;
```

| Column | Meaning |
|---|---|
| `am`, `format` | `sift_col`, `sift_table`, or `sift`; on-disk format `2.0`, `1.7` (BM25), `1.x` (a 5.x columnar relation, fully supported; upgrade optional), or `empty` |
| `state` | `ready`, `maintaining`, `quarantined`, `failing`, `invalid`, or `locked` |
| `health` | Worst severity of `sift.health(relation)`: `ok`, `info`, `warn`, or `error` |
| `rows`, `dead_pct`, `size_bytes`, `b_per_row` | Live and dead rows, dead share, relation size, bytes per live row |
| `compression_ratio`, `util_pct` | Heap size over index size (`sift_col`), and page utilization (target `>= 95%`) |
| `parts`, `scopes`, `quarantined` | Compact tier summary such as `L0:1 L1:2 L2:4`, active scope count, quarantine flag |

### 3.2 Deep Status (`sift.status()`)

Reads block 0, the free-space map, and the part directory only, so a scrape takes milliseconds even on very large relations:

```sql
SELECT relation, state, live_rows, dead_rows, dirty_rows, size_bytes, util_pct,
       free_bytes, l0_slabs, l0_parts, l1_parts, l2_parts,
       maint_due, maint_failures, last_maint_at, shrink_blocked_by
FROM sift.status();                    -- or sift.status('orders_col')
```

Columns come in five groups: identity (`relation`, `table_name`, `am`, `format_major`, `format_minor`, `state`, `quarantined`, `quarantine_reason`), row health (`live_rows`, `dead_rows`, `dirty_rows`, `dead_pct`), footprint (`size_bytes`, `heap_bytes`, `b_per_row`, `compression_ratio`, `util_pct`, `data_bytes`, `meta_bytes`, `scope_bytes`, `skip_bytes`, `free_bytes`, `reusable_bytes`, `slack_bytes`), structure (`partitions`, `l0_slabs`, `l0_parts`, `l1_parts`, `l2_parts`, `hot_parts`, `cold_parts`, `granules`, `scopes`, `vblocks_left`), and maintenance and shared memory (`maint_running`, `maint_due`, `maint_failures`, `last_maint_at`, `last_shrink_at`, `shrink_blocked_by`, `shm_manifest_cached`, `shm_manifest_bytes`, `shm_hit_pct`). Columns that do not apply to an access method are `NULL`. `sift.space(rel)` remains the focused extent view and `sift.index_size_breakdown(rel)` the per-codec byte breakdown.

### 3.3 One-Command Advisor (`sift.health()`)

```sql
SELECT severity, relation, check_name, detail, action_sql FROM sift.health();
```

It returns **zero rows when everything is healthy**, and every row carries a copy-pasteable `action_sql`. Rows are ordered `error`, `warn`, `info`:

| `check_name` | Severity | Fires when | Typical `action_sql` |
|---|---|---|---|
| `index_quarantined` | error | The relation is quarantined | `REINDEX INDEX CONCURRENTLY <idx>;` |
| `index_invalid` | error | An interrupted concurrent build left an invalid index | `DROP INDEX CONCURRENTLY <idx>;` |
| `vblocks_low` | error below 2%, warn below 10% | Virtual TID blocks of a `sift_table` are running out | `VACUUM <table>;` |
| `worker_failing` | error from 5 failures, else warn | Background maintenance keeps failing | `SELECT * FROM sift.maintain('<rel>');` |
| `tail_shrink_blocked` | warn | At least 4 MB and 15% of the file is free but a standby, snapshot, or lock blocks truncation | `SELECT * FROM sift.maintain('<rel>');` |
| `high_dead_rows` | warn | Dead rows reach 25% and maintenance is off or stale | `SELECT sift.merge('<rel>');` |
| `l0_backlog` | warn | 12 or more L0 parts or 32 or more slabs wait for compaction | `SELECT sift.promote('<rel>');` |
| `stale_scope` | warn | More than 10% of a scope's keys are dirty | `SELECT sift.col_refresh('<idx>');` |
| `format_upgrade_available` | info | A `sift_col` index or `sift_table` is still on Format Major 1 (works fully; Format 2.0 is smaller and faster) | `REINDEX INDEX CONCURRENTLY <idx>;` or `SELECT sift.upgrade_table('<tbl>');` |
| `heap_visibility_low` | info | Under 50% of heap pages are all-visible, so scans vet extra heap pages | `VACUUM <table>;` |
| `shared_cache_pressure` | info | A shared-memory pool refused a reservation or sits above 90% | `ALTER SYSTEM SET sift.shared_cache_kb = '<kb>';` |
| `extension_not_preloaded` | info | `pg_sift` is in neither `session_preload_libraries` nor `shared_preload_libraries` | `ALTER DATABASE <db> SET session_preload_libraries = 'pg_sift';` |

### 3.4 Space, Free Extents, and `vblocks_left` (`sift.space()`)

Unified space and shrink diagnostics across all three access methods:

```sql
SELECT am, parts, rows, dead_rows, pg_size_pretty(bytes) AS size,
       free_blocks, reusable_blocks, shrink_blocked_by, vblocks_left
FROM sift.space('orders_col');
```

Inspect Format Major 2 parts, granules, and scope trees directly via `sift.parts(rel)`, `sift.col_granules(rel)`, and `sift.scope_stats(idx)`. Check DSM pools via `SELECT * FROM sift.shared_memory();`.

### 3.5 Workers, Queue, Pacer, and Shared Memory (`sift.activity()`)

One call replaces joining `sift.progress`, `sift.workers`, `sift.shared_memory()`, and `pg_stat_activity` during an incident. It returns `kind = 'worker'` (an active job), `kind = 'queued'` (a pending job), and exactly one `kind = 'summary'` row:

```sql
SELECT kind, pid, relation, job, phase, priority, progress_pct, bytes_written, wait_event
FROM sift.activity()
WHERE kind <> 'summary';

SELECT workers_active, workers_max, pacer_mode, pacer_delay_ms, oltp_active_backends,
       shm_used_bytes, shm_limit_bytes, shm_util_pct, shm_refusals
FROM sift.activity()
WHERE kind = 'summary';
```

`job` is one of `slab_flush`, `pend_del`, `compact_l1`, `compact_l2`, `split`, `merge`, `scope_refresh`, `tail_shrink`, `quarantine`, or `bm25_merge`. `wait_event` shows `SiftMaintenancePace` while a worker waits in the pacer (see [section 8](#8-self-tuning-background-maintenance)) and `SiftTailShrinkLock` while it retries a tail shrink.

### 3.6 On-Disk Integrity Verification (`sift.check()`)

Verifies checksums, manifest chains, granules, and deletion vectors without triggering quarantine (safe on standbys):

```sql
SELECT * FROM sift.check();                       -- all accessible sift/sift_col/sift_table relations
SELECT * FROM sift.check('orders_col', true);     -- also cross-checks every row against heap
```

Zero rows means sound. Passing `heapallindexed => true` compares index contents against the heap within `maintenance_work_mem`.

### 3.7 Companion Views

- **`sift.workers`**: one row per relation with maintenance state, failures, and retry backoff (`relation, kind, running, failures, failures_total, last_ok, last_job, last_ms, due, next_retry, last_error`).
- **`sift.progress`**: active `promote`, `merge`, `rewrite`, and `vacuum` jobs (`pid, relation, job, phase, parts_done, parts_total, bytes_written, started`).
- **`sift.locks`**: extension locks held by sessions and workers, including the Format Major 2 partition, maintenance, load, worker, and split locks.
- **`sift.shared_memory()`**: the 5.1 view of the shared pools, with its 8 columns unchanged (`name`, `configured_bytes`, `mapped_bytes`, `used_bytes`, `hits`, `misses`, `shared`, `reason`) and one row for each of the three pools, so monitoring written for 5.1 keeps working.
- **`sift.shared_memory_pools()`**: the ledger of the same three pools (`manifest`, `col_dict`, `col_leaf`), always three rows, with `limit_bytes`, `used_bytes`, `peak_bytes`, `util_pct`, `evictions`, `refusals`, `shared`, and `reason`. `sift.health()` and `sift.activity()` read it.
- **`sift.quarantine`**: relations taken out of service after on-disk damage.

### 3.8 Prometheus and Grafana (`postgres_exporter`)

The columns are typed numbers, booleans, and enums, so no regex parsing is needed. Run the exporter as a `pg_monitor` role, not a superuser:

```yaml
pg_sift_relation_status:
  query: |
    SELECT relation::text AS relation, am, state,
           COALESCE(live_rows, 0) AS live_rows, COALESCE(dead_rows, 0) AS dead_rows,
           COALESCE(dirty_rows, 0) AS dirty_rows, size_bytes,
           COALESCE(b_per_row, 0) AS b_per_row, COALESCE(util_pct, 0) AS util_pct,
           COALESCE(free_bytes, 0) AS free_bytes, COALESCE(l0_slabs, 0) AS l0_slabs,
           COALESCE(l0_parts, 0) AS l0_parts, COALESCE(l1_parts, 0) AS l1_parts,
           COALESCE(l2_parts, 0) AS l2_parts, maint_failures,
           CASE WHEN quarantined THEN 1 ELSE 0 END AS quarantined,
           COALESCE(vblocks_left, -1) AS vblocks_left
    FROM sift.status();
  metrics:
    - relation: { usage: "LABEL" }
    - am: { usage: "LABEL" }
    - state: { usage: "LABEL" }
    - live_rows: { usage: "GAUGE", description: "Live rows in the pg_sift relation" }
    - dead_rows: { usage: "GAUGE", description: "Dead rows pending compaction" }
    - dirty_rows: { usage: "GAUGE", description: "Unflushed L0 slab and pending delete rows" }
    - size_bytes: { usage: "GAUGE", description: "On-disk relation size in bytes" }
    - b_per_row: { usage: "GAUGE", description: "Bytes per live row" }
    - util_pct: { usage: "GAUGE", description: "Page utilization percentage (target >= 95)" }
    - free_bytes: { usage: "GAUGE", description: "Free-space map plus quarantined bytes" }
    - l0_slabs: { usage: "GAUGE", description: "Active L0 slab pages" }
    - l0_parts: { usage: "GAUGE", description: "Level 0 micro-part count" }
    - l1_parts: { usage: "GAUGE", description: "Level 1 warm part count" }
    - l2_parts: { usage: "GAUGE", description: "Level 2 cold part count" }
    - maint_failures: { usage: "GAUGE", description: "Consecutive background maintenance failures" }
    - quarantined: { usage: "GAUGE", description: "1 if the relation is quarantined" }
    - vblocks_left: { usage: "GAUGE", description: "Remaining virtual TID blocks of a sift_table (-1 for indexes)" }

pg_sift_activity_summary:
  query: |
    SELECT workers_active, workers_max, pacer_delay_ms, oltp_active_backends,
           shm_used_bytes, shm_limit_bytes, COALESCE(shm_util_pct, 0) AS shm_util_pct, shm_refusals
    FROM sift.activity() WHERE kind = 'summary';
  metrics:
    - workers_active: { usage: "GAUGE", description: "Active maintenance workers" }
    - workers_max: { usage: "GAUGE", description: "Effective maintenance worker cap" }
    - pacer_delay_ms: { usage: "GAUGE", description: "Adaptive maintenance delay per 256 KB (ms)" }
    - oltp_active_backends: { usage: "GAUGE", description: "Foreground backends sampled by the pacer" }
    - shm_used_bytes: { usage: "GAUGE", description: "Used bytes in the shared-memory pools" }
    - shm_limit_bytes: { usage: "GAUGE", description: "Configured sift.shared_cache_kb in bytes" }
    - shm_util_pct: { usage: "GAUGE", description: "Shared-memory utilization percentage" }
    - shm_refusals: { usage: "COUNTER", description: "Cumulative shared-memory reservation refusals" }

pg_sift_health_alerts:
  query: |
    SELECT severity, check_name, count(*) AS issues FROM sift.health() GROUP BY severity, check_name;
  metrics:
    - severity: { usage: "LABEL" }
    - check_name: { usage: "LABEL" }
    - issues: { usage: "GAUGE", description: "Relations failing this pg_sift health check" }
```

Page on `severity = "error"`; open a ticket for `severity = "warn"` lasting more than 15 minutes; raise `sift.shared_cache_kb` when `shm_util_pct` stays above 90 or `shm_refusals` grows.

### 3.9 `EXPLAIN (ANALYZE)` Output (Format 2.0)

A scan of a Format 2.0 `sift_col` index or `sift_table` prints four to six lines under the `Custom Scan (SiftAgg)` node. Zero counters are left out unless the statement is `VERBOSE` or the counter is not zero, and sizes use the same units as the messages (`512B`, `64KB`, `2.8MB`, `1.38GB`):

```text
Custom Scan (SiftAgg) on events  (actual time=4.120..4.890 rows=48 loops=1)
  Index: idx_events_col (Format 2.0, key_order=entity_id, event_date)
  Group Keys: status, notes.tag  |  Aggregates: count(*), sum(amount)
  Parts: scanned=4 pruned_key=188 pruned_time=8 l0_slabs=1
  Granules: scanned=62 pruned_zone=410 pruned_skip=118 pruned_dict=12 preagg=48
  Exec: SIMD=avx512 workers=4/4 cfilt=91.4% (dict=38 rle=14 forbit=8 fsst=2) late_mat_saved=18.4MB group=dict-direct
  I/O & Mem: aio_batches=14 (112 blks) peak_mem=2.8MB/4.0MB
```

| Line | Reads as |
|---|---|
| `Index` | The relation scanned and its format. For a `sift_table` the line names the table and its access method, and reports `NativeMVCC`: its visibility needs no heap check |
| `Parts` | Parts scanned, and parts skipped by `key_order` range (`pruned_key`) or by time bounds (`pruned_time`) |
| `Granules` | Granules scanned, and skipped by zone maps, skip indexes, or dictionaries; `preagg` counts granules answered from pre-aggregates without reading rows |
| `Exec` | SIMD level, workers launched over planned, share of filters evaluated directly on compressed data (`cfilt`) and by codec, bytes of late materialization saved |
| `I/O & Mem` | Asynchronous read batches and the peak memory against its budget; `spills` appears only when the statement spilled |
| `SIP Filter`, `Fallback` | A runtime join filter (its size and the share of rows it passes), and why a faster path was skipped |

A scope query prints `Scope: <name> on <index> (clustered=primary_parent, keys=N)` and `Scope Exec: L2_hit=N topk_ring=N/N (0 spills, 0 heap_vet)`; a `sift` index combined with a `sift_col` index or a `sift_table` prints a `Custom Scan (SiftHybridScan)` node with `Hybrid: order=bm25_first bm25_hits=312 -> col_granules=4/1221 (pruned=99.7%)`. In `FORMAT JSON`, `YAML`, and `XML` every number is a property under `Sift` (byte counts as integers, percentages as `0..100`), so monitoring needs no text parsing.

`EXPLAIN (SIFT)` (PostgreSQL 18+) adds each refused arm with its reason and the fix on the same line, so a second query is not needed. A Format 1.x relation keeps the 5.x plan output (`Units`, `Sift Segments`) with a calm state line, `Sift Index State: Format 1.51 (compat), 3 segments, 0 buffered rows, 0.2% dead`; a Format 2.0 relation prints `Sift Index State: Format 2.0, 4 parts (1 L0, 2 L1, 1 L2), 123 granules, 0.4% dead`.

### 3.9a Projections, Tiers, and TTL

| Surface | Reads as |
|---|---|
| `Projection: <name>` in `EXPLAIN (ANALYZE, SIFT)` | `SiftAgg` served the query from that secondary projection instead of the base parts |
| `sift.secondary_projections` | One row per projection: `relation, name, kind, state, spec, rows, bytes, synced_version`. `STALE` means its definition no longer matches (never read; recreate it) |
| `sift.tier_status()` | One row per tiered part: `relation, part_id, part_gen, location, byte_len, tiered_at, expires_at` (`expires_at` set by TTL) |
| `sift.parts(rel)`, `sift.check(rel)` | Remote parts show as such; `sift.check` cross-checks projection and tier directories against the manifest |

An unreachable object store raises a `WARNING` from `VACUUM` and the tier step ends; reads of parts already remote fail with a clean "object not found" or I/O error instead of returning wrong rows. The credentials of `s3://` and `r2://` come from the server environment only (see [TABLES.md](TABLES.md#9-cold-object-storage-tiering-and-ttl)).

### 3.10 Progress of Builds, `VACUUM`, and Maintenance

Every long operation reports into the standard PostgreSQL views as well as `sift.activity()`:

| Operation | Where to watch | Phases and counters |
|---|---|---|
| `CREATE INDEX`, `REINDEX` (also `CONCURRENTLY`, serial and parallel) | `pg_stat_progress_create_index` | `sampling schema and key order`, `scanning table`, `writing parts`, `compacting parts`, `building scope summaries`, `installing change tracking`; tuples and heap blocks done, and scopes done during the scope phase |
| `VACUUM` of a `sift_table` | `pg_stat_progress_vacuum` | Phase, parts scanned, index vacuum count, and dead-row bytes against `maintenance_work_mem` |
| Compaction, defrag, `sift.upgrade_table`, `VACUUM` of a `sift_col` index | `sift.activity()` and `sift.progress` | Parts done over total, bytes written, start time |

`sift.activity()` derives `elapsed_ms`, `progress_pct`, `throughput_mb_s`, and `eta_s` from those counters (`NULL` until the first unit of work is done), so a dashboard can show an estimate without extra shared memory. Durations are `*_ms` integers, ratios are `*_pct numeric(5,2)`, sizes are `*_bytes` integers (with `sift.indexes` also showing `size` through `pg_size_pretty`), and a `NULL` means *not applicable* where `0` means *measured zero*.

## 4. Alert Conditions

Alert on `sift.health()` first: it already encodes the thresholds below and returns the fix. The raw signals stay available for dashboards.

| Signal | Query / Column | Alert When | First Action |
|---|---|---|---|
| Any health error | `sift.health()` | `severity = 'error'` | Run the row's `action_sql` |
| Worker failing | `sift.status().maint_failures` / `sift.workers.failures` | `> 0` (page from 5) | Read `last_error` and `error_code`; retries at `next_retry` |
| Maintenance stalled | `sift.status().maint_due` / `sift.workers.last_ok` | `maint_due IS NOT NULL` past SLA | Check `max_worker_processes` or run `SELECT sift.maintain(rel)` |
| High dead space | `sift.indexes.dead_pct` | Rising past 20-30% | Verify autovacuum and long snapshots; run `VACUUM` on the table |
| Write backlog | `sift.status().l0_parts` / `l0_slabs` | 12+ parts or 32+ slabs | `SELECT sift.promote(rel)` |
| Shrink blocked | `sift.status().shrink_blocked_by` | Free space stays high | Resolve `standby`, `lock`, or the old snapshot holding tail blocks |
| `sift_table` vblocks low | `sift.status().vblocks_left` | `< 10%` remaining | Run `VACUUM` to reclaim frozen virtual block ranges |
| Shared cache pressure | `sift.activity()` summary `shm_util_pct`, `shm_refusals` | Above 90%, or refusals growing | Raise `sift.shared_cache_kb` |
| Shared cache fallback | `sift.shared_memory().shared` | `false` | Inspect `reason` (`/dev/shm` space or DSM slots) |
| Quarantined index | `sift.quarantine` / `state = 'quarantined'` | Any row | Rebuild with `REINDEX INDEX CONCURRENTLY <index>` |
| Invalid index | `sift.indexes.state` | `'invalid'` | Drop and recreate the interrupted `CREATE INDEX CONCURRENTLY` |
| Format 1 relation after upgrade | `sift.indexes.format` | `'1.x'` (no alert; optional) | `REINDEX INDEX CONCURRENTLY <index>` or `sift.upgrade_table(<table>)` when you want Format 2.0 ([UPGRADING.md](UPGRADING.md)) |
| Integrity problem | `sift.check()` | Any row | `REINDEX CONCURRENTLY` the index, or restore the `sift_table` from backup |
| Stale projection | `sift.secondary_projections.state` | `'STALE'` | `sift.drop_projection` and `sift.create_projection` again |
| Tier backlog or errors | `sift.tier_status()`, server log `WARNING` | Parts older than `move_after` stay local, or the store is unreachable | Check the URI, credentials, and `libcurl.so.4`; `sift.maintain(rel)` retries |

## 5. Emergency Switches

Inspect `sift.health()`, `sift.activity()`, and `pg_stat_activity` first, then toggle the narrowest switch that stops the impact:

| Incident | Switch | Effect |
|---|---|---|
| Query slow or suspect on `SiftAgg` | `SET sift.enable_agg = off` (or `ALTER DATABASE`/`SYSTEM`) | Forces PostgreSQL native plans immediately; writes continue |
| Bitmap scan on `sift_col` misbehaves | `SET sift.enable_col_scan = off` | Disables `sift_col` bitmap/index scans; `SiftAgg` stays active |
| Background workers saturate I/O or slots | `ALTER SYSTEM SET sift.async_maintenance = off` + reload | Stops all background workers; `VACUUM` and `sift.maintain()` still work |
| Background maintenance competes with foreground load | `ALTER SYSTEM SET sift.max_maint_workers = 0` + reload | Stops new maintenance workers without disabling the scheduler; `VACUUM` and `sift.maintain()` still work |
| Extension event triggers block DDL | `SELECT sift.ddl_hooks_enable(false)` (superuser) | Temporarily disables DDL triggers; re-enable with `true` to sweep orphans |

**Worker slots:** `pg_sift` always leaves at least `Max(2, max_worker_processes / 4)` worker slots free for parallel queries and other extensions. Idle workers exit well under `sift.maintenance_naptime` so `DROP DATABASE` is never blocked.

## 6. Quarantine and Corruption Recovery

If a `sift` or `sift_col` index encounters corrupt bytes (`XX001`/`XX002`) under the default `sift.on_corruption = 'quarantine'`:

1. The triggering statement fails once with a `REINDEX` hint and logs the cause.
2. The index is recorded in `sift.quarantine` (in shared memory immediately and persisted to its metapage: by the first write that skips it, in the transaction of that write, or else by a background worker).
3. Subsequent queries automatically fall back to PostgreSQL heap plans; DML and `VACUUM` skip the quarantined index with a rate-limited `WARNING`.
4. Rebuild without downtime via `REINDEX INDEX CONCURRENTLY <index>;` (or `CREATE INDEX CONCURRENTLY` + `DROP INDEX`).

Set `sift.on_corruption = 'error'` if you prefer queries to fail fast rather than fall back to slower heap scans; an index that is already out of service stays out of service, whatever the setting says, until `REINDEX`. `sift_table` relations hold primary data and cannot fall back to a heap — restore from a standby or backup if damaged.

## 7. Crash Safety, Locks, and Replication

- **Crash safety & PITR:** All Format Major 2 and BM25 pages use PostgreSQL WAL (`GenericXLog` / `log_newpage_range`). Recovery requires no manual extension step; run `SELECT * FROM sift.check();` after storage incidents.
- **Locking:** Online maintenance (`promote`, `merge`, `compact`, `VACUUM`) never acquires locks conflicting with `RowExclusiveLock` on user tables, so concurrent `INSERT`, `UPDATE`, `DELETE`, and `SELECT` never wait on compaction.
- **Physical replication:** Streaming standbys replay WAL directly. Enable `hot_standby_feedback = on` (or keep `sift.standby_conflicts = on`) to avoid retryable `40001` conflicts on long standby queries. Upgrade standbys **before** the primary (see [UPGRADING.md](UPGRADING.md)).
- **Logical replication:** Row changes replicate normally; subscribers maintain their own indexes. Publishing a `sift_table` requires `shared_preload_libraries = 'pg_sift'` on the publisher and its standbys.

### 7.1 Reading the WAL: `Btree/REUSE_PAGE` Records

`pg_waldump` shows `Btree/REUSE_PAGE` records whose relation is a `sift_col`, `sift_table`, or `sift` relation, not a B-tree. They are deliberate: when a write reuses a freed extent, `pg_sift` logs the same standby conflict record PostgreSQL uses for a recycled B-tree page, so a standby cancels (`40001`) only the queries whose snapshot could still need the old page, and no others. The record is small, comes at most once per relation and backend until the conflict horizon advances, and is written only with `wal_level = replica` or higher, a WAL-logged relation, and `sift.standby_conflicts = on` (the default). It does not mean B-tree activity.

### 7.2 What `VACUUM` and Compaction Give Back

- **Reuse, not return:** `VACUUM` and compaction free the extents of dead rows and replaced parts to `SiftFsm`; later writes of the same relation reuse them, so a steady churn does not grow the file. The file shrinks only at its tail.
- **Tail shrink:** The relation is truncated when the free tail is at least 4 MB and 15% of the file and no standby, snapshot, or lock blocks it (a streaming standby does not block a Format 2 file under `sift.standby_conflicts = off`: the standby replays the cut and cancels the queries that hold the relation, as for a heap) (`tail_shrink_blocked` in `sift.health()`, `shrink_blocked_by` in `sift.space()`). The lock of the cut is tried every 5 ms and never queued for: a `VACUUM` tries it for up to 250 ms, autovacuum and the workers for up to 50 ms, so a reader that arrives is never made to wait behind the cut. Free extents in the middle of the file stay in the file and are reused; `pg_relation_size` does not drop for them.
- **Cost:** Compaction writes a part only when it passes `dead_rewrite_pct` and reuses untouched granules as raw bytes, but a rewrite still writes WAL and a new part before the old one is freed, so allow transient space for the largest partition being compacted ([section 2](#2-disk-compression-and-wal-sizing)).

## 8. Self-Tuning Background Maintenance

Maintenance needs no `pg_cron`, no manual `sift.maintain()` calls, and no extra settings. A commit, delete, or scan that finds work due enqueues it, and up to `sift.max_maint_workers` workers (4 by default, never more than half of `max_worker_processes`) drain the queue of 8192 requests; with `shared_preload_libraries = 'pg_sift'` a launcher starts at boot, and without it workers start on demand. Workers hold no transaction or snapshot while they wait, so they never pin the cluster's oldest transaction, and a worker that dies mid-step leaves only unreferenced extents that the next `VACUUM` or maintenance pass reclaims.

Work is ordered by four priority tiers, so a tiny flush never waits behind a large cold rewrite:

| Tier | Work | Under heavy foreground load |
|---|---|---|
| P0 | Quarantine marking, `L0` slab flushes, pending-delete folds | Still runs, in micro-batches of 64 KB or less |
| P1 | Hot `L0` to `L1` compaction, hot dead-row rewrite, scope refresh | Slowed by the pacer |
| P2 | Part splits and merges, cold `L1` to `L2` recompression | Deferred for 5 to 30 seconds |
| P3 | Tail shrink | Deferred; gives up if another session waits for the lock |

Older P2 and P3 work gains priority with its age, so heavy P1 churn cannot starve it. The **pacer** samples the number of running foreground backends and the latency of recent `SiftAgg` scans, then moves between `full`, `moderate`, `throttled`, and `deferred` (visible as `pacer_mode` and `pacer_delay_ms` in `sift.activity()`). Maintenance reads and writes use a small private buffer ring, so they do not evict the foreground working set from `shared_buffers`. Your own settings stay in force: `sift.maintenance_cost_delay` is a floor for the pacer, `sift.maintenance_wal_rate` caps WAL, and manual `sift.maintain()`, `sift.merge(rel, true)`, and `VACUUM` bypass P2 and P3 deferral so you can always force a pass.

## 9. Running Without `shared_preload_libraries`

`pg_sift` works with `shared_preload_libraries` unset; nothing below needs a restart. The library loads in a session when the first statement touches one of its access methods, and background maintenance starts dynamic workers on demand instead of a launcher at postmaster start.

| Capability | Without preload | With `shared_preload_libraries = 'pg_sift'` |
|---|---|---|
| `CREATE EXTENSION`, `sift`, `sift_col`, `sift_table`, reads, writes, `VACUUM`, WAL replay on standbys and PITR | Works | Same |
| `SiftAgg`, `SiftHybridScan`, planner paths | Work; the first statement of a connection can plan before the library registers its hooks and keep PostgreSQL's plan (`extension_not_preloaded` in `sift.health()`); `session_preload_libraries` per database removes that | Hooks exist before the first statement |
| Quarantine of a damaged index | The on-disk quarantine flag is checked when the planner costs the index and when a scan starts, so the first query of a session never uses a quarantined index | Same, and the in-memory registry is shared by all backends from the start |
| Background maintenance (L0 flush, compaction, scope refresh, tail shrink) | Dynamic workers started by the commit, delete, or scan that finds work due, within `sift.max_maint_workers` and always leaving `Max(2, max_worker_processes / 4)` worker slots free; they exit after two idle naps | One launcher sleeps at postmaster start and dispatches the same workers |
| Shared pools (`sift.shared_cache_kb`) | Allocated lazily in PostgreSQL's dynamic shared memory registry; `sift.shared_memory()` reports `shared = false` with a `reason` when DSM is unavailable | Reserved at startup |
| Logical replication of a `sift_table` | **Not available**: the publisher's decoder needs the library at startup; `CREATE PUBLICATION` is refused with a hint | Works; subscribers need no preload |

Preload when you publish a `sift_table`, run many short-lived connections that must plan on `SiftAgg` from their first statement (or use `session_preload_libraries`), or want the shared pools reserved at boot.

## 10. Backup, `pg_dump`, and `pg_restore`

- **Physical backups and `pg_upgrade`** copy relation files as they are: Format Major 1 and 2 relations keep their format ([UPGRADING.md](UPGRADING.md)).
- **Logical dumps** carry rows and definitions, not pages. A `sift_table` or `sift_col` index created by the restore on 6.0 is written in Format Major 2, so a 5.x dump restored on 6.0 lands on Format 2.0 without a separate upgrade step.
- **Per-table options** live in the extension tables `sift.table_option_values` and `sift.table_paths`, which `pg_dump` writes as data. A restore may load a table's rows before it loads these rows, so the first parts are written with default `key_order`, `compression`, and `paths`. Nothing fails and no row is lost: the options apply to new writes at once, and existing parts adopt them as compaction rewrites them. `sift.health()` reports `table_options_unapplied` for such a table, with `VACUUM FULL <table>` or `SELECT * FROM sift.maintain('<table>')` as the fix.
- **Projections and TTL** definitions (`sift.projection_defs`, `sift.ttl_policies`) are dumped as data. Tier policies (`sift.tier_policies`) are **not**: the objects belong to the physical table, so a restored copy must set its own policy and never shares a location with the source. A database made by `CREATE DATABASE ... TEMPLATE` copies the policies with the relations: it reads the objects of the copied parts but moves, marks and deletes none (maintenance warns), so the original keeps them; clear the policy in the copy once none of its parts is in object storage and set it again to tier there. A logical restore rebuilds projections from the definitions; a physical backup copies them as parts, and remote objects are not part of the backup, so back up the object store with its own tooling.
- **Options from 5.1** that 6.0 retired (the 13 hidden `sift_table` options) are accepted silently by `pg_restore`, so an old dump never aborts on them.
- **Zero-rewrite restore** from a custom-format dump loads the option rows first:

```sh
pg_restore --section=pre-data -d target dump.fc
pg_restore --section=data -t sift.table_option_values -t sift.table_paths -d target dump.fc
pg_restore --section=data --exclude-table-data=sift.table_option_values \
           --exclude-table-data=sift.table_paths -d target dump.fc
pg_restore --section=post-data -d target dump.fc
```

A plain `pg_dump | psql` restore needs no special handling; run the `table_options_unapplied` fix afterwards.
