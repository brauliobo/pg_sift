# Multi-tenant scoped analytics (`scope = ...`)

Entity-Once scope indexes against tuned stock PostgreSQL, `pg_ivm` and ClickHouse. Summary: [README](../../README.md#benchmarks). Protocol: [BENCHMARKS.md](../BENCHMARKS.md). Feature guide: [SCOPES.md](../SCOPES.md).

## Committed 5.0 baseline

Ryzen 9 5900XT, PG 18.7, `pg_sift` 5.0 suite (`docs/results/scope-stock.tsv`): 2,400,007 events, 2,648 members, 32 tenants, 9 query classes × 8 tenants = 72 cells, 700 of 700 answers `md5_equal`. A rival wins a cell when it is faster by more than 0.1 ms and 10%.

<!-- bench:v6-scope-baseline:begin -->
| Rival | Warm: win / tie / loss | Cold: win / tie / loss | Tenant 1 `sum_by_month` | Tenant 1 distinct |
|---|---:|---:|---:|---:|
| `stock_default` | **72 / 0 / 0** | **71 / 0 / 1** | 542 ms vs **2.18 ms** | 757 ms vs **1.52 ms** |
| `semijoin` | **63 / 2 / 7** | **62 / 1 / 9** | 285 ms vs **2.18 ms** | 636 ms vs **1.52 ms** |
| `denorm_flat` | **51 / 2 / 19** | **48 / 1 / 23** | 329 ms vs **2.18 ms** | 652 ms vs **1.52 ms** |
| `denorm_part` | **54 / 4 / 14** | **54 / 0 / 18** | — | — |
| `rollup` | **54 / 5 / 5** | **52 / 3 / 9** | — | — |
| `ivm` | **32 / 2 / 30** | **34 / 3 / 27** | 43.3 ms vs **2.18 ms** | 58.4 ms vs **1.52 ms** |

win / tie / loss: `sift_scope` faster / within 0.1 ms or 10% / slower. Where a rival is faster (warm): `denorm_flat`: `latest_20_rows`; `ivm`: `count_by_score`, `filtered_count`.
<!-- bench:v6-scope-baseline:end -->

`pg_ivm` beats `sift_scope` on pre-materialized exact-match views (`count_by_score` `0.22 ms` vs `0.38 ms`, `filtered_count` `0.09 ms` vs `4.54 ms`); `denorm_flat` wins `latest_20_rows` (`0.05 ms` vs `1.02 ms` in 5.0), which the 6.0 O(1) Top-K ring targets.

## Current run

`bench/bench scope-stock --docs` writes these blocks. The ClickHouse arms are `ch_join` (the join runs per query: a `PROJECTION` cannot hold a JOIN) and `ch_proj` (a denormalized copy with `PROJECTION`s, which pays the copy on every write); `ch_mv` is the materialized-view arm when run.

<!-- bench:scope-stock-tally:begin -->
*Not measured for 6.0 yet: `bench/bench scope-stock --docs` writes `docs/results/scope-stock.tsv` and populates this block.*
<!-- bench:scope-stock-tally:end -->

<!-- bench:scope-stock-geomean:begin -->
*Not measured for 6.0 yet: `bench/bench scope-stock --docs` writes `docs/results/scope-stock.tsv` and populates this block.*
<!-- bench:scope-stock-geomean:end -->

<!-- bench:scope-stock-costs:begin -->
*Not measured for 6.0 yet: `bench/bench scope-stock --docs` writes `docs/results/scope-stock.tsv` and populates this block.*
<!-- bench:scope-stock-costs:end -->

## Entity-Once 6.0 build and storage overhead (wide + JSONB, 2.1× M:N)

<!-- bench:v6-scope-build:begin -->
Entity-Once scope on the wide + JSONB schema at 160,000 rows (`docs/results/v6-core.tsv`; 5.1 from `bench/baselines/v510.tsv`):

| Metric | v5.1.0 Measured | `pg_sift` 6.0 Target | `pg_sift` 6.0 Measured |
|---|---:|---:|---:|
| Plain scoped `CREATE INDEX` wall time | 1.79× entity | ≤ 1.15× entity | *TBD* |
| Scoped `CIC` Pass 2 wall time | 107% of Pass 1 | ≤ 15% of Pass 1 | *TBD* |
| Scope build temp-file I/O | 249 B/row | **0 B** | *TBD* |
| Scope disk overhead vs entity | +98.3% | ≤ 5% (≤ 1% `cluster`) | *TBD* |
| Scoped whole-key read | 0.06 ms | O(1) (< 0.5 ms) | *TBD* |
| Scoped month read | 0.09 ms | O(1) (< 0.5 ms) | *TBD* |
| Scoped latest-20 read | 0.07 ms | O(1) (< 0.5 ms) | *TBD* |
<!-- bench:v6-scope-build:end -->
