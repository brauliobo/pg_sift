# Analytics benchmark (sift_col)

`bench/bench analytics` measures what the `sift_col` index is for: aggregate queries over a heap table whose columns are jsonb objects
with dynamic keys and arrays of objects, as the CPU time and instructions of one backend. It runs today against plain PostgreSQL
(the baseline) and measures the sift_col plan as soon as the build has the `sift_col` access method and an index of it exists: the DDL
of the index is a parameter. Nothing is written outside `SIFT_TMP`, nothing but the throwaway servers it starts is ever connected to.

```sh
export SIFT_TMP=/tmp/sift-col-bench SIFT_PORT=56170            # the work directory (tmpfs: the corpus needs about 3 GiB) and the port of the cluster
bench/bench analytics run                                      # 1.2M orders, queries S1-S7 x K10/K400/K4000 and G1-G8, baseline plan
bench/bench analytics run --flat --clickhouse                  # + flat PostgreSQL tables and a throwaway ClickHouse
bench/bench analytics run --queries S1,S2 --sets K400          # a part of the corpus
bench/bench analytics run --col-index @my_index.sql            # the sift_col DDL of the branch under test (default: bench/sql/analytics/col_index.sql)
bench/bench analytics run --scale 10 --runs 3                  # 12M orders: about 25 GiB, put SIFT_TMP on a disk
bench/bench analytics clickbench --rows 1000000                # the 43 queries of ClickBench on the first million rows of hits
bench/bench analytics report [DIR]                             # the table of a result directory again (default bench/out/latest)
bench/bench analytics clean [--all]                            # stop and remove the servers and the data (--all: also the sift build)
```

`bench/bench` holds `/tmp/sift-perf.lock` for `run` and `clickbench` (never wrap it in `flock`); `setup` (cluster, corpus, index) does not need it
and can be run first (`bench/bench analytics setup --flat --clickhouse`). Loaded data is kept until `clean`: a later `run` with the same
arguments loads nothing. The run builds sift from this checkout (`--no-build` skips it) so that the `sift_col` access method of the branch
is installed in the cluster.

## Corpus

An orders table of the usual e-commerce ORM shape, `bench/sql/analytics/schema.sql`: scalar columns (`store_id`, `customer_id`, `placed_at`
date, `status`, `currency`, `source`, `country_code`, `lang`, `total`, `discount`, shipping and payment method, timestamps, soft delete
in `deleted_at`) and four jsonb columns:

| column | shape |
|---|---|
| `attributes` | object: `locale`, `channel`, `device`, and `normalized: true` on the orders whose items already carry canonical names |
| `metrics` | object with dynamic keys and float values, 1 to 6 of 12 keys (`overall` always): `{"overall": 4.5, "delivery_speed": 3.0}` |
| `items` | array of objects `[{category, sku, kind, qty, price, discount}]`, 0 to 8 (mean 3.2) |
| `notes` | array of objects `[{type, author_role, text}]`, 0 to 3 |

and the mapping tables `metrics_map(source, metric_key, ...)`, `categories_map` and `kinds_map` that the queries join like an application does
(a key without a row falls back to the split of the key at the underscore; the items of the 30% of the orders that are not normalized
carry lower case categories and upper case kinds that the maps translate).

`bench/sql/analytics/gen.sql` generates it inside PostgreSQL from hashes of (order id, draw number), so the same arguments give the same rows.
Defaults and skews: 1.2M orders (`--scale S` or `--orders N`), 40,000 stores (`--stores`) with `store ~ u^2`, 30 sources, 12 languages,
60 countries (all `u^2`), dates 2022-01-01 to 2025-10-31 denser towards the end, 2% soft-deleted. `--cluster store` writes the heap
ordered by store and date instead of by order id. Prices are multiples of 0.25 and metrics multiples of 0.5, so every sum and average is exact
and no engine or order of summation can change a result.
`indexes.sql` adds what an application puts on such a table (primary key, btree on `(store_id, placed_at, deleted_at)` and `customer_id`, a
unique `number`, a GIN on `items`, a BRIN). The store sets are hash buckets: `K10` (about 10 stores), `K400` and `K4000`, each inside the next
(`analytics_sets`). The sets and the table are the same for every engine below.

## Queries

`bench/sql/analytics/queries/ID.sql`: a header (`-- sets:`, `-- key:` the result columns that sort the rows before they are compared), then a
section per layout, `--@ pg` (the table above), `--@ flat`, `--@ ch`. Macros: `{stores}` (the ids of the set), `{sources}`,
`{range(col)}` (two years: the current period 2024-11-01..2025-10-31 and the one before it), `{period(col)}` (`current`, `previous`).
Every S query filters by store set, sources, `deleted_at IS NULL` and the two periods, as the application does.

| id | what it does (`pg` section) |
|---|---|
| S1 | metrics summary per mapped metric, aspect and period: `jsonb_each(metrics)`, avg, count distinct orders, three `FILTER`ed distincts |
| S2 | item summary at three rollup levels (category + kind, category, all): per-order pre-aggregation of `jsonb_to_recordset(items)`, then average and distinct counts over orders; categories and kinds mapped |
| S3 | blend of S1 and S2: revenue of the items and the `overall` metric of their orders per mapped category and period |
| S4 | time series of orders, units and revenue at four granularities (day, week, month, quarter) over one expansion of the items |
| S5 | per-store rows: the top 3 categories of each store by revenue with their orders and customers (window function) |
| S6 | orders with a staff reply among `notes` (`jsonb_path_exists`) per country and language, with their total |
| S7 | orders with an item of a category, kind and price above a bound, the three conditions on the same element (`EXISTS` over the array) |
| G1-G8 | whole-table scans of the items (no store filter): filtered count, selective avg/sum, group by two low cardinality columns, count distinct, group by store, by month and language, percentiles, group by order |

S1-S7 run for each of K10, K400 and K4000 (21 measurements), G1-G8 once: 29 queries per engine. The `flat` and `ch` sections are the same
questions on the layouts below (S6 has no ClickHouse section: it needs the order row).

## Engines of the comparison

| engine | what runs | needs |
|---|---|---|
| `heap` | the `pg` text on `orders` with `--base-gucs` (default `sift.enable_agg=off`): the plan PostgreSQL has without sift_col | always |
| `col` | the same text with `--col-gucs` (`sift.enable_agg=on`) | an index of the access method `sift_col` on `orders` |
| `flat` | the `flat` text on `pg_items`, `pg_metrics`, `pg_notes` (one row per item, metric and note with the order's columns, btree on store and date): the best conventional PostgreSQL layout | `--flat` |
| `ch` | the `ch` text on a throwaway ClickHouse (`/usr/bin/clickhouse server`, own directory, ports `SIFT_PORT+10` and `+11`): `items_tuned` and `metrics_tuned` sorted by `store_id` first, `LowCardinality` strings, no `Nullable`, plain `MergeTree`, `Float32` | `--clickhouse` |

`heap` and `col` run the same SQL text on the same table; only the GUC differs, so the A/B is about the plan. The `col` engine is skipped with
a message when the table has no `sift_col` index (the baseline is measured alone), and the `col` plan is checked: each query's
`EXPLAIN (COSTS OFF)` is written to `plans.txt` and the result row `plan` says `sift_col` (the plan matches `--col-plan`, default `sift`), `same as heap`
(the plan did not change: the pushdown did not apply) or `other`.

`--col-index` takes the DDL (a statement, or `@FILE`; lines starting with `--` are dropped) and `--col-gucs` / `--base-gucs` the settings of the two
plans (`name=value` list; names the server does not know are left out with a message). The index is built once and again only when the DDL changes;
its build time and size are results (`col_build_s`, `index_mib`). `--col-am` and `A_COL_EXT` name another access method or extension.

## Method

One psql backend per run with `max_parallel_workers_per_gather = 0`. For each query and set: one warm-up run per engine (its result is kept and compared
with the `heap` result), then `--runs` (5) measured runs, the engines interleaved with the first one rotating, so a drift of the machine load hits all of
them. A run's **CPU time** is the on-CPU time of the backend read from `/proc/PID/schedstat` before and after the statement (planning included; it does
not depend on the load of the machine, which is shared and busy); **instructions** are the user-space instructions of that process from `perf stat -p`
(`bench/steps/perf.sh`, the count of `bench/steps/ab.sh` and `bench/cpu-query.sh` too; not measured, with a message, when `perf_event_paranoid` forbids it); the wall time is kept for reference. ClickHouse runs with one thread
(`max_threads = 1`): the CPU time is user plus system time of the query from `system.query_log`, the instructions are those of the server process
during the query. The value of a cell is the median of the runs, with minimum, maximum and standard error.

A result differs from the reference when a string or an integer differs, or a number by more than `--tol` (1e-6 relative), after sorting the rows by the
`key` columns of the query; empty strings and NULL are the same (ClickHouse has no NULL in the tuned layout). A difference or an error is the exit status 1
of the run and a `DIFF` or `error` in the table.

## Results

`bench/out/run-<time>/` (the layout of the other tiers): `analytics.json` with `meta` and `rows` in the layout of `docs/results/*.tsv`
(`dataset tier engine class variant metric value min max unit stderr n values`; `class` is the query, `variant` the set or `-`; metrics `cpu_ms`, `wall_ms`,
`instr_m`, `rows`, `result`, `plan`, and for the corpus `orders`, `items`, `metrics`, `load_s`, `table_mib`, `indexes_mib`, `col_build_s`, `index_mib`),
`rows.jsonl` (the same, written as the run goes), `plans.txt`, `machine.txt` (`bench/bench machine` and the corpus), `bench.log`.
`bench/bench analytics report` prints the table again: CPU time per engine and the ratios of the baseline over each other engine (above 1 the other
engine is cheaper), the ratio of instructions of `col`, the check and the plan.

## ClickBench

`bench/bench analytics clickbench` runs the 43 queries of [ClickBench](https://github.com/ClickHouse/ClickBench) (`postgresql/queries.sql`, pinned to
a commit and a sha256 in `bench/steps/analytics.sh`) on the first `--rows` (10,000,000) rows of the `hits` table: one parquet file of the public
`hits_compatible` dataset per million rows is downloaded into a directory of its own under `$BENCH_STORE` (default `SIFT_TMP`), converted by
`clickhouse local` and copied into the table of ClickBench's `postgresql/create.sql`, then deleted (`KEEP_DOWNLOADS=1` keeps it). It is explicit
because it downloads about 175 MB and needs about 800 bytes of table per row (8 GB for the default; point `BENCH_STORE` and `SIFT_TMP` at a disk).
The engines are `heap` and `col` (`--col-index` takes the DDL on `hits`; there is none by default). Queries with `ORDER BY ... LIMIT` over ties can differ between plans
without being wrong: read a `DIFF` of those with the plan.

Baseline (`docs/results/clickbench.json`): PostgreSQL 18.6 with no index, 1,000,000 rows of `hits` (706 MiB), the same host as above (load average 14), one
backend, 3 runs; the median relative standard error is 1.9% and the worst 17% (Q8). The default of 10,000,000 rows was not run on this host, where the
work directory is RAM; `heap Minstr` is millions of user-space instructions.

| query | set | heap cpu ms | heap Minstr | result |
|---|---|--:|--:|---|
| Q1 | - | 64.8 | 464 | ok |
| Q2 | - | 234.8 | 1829 | ok |
| Q3 | - | 247.4 | 2019 | ok |
| Q4 | - | 142.7 | 912 | ok |
| Q5 | - | 225.9 | 1587 | ok |
| Q6 | - | 431.1 | 2718 | ok |
| Q7 | - | 206.9 | 859 | ok |
| Q8 | - | 242.0 | 1837 | ok |
| Q9 | - | 316.6 | 3825 | ok |
| Q10 | - | 451.6 | 5221 | ok |
| Q11 | - | 301.5 | 1879 | ok |
| Q12 | - | 256.0 | 1945 | ok |
| Q13 | - | 280.1 | 2012 | ok |
| Q14 | - | 276.1 | 2368 | ok |
| Q15 | - | 262.0 | 2029 | ok |
| Q16 | - | 224.8 | 1529 | ok |
| Q17 | - | 425.3 | 2914 | ok |
| Q18 | - | 318.3 | 2766 | ok |
| Q19 | - | 480.5 | 4921 | ok |
| Q20 | - | 138.2 | 842 | ok |
| Q21 | - | 255.0 | 2888 | ok |
| Q22 | - | 331.3 | 3712 | ok |
| Q23 | - | 391.6 | 4169 | ok |
| Q24 | - | 259.7 | 2888 | ok |
| Q25 | - | 340.9 | 1965 | ok |
| Q26 | - | 315.7 | 1940 | ok |
| Q27 | - | 336.5 | 1965 | ok |
| Q28 | - | 388.1 | 3699 | ok |
| Q29 | - | 3057.6 | 51163 | ok |
| Q30 | - | 1028.4 | 8635 | ok |
| Q31 | - | 390.1 | 2057 | ok |
| Q32 | - | 416.9 | 2223 | ok |
| Q33 | - | 1136.0 | 7436 | ok |
| Q34 | - | 409.5 | 2528 | ok |
| Q35 | - | 396.9 | 2557 | ok |
| Q36 | - | 256.6 | 2229 | ok |
| Q37 | - | 421.1 | 3464 | ok |
| Q38 | - | 412.2 | 3186 | ok |
| Q39 | - | 294.9 | 2415 | ok |
| Q40 | - | 423.2 | 3153 | ok |
| Q41 | - | 411.2 | 4221 | ok |
| Q42 | - | 523.3 | 3998 | ok |
| Q43 | - | 350.4 | 3108 | ok |

## Footprint and cost

Generating and loading 1.2M orders takes about 50 s; the data directory is 2.5 GiB on tmpfs with the flat tables (the table 997 MiB, its indexes
169 MiB, `pg_items` 547 MiB, `pg_metrics` 416 MiB) and ClickHouse another 240 MiB, so keep `SIFT_TMP` on a disk beyond `--scale 2`. The
`run` itself took 9 minutes of wall time for the three engines at 5 runs. The host is shared: the CPU time of a run is
what the backend was on the CPU, but the load average (printed in `machine.txt`) still moves it a few percent through caches and SMT; the median
relative standard error of the cells below is 3.4%, the worst 9.7%.

## Baseline on the generated data

`docs/results/analytics.json`, measured without the sift_col plan (`col` is absent: the build had no access method): PostgreSQL 18.6 on an AMD Ryzen 7 9700X
(8 cores, 188 GiB), 1.2M orders, 3,753,480 items, 4,201,756 metrics, 40,000 stores, tmpfs, one backend, 5 runs, ClickHouse 26.8 with one thread. Every
`flat` and `ch` result equals the `heap` result (`ok`). `heap/X` is the CPU time of the heap query over the engine's: above 1 the engine is cheaper
than the application's query on the heap table. `heap Minstr` is the user-space instructions of the `heap` query in millions.

| query | set | heap cpu ms | flat cpu ms | ch cpu ms | heap Minstr | heap/flat | heap/ch | result |
|---|---|--:|--:|--:|--:|--:|--:|---|
| S1 | K10 | 3.9 | 2.5 | 13.5 | 27 | 1.53 | 0.29 | ok |
| S1 | K400 | 119.4 | 80.0 | 88.9 | 1063 | 1.49 | 1.34 | ok |
| S1 | K4000 | 1453.8 | 992.7 | 208.5 | 12378 | 1.46 | 6.97 | ok |
| S2 | K10 | 6.1 | 4.8 | 47.4 | 47 | 1.26 | 0.13 | ok |
| S2 | K400 | 215.6 | 204.4 | 257.7 | 1898 | 1.05 | 0.84 | ok |
| S2 | K4000 | 2743.7 | 2464.6 | 805.6 | 21252 | 1.11 | 3.41 | ok |
| S3 | K10 | 2.9 | 1.9 | 11.1 | 19 | 1.49 | 0.26 | ok |
| S3 | K400 | 94.2 | 59.9 | 73.4 | 749 | 1.57 | 1.28 | ok |
| S3 | K4000 | 1198.9 | 810.5 | 169.4 | 8817 | 1.48 | 7.08 | ok |
| S4 | K10 | 3.5 | 2.9 | 39.1 | 25 | 1.23 | 0.09 | ok |
| S4 | K400 | 91.8 | 77.8 | 200.6 | 819 | 1.18 | 0.46 | ok |
| S4 | K4000 | 1134.9 | 909.2 | 476.1 | 9069 | 1.25 | 2.38 | ok |
| S5 | K10 | 2.7 | 1.9 | 14.8 | 17 | 1.41 | 0.18 | ok |
| S5 | K400 | 59.5 | 44.2 | 80.9 | 513 | 1.34 | 0.74 | ok |
| S5 | K4000 | 625.1 | 426.8 | 161.9 | 5491 | 1.46 | 3.86 | ok |
| S6 | K10 | 1.2 | 2.1 | NA | 5 | 0.58 | NA | ok |
| S6 | K400 | 24.4 | 44.5 | NA | 129 | 0.55 | NA | ok |
| S6 | K4000 | 208.4 | 517.0 | NA | 1569 | 0.40 | NA | ok |
| S7 | K10 | 1.6 | 0.9 | 6.2 | 7 | 1.66 | 0.25 | ok |
| S7 | K400 | 33.5 | 17.6 | 29.6 | 234 | 1.90 | 1.13 | ok |
| S7 | K4000 | 323.7 | 125.0 | 84.0 | 2670 | 2.59 | 3.86 | ok |
| G1 | - | 2571.4 | 360.5 | 6.4 | 16431 | 7.13 | 399.90 | ok |
| G2 | - | 970.6 | 378.5 | 27.9 | 6132 | 2.56 | 34.78 | ok |
| G3 | - | 3180.3 | 823.0 | 146.4 | 28363 | 3.86 | 21.73 | ok |
| G4 | - | 1454.9 | 630.2 | 73.3 | 13171 | 2.31 | 19.84 | ok |
| G5 | - | 2045.8 | 239.8 | 14.6 | 14671 | 8.53 | 140.28 | ok |
| G6 | - | 2871.1 | 1246.6 | 80.0 | 26712 | 2.30 | 35.90 | ok |
| G7 | - | 4458.7 | 2433.2 | 93.0 | 31743 | 1.83 | 47.94 | ok |
| G8 | - | 1917.4 | 885.0 | 79.8 | 14895 | 2.17 | 24.04 | ok |

cpu ms: median over the runs of the on-CPU time of one backend (ClickHouse: one query thread); heap/X: heap over engine X, above 1 X is cheaper

What it shows, for the sift_col plan to beat:

- The heap queries cost 1.05 to 2.6x the flat PostgreSQL tables on S1-S5 and S7 (the expansion of the jsonb is most of the cost), 1.8 to 8.5x on the whole-table scans
  and 0.4 to 0.6x on S6 (`jsonb_path_exists` over the `notes` column is cheaper than the semijoin with a child table).
- A tuned ClickHouse is slower than the heap query on the small sets (K10: its fixed cost is about 10 ms of CPU, 40 to 50 ms for the rollup queries S2 and S4) and between 0.5 and 1.3x
  on K400, but 2.4 to 7.1x cheaper on K4000 and 20 to 400x on the whole-table scans (G1 is a 400x count: ClickHouse reads one `LowCardinality` column).
- A query of the K10 set is 1 to 6 ms of CPU on the heap, K400 25 to 215 ms, K4000 0.2 to 2.7 s, the scans 1 to 4.5 s: the columnar plan has to win on K4000 and on the scans,
  and not to lose on K10, where the baseline is already a few ms.
