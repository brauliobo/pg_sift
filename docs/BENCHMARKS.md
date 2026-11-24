# Benchmarks

Methodology, fairness rules and reproduction of every `pg_sift` number. Each figure checks **exact answer equality** (`md5_equal`) against stock PostgreSQL 18 before a timing counts. Summary and charts: [README](../README.md#benchmarks).

| Page | Measures |
|---|---|
| [Analytics](benchmarks/analytics.md) | 10M and 1M orders: `sift_col`, `sift_table`, ClickHouse (with and without `PROJECTION`), DuckDB, Elasticsearch, PG 18 |
| [ClickBench](benchmarks/clickbench.md) | The 43 queries `Q0`-`Q42` on `hits`, every arm `md5_equal` |
| [Search](benchmarks/search.md) | Exact BM25 against Elasticsearch, ParadeDB, `rum`, `pg_textsearch` and GIN; autocomplete |
| [Scopes](benchmarks/scopes.md) | Entity-Once scopes against stock PG, `pg_ivm` and ClickHouse: 72 cells, build and write cost |
| [Scalability](benchmarks/scalability.md) | Rows, workers, cold I/O, partitions, writers, opt-in Vulkan GPU |
| [Storage and memory](benchmarks/storage-memory.md) | Disk size, load, WAL, OLTP churn, flat memory to 10M+ rows |
| [Coverage](benchmarks/coverage.md) | Coverage matrix, plan gate, ClickHouse-parity gates `CH-01` to `CH-08` |

## Protocol

- **Warm** is the median of the timed runs after untimed warmups; **cold** is below. One engine runs at a time on equal container CPU and RAM budgets. Measured: wall latency, throughput, disk size, WAL bytes, peak `RssAnon`.
- **Baselines** are committed and dated: Ryzen 7 9700X (3 CPUs and 8 GiB per server, PG 18.6, `pg_sift` 3.5), Ryzen 9 5900XT (PG 18.7, `pg_sift` 5.0 scope suite) and the `5.1.0` wide + JSONB baseline (`bench/baselines/v510.tsv`). The header of each `docs/results/` file names its date and versions; a run rewrites the file and `scripts/gen-docs.py` regenerates every table and chart.
- **No number is typed.** A cell is a committed measurement or says it is not measured.

| Cold means | Emptied before each cold run | OS page cache |
|---|---|---|
| Orders (`store`, `duckdb`) | PG shared buffers; ClickHouse mark, block, index and query caches; Elasticsearch caches (index reopened) | Kept (server-memory-cold); dropped for the Parquet file |
| Articles, places | PG heap, TOAST and index buffers; Elasticsearch caches (index reopened) | Dropped (`posix_fadvise`), except on `tmpfs` |
| ClickBench | Each engine's own caches (PG evict or restart, ClickHouse `SYSTEM DROP ... CACHE`, DuckDB restart) | Dropped; the result file records `os_cache` |
| Scaling suite B | The relation leaves shared buffers | Dropped per segment; under 90% reads is `notcold` |

## Fairness rules

1. Same data, same host, equal limits; every engine loads from `bench/sql/` and runs through `bench/engines/`.
2. Exactness first: an arm whose rows differ from PostgreSQL's records no timing and fails the run.
3. Features are named, not hidden: `sift_col` pre-aggregates are compared with a plain ClickHouse `MergeTree` and with the `ClickHouse + PROJECTION` arm; point lookups run on the B-tree primary key.
4. Clocks differ and are stated: ClickHouse client wall clock (about 1 ms of HTTP) beside its server figure; PostgreSQL `psql` or `EXPLAIN (ANALYZE)` time; Elasticsearch `06` is approximate.
5. Rivals that win stay in the table and are marked amber in the charts.

## Reproduce

```sh
# 1. Self-contained 10-query orders comparison in Docker
bench/bench kit --rows 200000 --cpus 2 --runs 3
# 2. The whole 6.0 pass from the rival caches, then docs/results, docs/charts and the pages (--dry-run prints the plan)
bench/bench v6-release --docs
# 3. One suite: store all, duckdb, clickbench run, all, scope-stock, scaling-io, scaling, v6-core
bench/bench store all --docs
# 4. Regenerate README, pages and charts from docs/results/
python3 scripts/gen-docs.py
```
