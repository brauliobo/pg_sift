# Analytical aggregations

`sift_col` and `sift_table` against ClickHouse, DuckDB, Elasticsearch and PostgreSQL 18 on the `store` orders table. Summary: [README](../../README.md#benchmarks). Protocol and fairness rules: [BENCHMARKS.md](../BENCHMARKS.md).

## 10,000,000 orders: warm median

Ryzen 7 9700X, 3 CPUs and 8 GiB per server. `docs/results/store-scaling.json` tier `10M-c3` (PG 18.6, `pg_sift` 3.5 baseline, ClickHouse 26.9.19.9, Elasticsearch 9.5.5, Citus 14.2); `docs/results/duckdb-10000000.tsv` (`pg_duckdb` 1.1.1, DuckDB 1.4.3).

<!-- bench:v6-orders-10m:begin -->
| Query (10M orders) | `sift_col` | `sift_table` | ClickHouse | DuckDB Parquet | PG 18 |
|---|---:|---:|---:|---:|---:|
| `01` Point lookup (`WHERE id = 42`) | 0.27 ms | **0.20 ms** | 5.90 ms | 7.00 ms | 0.25 ms |
| `02` Count by country (`GROUP BY`) | **0.85 ms** | 1.65 ms | 9.32 ms | 8.97 ms | 188.9 ms |
| `03` Revenue / month (`sum GROUP BY`) | **0.93 ms** | 1.75 ms | 23.8 ms | 99.2 ms | 1,377 ms |
| `04` Top 10 categories (`LIMIT 10`) | **0.90 ms** | 2.09 ms | 21.0 ms | 26.7 ms | 370.2 ms |
| `05` Avg shipped order (3 filters) | 10.0 ms | 9.87 ms | **7.68 ms** | 48.9 ms | 124.2 ms |
| `06` Distinct customers / month | **49.3 ms** | 53.9 ms | 104.5 ms | 193.7 ms | 2,334 ms |
| `07` Big recent orders (range + `>100`) | 10.6 ms | 13.8 ms | **4.39 ms** | 39.7 ms | 146.4 ms |
<!-- bench:v6-orders-10m:end -->

- **Other engines, same run** (`02`-`07`): `pg_duckdb` on the heap `383 / 519 / 589 / 158 / 601 / 191 ms`; Elasticsearch 9.5 `6.0 / 69.7 / 68.0 / 28.9 / 260.1 (approx HLL++) / 6.3 ms`; Citus 14.2 columnar `709 / 1,183 / 946 / 603 / 4,814 / 381 ms`.
- **Where ClickHouse wins:** `05` average shipped order (`7.68 ms` vs `10.0 ms`) and `07` 30-day range count (`4.39 ms` vs `10.6 ms`). Composite key marks, adaptive `PREWHERE` and cold read-ahead target these two ([ANALYTICS.md](../ANALYTICS.md#composite-key-marks)); gate `CH-01` is in [coverage.md](coverage.md#clickhouse-parity-gates).
- **Not measured yet:** the `ClickHouse + PROJECTION` arm (`ch_proj`, `bench/sql/store/ch.sql` section `proj`: `p_by_month`, `p_by_cat`, `p_by_country`) and Elasticsearch at the `5M` standard tier. `bench/bench store all --docs` adds a column for each arm that has rows.

## 1,000,000 orders: warm / cold median

`docs/results/store.json` tier `1M-c3`. Cold empties the server's own caches before each query; the OS page cache stays.

<!-- bench:v6-orders-1m:begin -->
| Query (1M orders, warm / cold) | `sift_col` | `sift_table` | ClickHouse | Elasticsearch | PG 18 |
|---|---:|---:|---:|---:|---:|
| `02` Count by country | 0.56 / 9.89 | **0.54 / 8.51** | 4.95 / 10.7 | 6.00 / 125.2 | 22.0 / 34.7 |
| `03` Revenue / month | 0.65 / **2.90** | **0.61** / 3.26 | 6.91 / 7.18 | 13.7 / 215.0 | 192.5 / 197.7 |
| `04` Top 10 categories | 0.67 / **3.03** | **0.62** / 3.27 | 6.12 / 16.6 | 11.5 / 214.8 | 43.4 / 59.5 |
| `05` Avg shipped order | **1.92** / 16.9 | 2.10 / 18.0 | 7.01 / **6.57** | 5.68 / 182.1 | 10.6 / 25.8 |
| `06` Distinct customers / month | **6.16 / 10.9** | 8.71 / 14.0 | 14.0 / 10.9 | 45.9 / 441.3 | 293.4 / 224.4 |
| `07` Big recent orders | **2.05** / 7.79 | 2.07 / 9.95 | 3.66 / **3.77** | 3.38 / 89.5 | 12.0 / 28.3 |
<!-- bench:v6-orders-1m:end -->

## Fairness notes (read before quoting a table)

Every engine loads from `bench/sql/store/{sift,sift_table,ch,pg,citus}.sql` and is measured by `bench/engines/` and `bench/steps/duckdb.py`.

1. **`01` point lookup is the B-tree primary key**, not `sift_col`: both PostgreSQL columns run on it. ClickHouse uses a Bloom-filter skipping index on `id`; DuckDB scans a Parquet file sorted by `id`.
2. **`sift_col` has a precomputed month expression and pre-aggregates** (`03`, `06`); the ClickHouse arm is a plain `MergeTree` (`PARTITION BY toYYYYMM(created_at) ORDER BY (country, created_at, id)`) with no `PROJECTION`, materialized view or month column. The `03` gap is pre-aggregate against full scan; the like-for-like rows are `05` and `07`, where `pg_sift` does not win. `ch_proj` is the arm with matching projections.
3. **DuckDB reads Parquet through `pg_duckdb`**, written in `id` order with default row groups: statistics prune nothing for `country` or `created_at`. A native DuckDB CLI over a `(country, created_at)` file would be faster; this is the out-of-the-box PostgreSQL experience.
4. **ClickHouse time is the client wall clock** of one HTTP `POST` (about 1 ms of HTTP and parsing in every cell); its server figure (`elapsed_ns`) is recorded beside it. PostgreSQL arms time one `psql` statement.
5. **Elasticsearch `06` is approximate** (HyperLogLog++); `pg_sift` `06` is exact.
6. **Cold is server-memory-cold** for every engine on this suite.
