# ClickBench (`Q0`-`Q42`)

The 43 official queries in PostgreSQL dialect on the 105-column `hits` table (`bench/sql/clickbench/queries.sql`). Summary: [README](../../README.md#benchmarks). Protocol: [BENCHMARKS.md](../BENCHMARKS.md).

```sh
# 1. Run every arm; each query is verified md5_equal against PostgreSQL before any timing
bench/bench clickbench run
# 2. Write docs/results/clickbench.tsv into this page
bench/bench clickbench report --md docs/benchmarks/clickbench.md
```

- **Arms:** PG 18 (reference), `sift_table` (`skip_index = 'url:trigram, title:trigram'`), `sift_col` (`key_order` only), ClickHouse, `ClickHouse + PROJECTION` (`ch_proj`) and `pg_duckdb` / DuckDB. The `pg_sift` arms use the same projections as ClickHouse.
- **Exactness first:** rows are decoded from each engine's text format, `AVG` columns are rounded to 9 significant digits, everything else is exact, rows are sorted and the md5 must equal PostgreSQL's. An arm that differs records no timing and the run exits with status 1.
- **Fairness:** one list of projections (`p_by_adv`, `p_by_region`, `p_by_counter_day`, `p_by_user`) renders both `sift.create_projection(...)` and ClickHouse `ADD PROJECTION`; the result header names the projections the timed queries used. A `sift_col` index has at most 32 columns, so it covers the 25 columns the queries name and `SELECT *` (`Q23`) reads the heap. Every database uses the `C` locale.
- **Timing:** ClickHouse server time is `elapsed_ns` of `X-ClickHouse-Summary`, PostgreSQL's is `EXPLAIN (ANALYZE, TIMING OFF)` of a separate run, DuckDB's is `.timer`; each sits beside the client wall clock. Cold runs empty the engine's caches and the result file records `os_cache=dropped|kept`.
- **Data:** a deterministic synthetic generator (every query returns rows) and the real `hits.parquet` files (`--source parquet`).

## 5M standard tier

<!-- bench:clickbench-5m:begin -->
*Not measured for 6.0 yet: `bench/bench clickbench run` then `report --md` writes `docs/results/clickbench.tsv` and this block.*
<!-- bench:clickbench-5m:end -->

## 10M release tier

<!-- bench:clickbench-10m:begin -->
*Not measured for 6.0 yet: `bench/bench clickbench run` at the release tier, then `report --md`, writes this block.*
<!-- bench:clickbench-10m:end -->

A 20,000-row smoke on the installed 5.x build checked all 43 queries `md5_equal` on five arms; it is a correctness check, not a benchmark.
