# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml) [![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018%20%7C%2019-336791?logo=postgresql&logoColor=white)](docs/SUPPORT.md#compatibility) [![release](https://img.shields.io/badge/release-6.0.0-blue)](https://github.com/brauliobo/pg_sift/releases/latest) [![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

**pg_sift** brings columnar analytics, native column tables, and exact BM25 full-text search into PostgreSQL. Add a `sift_col` index to an existing OLTP table—or store data directly in a `sift_table`—and PostgreSQL's planner answers aggregations, filtered group-bys, top-N, and JSONB analytics through a SIMD-vectorized engine (`SiftAgg`) while keeping your existing SQL, transactions, MVCC, and replicas intact.

One `CREATE EXTENSION pg_sift`: no separate analytics database, no search cluster, no CDC pipeline, and no `shared_preload_libraries` required.

- **Best for:** Live OLTP tables serving dashboards, multi-tenant rollups, JSONB analytics, high-compression event tables, and exact BM25 search in the same transaction.
- **Exactness guarantee:** Every `SiftAgg` plan is semantically identical to PostgreSQL's native plan (`SET sift.enable_agg = off` to verify); queries it cannot prove equal run on PostgreSQL's own planner automatically.
- **Skip it for:** Primary-key point lookups (keep your b-tree), multi-node distributed warehouses requiring petabyte sharding, or CJK/n-gram text tokenization. See [docs/COMPARISON.md](docs/COMPARISON.md).

## Use Cases

### 1. Analytical Acceleration on Live OLTP Tables (`USING sift_col`)

Keep your heap table, primary keys, foreign keys, and existing SQL—including `NUMERIC`, `UUID`, joins, and shredded `JSONB`:

```sql
CREATE INDEX orders_col ON orders
  USING sift_col (customer_id, country, category, amount, created_at, payload)
  WITH (key_order = 'created_at', paths = 'payload.channel text, payload.discount float8');

-- Keep your existing SQL: runs as Custom Scan (SiftAgg) on orders_col
SELECT category, payload->>'channel' AS channel,
       count(*), count(DISTINCT customer_id), sum(amount)
FROM orders
WHERE country = 'DE' AND created_at >= now() - interval '90 days'
GROUP BY 1, 2
ORDER BY 5 DESC LIMIT 10;
```

`EXPLAIN` shows `Custom Scan (SiftAgg)`; `SET sift.enable_agg = off` runs PostgreSQL's standard plan for identical results. See [docs/ANALYTICS.md](docs/ANALYTICS.md).

### 2. Multi-Tenant & Hierarchy Scoped Analytics (`scope = ...`)

Accelerate tenant and group dashboards without duplicating fact rows or maintaining trigger-driven copies. Entity-Once scopes store fact rows once and maintain a 2-level cube-merge tree and Top-K ring with 0 B temp-file I/O:

```sql
CREATE INDEX events_col ON events
  USING sift_col (member_id, event_date, kind, score)
  WITH (
    key_order = 'member_id, event_date',
    scope     = 'tenant: member_id -> tenant_members(member_id, tenant_id)'
  );

-- Standard SQL join: answered in O(1) from pre-merged scope summaries & Top-K ring
SELECT date_trunc('month', e.event_date), count(*), avg(e.score)
FROM events e
JOIN tenant_members tm ON tm.member_id = e.member_id
WHERE tm.tenant_id = 42
GROUP BY 1;
```

Pair-table grants and revokes reconcile incrementally under your MVCC snapshot; add `cluster` for 1:1 hierarchies. See [docs/SCOPES.md](docs/SCOPES.md).

### 3. Native Columnar Storage Inside PostgreSQL (`USING sift_table`)

Store append-heavy or analytical tables directly in Format Major 2 compressed columns (≤ 33 B/row target on wide + JSONB schemas) with zero-refusal `COPY`, online `key_order`, native MVCC, secondary indexes, and the same scopes, pre-aggregates, skip indexes, and BM25 hybrid search as a `sift_col` index, tuned by 10 options and usually none:

```sql
CREATE TABLE events_col (
  id         bigint,
  tenant_id  int,
  event_date date,
  kind       text,
  amount     numeric(12,2),
  payload    jsonb
) USING sift_table;

SELECT sift.table_set_option('events_col', 'key_order', 'tenant_id, event_date');
COPY events_col FROM '/data/events.csv' WITH (FORMAT csv);

SELECT kind, count(*), sum(amount)
FROM events_col
WHERE tenant_id = 42 AND event_date >= '2026-01-01'
GROUP BY 1;
```

Shares 100% of Format Major 2 with `sift_col`—same 8,192-row granules, COW deletion vectors, and `SiftAgg` engine. Point lookups cost `O(log parts)`, a single-column `UPDATE` appends a per-column delta instead of rewriting the row, and `sift.approx_percentile()` / `sift.approx_quantiles()` answer from opt-in `kll` sketches. See [docs/TABLES.md](docs/TABLES.md).

6.0 adds the ClickHouse-class features, all with 0 new settings or options:

```sql
SELECT sift.create_projection('events_col', 'p_by_month',               -- secondary sort or rollup projection, chosen by SiftAgg
       'GROUP BY date_trunc(''month'', event_date) MEASURES count(*), sum(amount)');
SELECT sift.set_tier_policy('events_col', 's3://my-bucket/events', '30 days');   -- cold parts to S3/R2 (dlopen libcurl) or file://
SELECT sift.set_ttl('events_col', 'event_date', '400 days');                      -- O(1) part-drop TTL
SELECT country, sift.arg_max(user_id, amount) FROM events_col GROUP BY 1;         -- plus 64-FILTER pivots, ASOF, date_bin
```

Composite key marks and adaptive `PREWHERE` prune multi-column filters at granule level, trigram `LIKE '%...%'` runs as a SIMD pushdown, bulk `COPY` writes cold `L2` parts directly, and `ALTER TABLE ADD/DROP COLUMN` is `O(1)`. See [docs/ANALYTICS.md](docs/ANALYTICS.md) and [docs/TABLES.md](docs/TABLES.md#8-secondary-projections-siftcreate_projection).

### 4. Exact BM25 Full-Text Search (`USING sift`)

Ranked full-text search with boolean logic, phrases, prefix, wildcard, regex, fuzzy matching, `INCLUDE` filter pushdown, and exact facets in the same transaction as your rows:

```sql
CREATE INDEX docs_body ON docs
  USING sift (body)
  INCLUDE (category, published);

SELECT id, title, sift.score(ctid) AS score
FROM docs
WHERE body @@> '(battery OR charger) AND "friendly support" NOT squeaky'
  AND category = 1
ORDER BY sift.score(ctid) DESC
LIMIT 10;
```

The same BM25 search core also ships as standalone extensions for [SQLite](sqlite/README.md) and [MariaDB](mariadb/README.md). See [docs/SEARCH.md](docs/SEARCH.md).

### 5. Optional Vulkan Compute (`sift.gpu`)

Opt-in and off by default: build with `make VULKAN=1`, then `SET sift.gpu = auto|on` runs grouped and filtered integer and `numeric` counts and sums of a `sift_col` index on a Vulkan device. Units the device cannot take, a missing device, and a failed device all run on the CPU with identical rows. The 4 `sift.gpu*` settings are part of the 27 release GUCs; 6.0 reworks the device path for Format Major 2 (batched decode on the device, a shared VRAM cache, multi-key hash aggregation, `sift_col` and `sift_table`); GPU-versus-CPU numbers are measured at release. See [docs/GPU.md](docs/GPU.md).

## Benchmarks

Every figure checks exact answer equality (`md5_equal`) against stock PostgreSQL 18 before it counts. Dated baselines on AMD Ryzen 7 9700X (PG 18.6) and AMD Ryzen 9 5900XT (PG 18.7); amber marks a rival that wins. Methodology, fairness rules and 1-command repro: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

<!-- bench:readme-hero:begin -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-hero-dark.svg">
  <img alt="pg_sift at a glance: Revenue / month 1,481× faster than PostgreSQL 18 on 10M rows; BM25 search up to 16× ParadeDB QPS; sift_table 3.4× smaller than PostgreSQL 18; 72 of 72 scoped cells faster than stock PostgreSQL." src="docs/charts/readme-hero-light.svg" width="860">
</picture>
<!-- bench:readme-hero:end -->

<!-- bench:readme-analytics:begin -->
<details open>
<summary><b>Analytical aggregations</b> — <code>sift_col</code> wins 4 of 6 queries at 10M rows, up to 26× over ClickHouse</summary>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-analytics-dark.svg">
  <img alt="Speed against disk size at 10M rows: sift_col wins 4 of 6 queries; rivals win only where marked amber." src="docs/charts/readme-analytics-light.svg" width="860">
</picture>

`sift_col` wins 4 of 6 queries: 2.1–26× faster than ClickHouse and 47–1,481× faster than PG 18 (3 CPUs, 8 GiB, warm medians). ClickHouse leads Avg shipped order (7.68 ms vs 10.0 ms) and Big recent orders (4.39 ms vs 10.6 ms).

ClickHouse runs a plain `MergeTree`; `sift_col` keeps pre-aggregates. Cold runs, DuckDB, `ClickHouse + PROJECTION` and Elasticsearch: full tables.

| Workload (10M orders) | `pg_sift` | ClickHouse | DuckDB | PG 18 |
|---|---:|---:|---:|---:|
| Count by country (`GROUP BY`) | **0.85 ms** | 9.32 ms | 8.97 ms | 188.9 ms |
| Revenue per month (`sum`) | **0.93 ms** | 23.8 ms | 99.2 ms | 1,377 ms |
| Top 10 categories (`LIMIT 10`) | **0.90 ms** | 21.0 ms | 26.7 ms | 370.2 ms |
| Avg shipped order (3 filters) | 10.0 ms | **7.68 ms** | 48.9 ms | 124.2 ms |
| Distinct customers/mo (`DISTINCT`) | **49.3 ms** | 104.5 ms | 193.7 ms | 2,334 ms |
| Big recent orders (range count) | 10.6 ms | **4.39 ms** | 39.7 ms | 146.4 ms |

[Full tables and methodology](docs/benchmarks/analytics.md)

</details>
<!-- bench:readme-analytics:end -->

<!-- bench:readme-clickbench:begin -->
<details>
<summary><b>ClickBench <code>Q0–Q42</code></b> — 43 queries, <code>md5_equal</code> against PG 18</summary>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-clickbench-dark.svg">
  <img alt="Per-query win map of the 43 ClickBench queries: the fastest arm of each tile; amber where a rival beats pg_sift." src="docs/charts/readme-clickbench-light.svg" width="860">
</picture>

*Not measured for 6.0 yet: `bench/bench clickbench run` writes `docs/results/clickbench.tsv` and populates this section.*

[Full tables and methodology](docs/benchmarks/clickbench.md)

</details>
<!-- bench:readme-clickbench:end -->

<!-- bench:readme-search:begin -->
<details>
<summary><b>Exact BM25 search</b> — 4.1–16× ParadeDB QPS, 3 of 6 workloads at or above Elasticsearch</summary>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-search-dark.svg">
  <img alt="Throughput against index size on 50K Wikipedia articles: pg_sift is 4.1–16× ParadeDB; Elasticsearch leads in amber." src="docs/charts/readme-search-light.svg" width="860">
</picture>

`pg_sift` is 4.1–16× ParadeDB and ties or beats Elasticsearch on 3 of 6 workloads; Elasticsearch leads Filtered top-k (13,854 vs 12,604), Boolean `a OR b` (11,021 vs 8,201), Joined top-k\* (14,037 vs 6,689). \*Elasticsearch filters a denormalized field instead of joining.

10M orders title search `count(*)`: 2.35 ms vs Elasticsearch 11.6 ms and PG GIN 78.5 ms. Cold p50: `pg_sift` 1.0–5.5 ms, Elasticsearch 3.1–4.8 ms, ParadeDB 13.1–17.1 ms.

| Workload (50K, QPS) | `pg_sift` | Elasticsearch | ParadeDB | PG `rum` |
|---|---:|---:|---:|---:|
| Exact `count(*)` | **29,127** | 28,105 | 2,399 | 229 |
| Phrase `"a b"` | **21,794** | 19,423 | 2,332 | 184 |
| Boolean `a AND b` | **21,690** | 17,476 | 2,414 | 4,909 |
| Filtered top-k | 12,604 | **13,854** | 2,241 | 113 |
| Boolean `a OR b` | 8,201 | **11,021** | 2,000 | 50 |
| Joined top-k\* | 6,689 | **14,037** | 431 | 110 |

[Full tables and methodology](docs/benchmarks/search.md)

</details>
<!-- bench:readme-search:end -->

<!-- bench:readme-scopes:begin -->
<details>
<summary><b>Multi-tenant scoped analytics</b> — 72 of 72 cells faster than stock PostgreSQL, 249× on a tenant rollup</summary>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-scopes-dark.svg">
  <img alt="Read latency against write cost of the scope index and its rivals: 72 of 72 cells faster than stock PostgreSQL (v5.0)." src="docs/charts/readme-scopes-light.svg" width="860">
</picture>

Tenant 1 `sum_by_month` on 2.4M events: `sift_scope` 2.18 ms vs 542 ms on stock PG (249×); 72 of 72 warm cells faster than stock PG (`v5.0`). Cells read win / tie / loss: `sift_scope` faster / within 0.1 ms or 10% / slower. Where a rival is faster (warm): `denorm_flat`: `latest_20_rows`; `ivm`: `count_by_score`, `filtered_count`.

ClickHouse arms (`ch_join`, `ch_proj`): not measured yet.

| Rival | Warm: win / tie / loss | Cold: win / tie / loss | Tenant 1 rollup |
|---|---:|---:|---:|
| `stock_default` | **72 / 0 / 0** | **71 / 0 / 1** | 542 ms vs **2.18 ms** |
| `semijoin` | **63 / 2 / 7** | **62 / 1 / 9** | 285 ms vs **2.18 ms** |
| `denorm_flat` | **51 / 2 / 19** | **48 / 1 / 23** | 329 ms vs **2.18 ms** |
| `ivm` | **32 / 2 / 30** | **34 / 3 / 27** | 43.3 ms vs **2.18 ms** |

[Full tables and methodology](docs/benchmarks/scopes.md)

</details>
<!-- bench:readme-scopes:end -->

<!-- bench:readme-scalability:begin -->
<details>
<summary><b>Scalability and parallelism</b> — 1 to 8 workers, cold I/O by <code>io_method</code>, partitions, concurrent writers</summary>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-scalability-dark.svg">
  <img alt="Small multiples of row scale, parallel workers and cold I/O: speedup of pg_sift from 1 to 8 workers." src="docs/charts/readme-scalability-light.svg" width="860">
</picture>

*Not measured for 6.0 yet: `bench/bench scaling-io --docs` writes `docs/results/v6-scaling.tsv` and populates this section.*

[Full tables and methodology](docs/benchmarks/scalability.md)

</details>
<!-- bench:readme-scalability:end -->

<!-- bench:readme-storage:begin -->
<details>
<summary><b>Storage, churn and bounded memory</b> — <code>sift_table</code> 3.4× smaller than PG 18, RSS slope gated at 0.5 B/row</summary>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/charts/readme-storage-dark.svg">
  <img alt="On-disk size at 10M orders and peak memory by rows: sift_table 3.4× smaller than PG 18, memory flat to 10M rows." src="docs/charts/readme-storage-light.svg" width="860">
</picture>

`sift_table` is 3.4× smaller than PG 18 and within 35 MiB of ClickHouse at 10M orders, with the PK and BM25 inside. Memory is gated flat to 10M+ rows (`≤ 0.5 B/row` slope; caches capped at 16 / 64 MB).

Rows marked `gate` are 6.0 targets until the 6.0 run replaces them. OLTP churn (`10%` `UPDATE`, `5%` `DELETE`, reads during churn): not measured yet.

| Dimension | `pg_sift` | Reference | Comparison | Notes |
|---|---:|---:|---:|---|
| 10M orders (`sift_table`) | **457 MiB** | 1,549 MiB (PG 18) | **3.4× smaller** | vs 421 MiB ClickHouse, 1,457 MiB ES |
| 10M orders (`sift_col`) | **1,438 MiB** | 1,549 MiB (PG 18) | **−7% vs PG** | Shares the PG heap and PK |
| 50K Wiki BM25 index | **30.2 MiB** | 54.5 MiB (ParadeDB) | **1.8× smaller** | 12.0 MiB `positions=off`; 70.5 MiB ES |
| Scoped build RSS slope | `gate` ≤ 0.5 B/row | 470.8 B/row (v5.1) | 6.0 run pending | Scoped `CIC`, `maintenance_work_mem = 4MB` |
| 1-row `INSERT` WAL | `gate` ≤ 220 B/row | 495.7 B/row (v5.1) | 6.0 run pending | 160,000 rows |
| 2% `DELETE` WAL | `gate` ≤ 45 B/row | 98.7 B/row (v5.1) | 6.0 run pending | 160,000 rows |

[Full tables and methodology](docs/benchmarks/storage-memory.md)

</details>
<!-- bench:readme-storage:end -->

## How It Works

```text
SQL Query ──▶ Single-Pass Planner (<2 ms) ──▶ SiftAgg (2,048-row SIMD: AVX-512 / AVX2 / NEON)
                     │                                        │
                     │ (falls back if not provably equal)     ▼
                     ▼                          Format Major 2 Storage (sift_col & sift_table)
              PostgreSQL Native Plan            • 2-level COW manifest + 8,192-row sorted granules
                                                • Adaptive 2-tier codecs (FOR/DELTA/RLE/ALP/FSST/ZSTD)
                                                • COW deletion vectors + O(1) [min_xid, max_xid] MVCC
```

- **Unified Format Major 2 Storage:** new `sift_col` indexes and `sift_table` tables share one immutable, CRC32C-verified format of 8,192-row granules, partitioned manifests, copy-on-write deletion vectors, and two-tier compression (fast L0 micro-parts for OLTP writes, compact L1/L2 parts for analytics). Relations created by 5.x stay on Format Major 1 with full read and write support, so upgrading needs no rebuild; `REINDEX INDEX CONCURRENTLY` (or `VACUUM FULL` / `sift.upgrade_table()` for a `sift_table`) moves one to Format 2.0 online.
- **Zero-Config Change Tracking:** a `sift_col` index follows deletes and updates by heap address (`track_changes = 'auto'`), so it works on any table: no primary key, a composite or `uuid` key, a partitioned table, or an index that omits the key.
- **2,048-Row Vectorized Execution:** Native fixed-width `NUMERIC`, `UUID`, inline-prefix strings, and zero-heap-I/O JSONB variant streams flow through 64-byte salted hash tables and 2-phase radix-partitioned parallel merges.
- **Strict O(1) Bounded Memory:** Every operation obeys PostgreSQL's `work_mem` / `maintenance_work_mem` plus two unified cache caps (`sift.backend_cache_kb = 16MB`, `sift.shared_cache_kb = 64MB`)—flat memory from 10k to 100M+ rows and hundreds of pooled backends. `sift.shared_memory()` and `sift.shared_memory_pools()` report all five shared pools.

## Quick Install

Supports PostgreSQL **17, 18, and 19** on Linux (`x86-64`, `arm64`) and macOS (`arm64`).

| Platform | Install |
|---|---|
| Debian 12/13, Ubuntu 22.04/24.04 | Download `.deb` from [Releases][rel], then `sudo apt install ./postgresql-N-pg-sift_*.deb` |
| RHEL/Rocky/Alma 9–10, AL2023 | Download `.rpm` from [Releases][rel], then `sudo dnf install ./pg_sift_N-*.rpm` |
| Signed APT / DNF repositories | See [docs/INSTALL-REPOS.md](docs/INSTALL-REPOS.md): `apt install postgresql-N-pg-sift` or `dnf install pg_sift_N` after one repository line |
| macOS / Arch / Docker | `brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git && brew install pg_sift` · AUR · [Docker](docs/CONTAINERS.md) |
| From source (PG 17–19 headers) | `make USE_PGXS=1 && sudo make USE_PGXS=1 install` |

[rel]: https://github.com/brauliobo/pg_sift/releases/latest

Enable in your database:

```sql
CREATE EXTENSION pg_sift;
-- Recommended for aggregate workloads (avoids first-query re-planning; no restart needed):
ALTER DATABASE mydb SET session_preload_libraries = 'pg_sift';
```

Try it in 5 minutes with Docker: [docs/QUICKSTART.md](docs/QUICKSTART.md).

## Documentation

| Guide / Reference | Covers |
|---|---|
| **Guides** | |
| [Quickstart](docs/QUICKSTART.md) | 5-minute Docker tour: 100K-row table, `sift_col`, `sift` BM25, `bench/bench kit` |
| [Columnar Analytics](docs/ANALYTICS.md) | `sift_col`, `key_order`, key marks, `PREWHERE`, `preagg`, JSONB `paths`, joins, `arg_max`, `ASOF`, HLL |
| [Scoped Analytics](docs/SCOPES.md) | Entity-Once `scope` for multi-tenant/hierarchy joins, Top-K rings, pair edits |
| [Native Column Tables](docs/TABLES.md) | `sift_table` creation, 10 options, online `key_order`, direct `COPY`, MVCC, projections, S3/R2 tiering, TTL |
| [BM25 Full-Text Search](docs/SEARCH.md) | `sift` indexes, `INCLUDE` pushdown, SiftQL, analyzers, field weights, facets, RLS |
| **Operations & Lifecycle** | |
| [Operations & Sizing](docs/OPERATIONS.md) | 3-budget O(1) memory, disk/WAL sizing, monitoring, alerts, emergency switches, quarantine |
| [Vulkan Compute](docs/GPU.md) | Opt-in `make VULKAN=1` device path for `SiftAgg` integer aggregates: `sift.gpu*` settings, eligibility, residency, EXPLAIN, safety |
| [Repositories](docs/INSTALL-REPOS.md) | Signed APT and DNF repositories for PostgreSQL 17-19 on x86-64 and arm64 |
| [Containers](docs/CONTAINERS.md) | Docker, Docker Compose, and CloudNativePG image-volume & operand images |
| [Upgrading](docs/UPGRADING.md) | 5.1 → 6.0 preflight, `sift.migrate_plan()`, full 5.x compatibility, optional online Format 2.0, downgrades |
| [Support](docs/SUPPORT.md) | PostgreSQL 17–19 & OS/arch compatibility matrix, versioning, and format policy |
| **Engineering & Reference** | |
| [Benchmarks](docs/BENCHMARKS.md) | Methodology, hardware, analytical/scope/memory/compression/BM25 suites, reproduction |
| [Comparison](docs/COMPARISON.md) | Honest feature matrices vs PostgreSQL, Citus, TimescaleDB, ClickHouse, DuckDB, ES |
| [Architecture](docs/ARCHITECTURE.md) | Format Major 2 layout, codecs, SIMD `SiftAgg`, planner, refusals, EXPLAIN labels, C safety |
| [Options & Tuning](docs/OPTIONS.md) | The 8 knobs worth touching, plus all 63 options: 27 GUCs, 26 reloptions, and 10 table options; everything strictly better is automatic |
| [SQL Reference](docs/REFERENCE.md) | Public `sift.*` functions, views, operator classes, and `@@>` operator |
| [Limits](docs/LIMITS.md) | Hard limits and lifted 6.0 ceilings across `sift_col`, `sift_table`, `SiftAgg`, and `sift` |
| [Messages](docs/MESSAGES.md) | Operator reference for every `NOTICE`, `WARNING`, and `INFO` message |

**Standalone Search Ports:** The exact BM25 search core is also available for [SQLite (`sqlite/README.md`)](sqlite/README.md) and [MariaDB (`mariadb/README.md`)](mariadb/README.md); the columnar analytics engine is PostgreSQL-only.

**Contributing, Security & License:** See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md). Licensed under the [PostgreSQL License](LICENSE) (third-party notices in [NOTICE](NOTICE)).
