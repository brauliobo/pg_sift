# Exact BM25 search

`sift` indexes against Elasticsearch 9.5, ParadeDB `pg_search`, PG `rum`, `pg_textsearch` and stock PostgreSQL GIN. BM25 parity with each engine is verified before timing. Summary: [README](../../README.md#benchmarks). Protocol: [BENCHMARKS.md](../BENCHMARKS.md).

## 50,000 Wikipedia articles: warm throughput (QPS, 8 clients)

Ryzen 7 9700X, `docs/results/articles.tsv` tier `slim` (Simple English Wikipedia), 2-3 interleaved rounds.

<!-- bench:v6-search-warm:begin -->
| Query Class | `pg_sift` (`sift`) | Elasticsearch 9.5 | ParadeDB (`pg_search`) | PG `rum` | `pg_textsearch` |
|---|---:|---:|---:|---:|---:|
| Exact `count(*)` | **29,127** | 28,105 | 2,399 | 229 | 161 |
| Phrase `"a b"` | **21,794** | 19,423 | 2,332 | 184 | 201 |
| Boolean `a AND b` | **21,690** | 17,476 | 2,414 | 4,909 | 172 |
| Mixed boolean | 14,159 | **14,749** | 2,248 | 112 | 147 |
| Filtered top-k | 12,604 | **13,854** | 2,241 | 113 | 163 |
| Boolean `a OR b` | 8,201 | **11,021** | 2,000 | 50 | 9,222 |
| Joined top-k\* | 6,689 | **14,037**\* | 431 | 110 | 189 |
<!-- bench:v6-search-warm:end -->

*\*Elasticsearch runs a term filter on a denormalized region field rather than a relational join. With `positions = off`, `pg_sift` reaches **24,146 QPS** on `a AND b` with a **12.0 MiB** index. On the full 218,058-article corpus (tier `full`) `pg_sift` reaches **10,279 QPS** (`AND`), **8,239 QPS** (`phrase`) and **6,976 QPS** (`count`) against ParadeDB's `2,679 / 2,520 / 1,501 QPS`.*

## 50,000 Wikipedia articles: cold p50 (ms)

Index, heap and TOAST leave shared buffers and the OS page cache before every query (`slim-cold`).

<!-- bench:v6-search-cold:begin -->
| Query Class | `pg_sift` (`sift`) | Elasticsearch 9.5 | ParadeDB (`pg_search`) | PG `rum` | `pg_textsearch` |
|---|---:|---:|---:|---:|---:|
| Exact `count(*)` | **1.03 ms** | 3.11 ms | 16.06 ms | 31.29 ms | 8.86 ms |
| Phrase `"a b"` | **1.31 ms** | 3.29 ms | 14.80 ms | 16.95 ms | 8.08 ms |
| Boolean `a AND b` | **1.76 ms** | 4.24 ms | 17.08 ms | 3.57 ms | 9.21 ms |
| Filtered top-k | **1.67 ms** | 3.88 ms | 15.90 ms | 17.30 ms | 9.01 ms |
| Boolean `a OR b` | 5.50 ms | **4.77 ms** | 14.54 ms | 56.90 ms | 7.02 ms |
| Joined top-k | **1.75 ms** | 3.67 ms | 13.12 ms | 17.97 ms | 8.84 ms |
<!-- bench:v6-search-cold:end -->

## Orders title search against PostgreSQL GIN

`docs/results/store-scaling.json` tier `10M-c3`, 3 CPUs, warm median; PG 18 uses `CREATE INDEX USING gin (to_tsvector('simple', title))`. The `5M` standard tier of `store` (`bench/bench store all --docs`) is not measured yet.

<!-- bench:v6-search-10m:begin -->
| Query (10M orders, `"wireless headphones"`) | `sift_col` + `sift` | `sift_table` + `sift` | Elasticsearch 9.5 | ClickHouse 26.9 | PG 18 GIN |
|---|---:|---:|---:|---:|---:|
| `08` Search `count(*)` | **2.35 ms** | 6.46 ms | 11.57 ms | 3.23 ms | 78.53 ms |
| `09` Search + filter + `GROUP BY category` | 13.69 ms | 11.70 ms | **8.27 ms** | 24.56 ms | 80.44 ms |
| `10` Search + filter + `ORDER BY ts DESC LIMIT 10` | 13.13 ms | **9.93 ms** | 13.63 ms | 17.73 ms | 141.68 ms |
<!-- bench:v6-search-10m:end -->

## Autocomplete

`docs/results/places.tsv` (34,189 GeoNames places, 1 client, top-20): `pg_sift` p50 **0.20-0.26 ms** across all 9 prefix and multi-word classes against Elasticsearch **0.22-0.46 ms**, ParadeDB **0.56-1.30 ms** and PG `rum` **0.44-28.07 ms**.
