# Storage, churn and bounded memory

On-disk size, load time, write cost and memory of `sift_table`, `sift_col`, `sift` and their rivals. Summary: [README](../../README.md#benchmarks). Protocol: [BENCHMARKS.md](../BENCHMARKS.md).

## On-disk size and load time (10M and 1M orders)

`docs/results/store-scaling.json` (`10M-c3`), `docs/results/store.json` (`1M-c3`), `docs/results/duckdb-10000000.tsv`.

<!-- bench:v6-size-orders:begin -->
| System | 10M Total Size | 10M Load | 1M Breakdown (Heap+PK / Col+Idx) | Notes |
|---|---:|---:|---:|---|
| DuckDB (`pg_duckdb` Parquet) | 297.3 MiB | 3.3 s | — | External file; no PK or BM25 |
| ClickHouse 26.9 | 421.4 MiB | 9.3 s | 0 / 37.6 MiB | MergeTree; no PK or BM25 |
| **`pg_sift` (`sift_table`)** | **456.6 MiB** | 28.4 s | 21.4 / 28.9 MiB (50.4 MiB) | **3.4× smaller** than PG 18; incl. PK + BM25 |
| Citus 14.2 columnar | 463.4 MiB | 13.2 s | 21.4 / 24.7 MiB (46.1 MiB) | Append-only; no BM25 |
| **`pg_sift` (`sift_col`)** | **1,437.5 MiB** | 16.1 s | 124.2 / 19.4 MiB (143.7 MiB) | Shares PG heap + PK; **7% smaller** than PG |
| Elasticsearch 9.5 | 1,456.9 MiB | 69.6 s | 0 / 112.1 MiB | Separate JVM cluster |
| PostgreSQL 18 (stock) | 1,549.1 MiB | 27.6 s | 124.2 / 34.6 MiB (158.8 MiB) | Heap + PK + B-trees + GIN |
<!-- bench:v6-size-orders:end -->

The `sift_table` load row is the committed baseline of the unbatched path; 6.0 loads through the direct-to-`COLD_L2` route (gate `CH-02`, [coverage.md](coverage.md#clickhouse-parity-gates)) and the 6.0 run replaces this row.

## BM25 index size and build time (50,000 Wikipedia articles)

`docs/results/articles.tsv` tier `slim`.

<!-- bench:v6-size-bm25:begin -->
| Engine | Index Size | Build Time | Table Growth | Notes |
|---|---:|---:|---:|---|
| **`pg_sift` (`positions=off`)** | **12.0 MiB** | **0.6 s** | 0.0 MiB | No phrase positions |
| **`pg_sift` (default)** | **30.2 MiB** | 0.7 s | 0.0 MiB | Full phrase + BM25 + filters |
| `pg_textsearch` 1.5 | 40.1 MiB | 5.1 s | 0.0 MiB | Timescale BM25 |
| ParadeDB (`pg_search` 0.26) | 54.5 MiB | 0.6 s | 0.0 MiB | Tantivy inside PG |
| Elasticsearch 9.5 | 70.5 MiB | 3.3 s | 0.0 MiB | +1.3 s force-merge (71.7 MiB pre-merge) |
| PG `rum` 1.3 | 85.7 MiB | 3.8 s | +100.3 MiB | Requires stored `tsvector` column |
<!-- bench:v6-size-bm25:end -->

## Wide + JSONB compression and write efficiency

13-column wide + 3-JSONB schema with `NUMERIC(12,2)`, `UUID` and a `2.1×` M:N scope (`5.1.0` measured in `round0-v510`, PG 18.7). `bench/bench v6-core --docs` fills the 6.0 columns.

<!-- bench:v6-wide-json:begin -->
13-column wide + 3-JSONB schema with `NUMERIC(12,2)`, `UUID`, and a `2.1×` M:N scope at 160,000 rows (`docs/results/v6-core.tsv`; 5.1 from `bench/baselines/v510.tsv`):

| Metric | PG 18 Heap | ClickHouse ZSTD | `pg_sift` v5.1 | `pg_sift` 6.0 Target | `pg_sift` 6.0 Measured |
|---|---:|---:|---:|---:|---:|
| Size, entity index | — | — | 57.5 B/row | — | *TBD* |
| Size, scoped index | — | — | 114.1 B/row | ≤ 33 B/row (≤ 16 excl. a 16 B UUID) | *TBD* |
| Heap + TOAST | — | — | 363.8 B/row | — | *TBD* |
| Page / extent utilization | 92–96% | ~99% | *TBD* | ≥ 95% | *TBD* |
| Bulk build WAL (`wal_level=replica`) | — | — | 111.8 B/row | ≤ 45 B/row | *TBD* |
| 1-row OLTP `INSERT` WAL | ~460 B/row | n/a | 495.7 B/row | ≤ 220 B/row | *TBD* |
| 2% `DELETE` WAL (incl. DV fold) | ~60 B/row | ~40 B/row | 98.7 B/row | ≤ 45 B/row | *TBD* |
| Steady-state compaction WA | — | 3.0–5.0× | 2.13× | ≤ 2.35× | *TBD* |
<!-- bench:v6-wide-json:end -->

## OLTP churn

`10%` `UPDATE`, `5%` `DELETE`, reads during churn, `VACUUM` and WAL bytes a row against PG 18 and ClickHouse (`bench/churn/`): not measured for 6.0 yet.

## Memory flatness (`10k` to `10M+` rows)

CI enforces three invariants across `10k`, `100k`, `1M` and `10M` rows (nightly `50M`):

- **Peak:** `peak_context_bytes <= configured_budget * 1.15`.
- **Slope (OLS `100k..10M`):** `slope <= 0.5 B/row`.
- **Idle retention:** `idle_RssAnon <= sift.backend_cache_kb * 1024 + 8 MB`.

<!-- bench:v6-memory:begin -->
Slopes are an OLS fit over the 0k–160k ladder (`docs/results/v6-core.tsv`; 5.1 from `bench/baselines/v510.tsv`):

| Workload | Budget | Slope gate | v5.1.0 Baseline | `pg_sift` 6.0 Measured |
|---|---|---:|---:|---:|
| Scoped `CIC` `RssAnon` slope | `maintenance_work_mem = 4MB` | ≤ 0.5 B/row | 470.8 B/row | *TBD* |
| Scoped `CIC` temp-file slope | `maintenance_work_mem = 4MB` | ≤ 0.5 B/row | 282.8 B/row | *TBD* |
| Scoped index size slope | — | — | 113.7 B/row | *TBD* |
| Scoped `CIC` WAL slope | — | — | 111.5 B/row | *TBD* |
| `VACUUM` `RssAnon` slope | `maintenance_work_mem = 4MB` | ≤ 0.5 B/row | 197.6 B/row | *TBD* |
| Idle pooled backends (34 indexes) | `backend_cache_kb` cap | ≤ cap + 8 MB | +8.3 MB | *TBD* |
<!-- bench:v6-memory:end -->
