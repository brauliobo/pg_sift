# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml) [![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml) [![release](https://img.shields.io/badge/release-3.2.0-blue)](https://github.com/brauliobo/pg_sift/releases/latest) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](#install) [![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

pg_sift adds fast full-text search and column-store analytics to PostgreSQL with one `CREATE EXTENSION pg_sift`. No external search
server, no separate analytics database, no `shared_preload_libraries`. The search engine also runs as a SQLite extension and a MariaDB library.

[Features](#features) · [Install](#install) · [Homebrew](#install-with-homebrew) · [Search](#search) · [Analytics](#analytics) · [Benchmarks](#benchmarks) · [Compared with](#compared-with) · [What is covered](#what-is-covered) · [SQLite](#sqlite) · [MariaDB](#mariadb) · [Operations](#operations) · [Limits](#limits) · [Vulkan (experimental)](#vulkan-experimental) · [Docs](#docs)

## Features

- **BM25 search index** (`USING sift`): boolean, phrase, prefix, wildcard, regex and fuzzy queries, exact BM25 ranking, top-k with filters applied inside the scan, counts from the index, facets, autocomplete.
- **Column engine** (`USING sift_col`, `USING sift_table`): `count`, `count(DISTINCT ...)`, `sum`, `avg`, `min`, `max` with filters and `GROUP BY` run on compressed columns as `Custom Scan (SiftAgg)`; `sift_table` stores the table itself in column format.
- **Vulkan (experimental, opt-in, 3.1):** `make VULKAN=1` and `SET sift.gpu = auto|on` run grouped and filtered integer counts and sums of a `sift_col` index on a GPU; off by default, the CPU path unchanged ([Vulkan](#vulkan-experimental), [docs/GPU.md](docs/GPU.md)).
- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, generic WAL (crash and replica safe), `pg_dump`; a query the planner cannot prove equal keeps its normal plan.
- **Ports:** [SQLite](#sqlite) and [MariaDB](#mariadb) run the same search core.

## Install

Supports PostgreSQL 17 and 18 on Linux and macOS. Linux packages for Arch (AUR), Debian, Ubuntu and RHEL-family systems: [docs/USAGE.md](docs/USAGE.md#install).

### Install with Homebrew

On macOS, the [formula](Formula/pg_sift.rb) in this repository builds for Homebrew PostgreSQL 17 and 18:

```sh
brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git
brew install --HEAD brauliobo/pg_sift/pg_sift
brew test brauliobo/pg_sift/pg_sift
```

The HEAD-only formula builds `main` and supports macOS arm64. Tested on Apple Silicon with PostgreSQL 17.11 and 18.6.
With your PostgreSQL server running, connect as a superuser:

```sh
"$(brew --prefix postgresql@18)/bin/psql" -d mydb -c 'CREATE EXTENSION pg_sift;'
```

Replace `mydb` with your database name; use `postgresql@17` for a PostgreSQL 17 server.

### Build from source

Needs server headers with `pg_config` on `PATH` and a C compiler.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
psql -d mydb -c 'CREATE EXTENSION pg_sift'          # as a superuser; after an upgrade: ALTER EXTENSION pg_sift UPDATE
```

## Search

```sql
CREATE TABLE docs (id int PRIMARY KEY, title text, body text, published date, category int);
INSERT INTO docs VALUES
  (1, 'Wireless headphones', 'Battery was great and the support was friendly. The case was squeaky.', '2026-01-10', 1),
  (2, 'Charger', 'Friendly support, no battery, but a sturdy case and clip.', '2026-02-11', 1),
  (3, 'Resume tips', 'A good résumé is short. Cafés near the office help.', '2026-03-12', 2),
  (4, 'Running', 'Runners were running; she runs every day.', '2026-04-13', 2),
  (5, 'Squeaky headphones', 'The headphones were squeaky at night, squeak everywhere. Friendly support though.', '2026-05-14', 1);
CREATE INDEX docs_body ON docs USING sift (body);

SELECT id FROM docs WHERE body @@> 'battery friendly';                     -- all terms: 1, 2
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'case OR clip' ORDER BY sift.score(ctid) DESC LIMIT 10;   -- BM25 top-10
SELECT count(*) FROM docs WHERE body @@> 'friendly';                        -- 3, from the index
```

Words are `AND`ed; `OR`, `NOT`, `"phrases"`, `batt*`, `sq?eaky`, `/regex/`, `frendly~1` and `^3` boosts combine them: [docs/USAGE.md](docs/USAGE.md#queries).

## Analytics

```sql
CREATE TABLE orders (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), created_at timestamp);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, created_at);

SELECT category, count(*), sum(amount) FROM orders
WHERE country = 'DE' AND created_at >= '2025-06-01' GROUP BY category;          -- Custom Scan (SiftAgg) on orders_col
CREATE TABLE events (id bigint, kind text, price numeric(10,2), at timestamp) USING sift_table;   -- the table itself in column format
```

`SET sift.enable_agg = off` gives the plan PostgreSQL has without sift. Types, options and limits: [docs/USAGE.md](docs/USAGE.md#column-indexes-aggregates-and-sift_table).

## Benchmarks

<!-- bench:summary:begin (generated by `bench/bench readme` from docs/results; do not edit by hand) -->
- **Fast:** ahead of ParadeDB pg_search on every query class we measured, 2.4x to 7.2x on 50,000 Wikipedia articles ([benchmarks](#benchmarks)).
- **Small:** 30 MiB for those articles with phrase support, 1.4x smaller than pg_search; 12 MiB without word positions.
<!-- bench:summary:end -->

### Search

<!-- bench:readme:begin (generated by `bench/bench readme` from docs/results; do not edit by hand) -->
Simple English Wikipedia (50,000 articles, 8 clients) and GeoNames cities15000 (34,165 places, autocomplete, 1 client): the slim tier of the suite, the default of `bench/bench all`.

- **Scope:** the engines, query classes and workloads of the full tier on a smaller corpus; cells stop at a 5% relative standard error (at most 1 s).
- **Method:** same data, queries and clients for every engine, one engine at a time, median of 2-3 interleaved rounds.
- **CPUs:** the same budget of 3 CPUs for the server of every engine:
  - a cgroup quota on the server processes only, on four physical cores
  - the clients, pgbench and the load generator, run outside it on the other four
  - PostgreSQL engines with one parallel worker fewer than the budget, Elasticsearch with that many processors and shards
- **Host:** measured 2026-10-20 on AMD Ryzen 7 9700X (16 threads), PostgreSQL 18.6.
- **Cells:** queries per second (higher is better) and, for the places, the p50 latency of the top-20 workload in milliseconds (lower is better); the fastest engines of a column (all within one standard error of the best) are in bold; a `±` is one standard error where it exceeds 10% of the value.

| articles (queries/s) | index | build | and | or | phrase | mixed | `count(*)` | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **sift** (default) | 30 MiB | 1.4 s | **13,400** | 4,000 | **11,700** | **7,380** | **14,400** | **6,050** | 3,100 |
| sift `positions = off` | 12 MiB | 0.9 s | **13,700** | 3,920 | 2,790 | 4,850 | 3,450 | 5,200 | 2,020 |
| Elasticsearch | 72 MiB | 5.9 s | 5,140 ± 2,400 | 2,770 ± 1,000 | 8,450 ± 1,500 | 5,880 ± 810 | 8,880 ± 2,700 | 5,420 ± 810 | **5,610** |
| pg_search 0.26.1 | 41 MiB | 2.3 s | 2,340 | 1,700 | 2,280 | 2,050 | 2,290 | 1,990 | 430 |
| pg_textsearch 1.5.1 | 40 MiB | 8.0 s | 72.7 ± 14 | **4,210** | 88.7 ± 14 | 100 ± 25 | 131 ± 57 | 78.6 ± 15 | 97.1 ± 24 |
| rum 1.3.15 | 86 MiB | 5.1 s | 2,610 | 22.8 ± 3.3 | 73.3 | 51.4 ± 5.3 | 101 | 48.7 | 46.9 ± 5.3 |
| pgx-bm25 | 54 MiB | 3.6 s | 93 | 73.9 | 54 | 57 ± 6.2 | 55.9 | 61.3 | 61.8 ± 8 |

| places (p50 ms, top 20) | p1 | p2 | p3 | p4 | w1 | a2 | a2s | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **sift** | **0.17** | **0.199** | **0.162** | 0.173 | **0.151** | 0.169 | **0.162** | 0.185 | **0.161** |
| sift `positions = off` | **0.175** | 0.202 | 0.17 | **0.161** | **0.151** | **0.163** | **0.161** | **0.183** | 0.168 |
| Elasticsearch | 0.301 | 0.278 | 0.252 | 0.215 | 0.182 | 0.18 | 0.18 | 0.254 | 0.199 |
| pg_search 0.26.1 | 0.5 | 0.565 | 0.598 | 0.628 | 0.697 | 0.839 | 0.783 | 1.25 | 0.941 |
| pg_textsearch 1.5.1 | 186 ± 270 | 84.1 | 13.3 ± 3.1 | 3.17 | 2.67 | 2.66 | 2.72 | 2.74 | 2.72 |
| rum 1.3.15 | 19.8 | 4.42 | 2.58 | 0.398 | 0.244 | 4.03 | 10.1 | 3.69 ± 0.48 | 2.7 |
| pgx-bm25 | 4.59 | 1.15 | 36.6 ± 11 | 3.08 | 1.5 | 2.22 | 4.44 | 5.15 | 3.74 |

- Autocomplete on 34,165 places: sift answers the top 20 in 0.15-0.2 ms in every query class, Elasticsearch in 0.18-0.3 ms, pg_search in 0.5-1.2 ms; sift is ahead of pg_search in 36 of 36 warm cells (1.9x to 12x).
- `positions = off` shrinks the index to 12 MiB (3.4x smaller than pg_search's 41 MiB, the default sift index is 30 MiB); phrase runs at 2,790 queries/s and `count(*)` at 3,450 (pg_search: 2,280 and 2,290), against 11,700 and 14,400 with positions.
- Prefix ranking agrees less with a BM25-over-n-grams oracle than pg_search's does, because sift scores a prefix as the best word it expands to ([correctness](docs/BENCHMARKS.md#correctness)).
- The host was shared (load average 4-14 during the cells, median 10), so absolute numbers are noisy and ratios close to each other are not distinguishable.
- Every cell with its standard error, the cold-cache tables and the method: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

![Throughput by workload](docs/img/articles-throughput.svg)
![Index size](docs/img/index-size.svg)
![Autocomplete latency](docs/img/places-latency.svg)

Reproduce:

- `bench/bench all` (see [bench/README.md](bench/README.md)): about 11 minutes of builds and measurements for the slim tier, about 14 minutes with the cold-cache tables of `--docs`.
- `bench/bench all --docs` rewrites this section from a new session.
- `--full` runs the full-size tier (every article of the dump, GeoNames cities500, about an hour); its last numbers are kept in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) as history.
- `bench/bench regress --run` compares this checkout with an earlier release and exits non-zero on a regression.
<!-- bench:readme:end -->

### Analytics

Each row is a query on the `orders` table; `month` is `date_trunc('month', created_at)` and `...` stands for the other filters of the query (the full queries are in `bench/sql/store/queries/`).

<!-- bench:store:begin (generated by `bench/bench store`; do not edit by hand) -->
An online store: a table of 1,000,000 orders (country, category, status, amount, date, a product title) and ten questions anyone can ask of it. Every system holds the same seeded rows, runs one node in a docker container limited to 3 CPUs (a quota, no pinning), the same 3 CPUs per server as the search tables, and 8g of memory and the same thread budget for every engine (a [fair comparison](docs/BENCHMARKS.md#fair-comparison-the-same-threads-for-every-engine)), and answers over a local connection from one client. Cells are the median of 5 warm runs in milliseconds (lower is better; the fastest of a row, and anything within 5% of it, in bold). Measured 2026-10-20 on AMD Ryzen 7 9700X 8-Core Processor (16 threads), PostgreSQL 18.6 (Debian 18.6-1.pgdg13+2), pg_sift 3.1, Citus 14.2-1 columnar, Elasticsearch 9.5.5, ClickHouse 26.9.19.9.

**3 CPUs per container (PostgreSQL, pg_sift and Citus: a leader and 2 parallel workers; Elasticsearch: `node.processors` 3; ClickHouse: `max_threads` 3)**

| query | PostgreSQL | pg_sift `sift_col` | pg_sift `sift_table` | Elasticsearch | ClickHouse | Citus columnar |
|---|---:|---:|---:|---:|---:|---:|
| `* WHERE id = 42` | 0.238 | 0.212 | **0.158** | 1.49 | 2.95 | 1.79 |
| `count(*) GROUP BY country` | 22.7 | 0.815 | **0.641** | 5.82 | 4.62 | 72.5 |
| `sum(amount) GROUP BY month` | 197 | **0.797** | 4.81 | 9.78 | 16.6 | 121 |
| `sum(amount) GROUP BY category` top 10 | 41.3 | 0.852 | **0.717** | 9.93 | 6.05 | 101 |
| `avg(amount) WHERE status='shipped', ...` | 10.5 | **1.96** | 3.53 | 3.82 | 5.88 | 62.6 |
| `count(DISTINCT customer_id) GROUP BY month` | 217 | **5.21** | 6.73 | 35.9 | 9.89 | 466 |
| `count(*) WHERE amount > 100, created_at >= ...` | 12.2 | **1.46** | 1.8 | 3.49 | 3.36 | 39.6 |
| `count(*) WHERE title @@> 'a b'` | 7.42 | **0.613** | 1.08 | 3.18 | 3.28 | 865 |
| `count(*) WHERE title @@> 'a b', ... GROUP BY category` | 7.9 | 1.94 | **1.61** | 4.68 | 10.1 | 398 |
| `title @@> 'a b', ... ORDER BY created_at DESC LIMIT 10` | 65.6 | 4.6 | **2.65** | 6.32 | 7.47 | 525 |
| load time (data, indexes, merge) | 2.4 s | 1.9 s | 4.1 s | 12 s | **0.8 s** | 1.3 s |
| size: heap and primary key | 124.2 MiB | 124.2 MiB | 21.4 MiB | n/a | n/a | 21.4 MiB |
| size: index or columnar data (without heap and primary key) | 34.6 MiB (6.8 + 21.3 + 6.5) | **18.8 MiB (10.9 + 7.9)** | 21.0 MiB (13.2 + 7.8) | 111.7 MiB | 37.6 MiB | 24.7 MiB |
| size on disk, total (free blocks inside the file in parentheses) | 159 MiB | 143 MiB | 193 MiB (150 free, 42 live) | 112 MiB | **38 MiB** | 46 MiB |

- **The size rows.** *Heap and primary key* is what the table itself costs: the heap and the primary key btree of PostgreSQL and `sift_col`; `sift_table` and Citus hold the rows in their own format, so it is the primary key alone, and Elasticsearch and ClickHouse have no such part. *Index or columnar data* is everything else: PostgreSQL its secondary indexes (the btrees on the date and on country and status, the GIN on the title; 34.6 MiB); `sift_col` the column index and the BM25 title index (10.9 + 7.9 MiB); `sift_table` the live bytes of its columns plus the title index (13.2 + 7.8 MiB); Elasticsearch the index; ClickHouse the table with its text index; Citus the columnar table. pg_sift `sift_table`, Elasticsearch, ClickHouse and Citus columnar store the data themselves, so there that row is the data, not an index in the PostgreSQL sense. The total is the file sizes (the parts add up to it within rounding); the free blocks of `sift_table` are inside its file after the load. Like for like, the seven columns of the `sift_col` index (the table without the title) take 9.6 MiB against 14.6 MiB for ClickHouse with its default `LZ4`, 10.4 MiB with `ZSTD(3)` and 9.3 MiB with a hand-tuned schema (`system.columns`, measured on 2026-10-18; per column in [docs/BENCHMARKS.md](docs/BENCHMARKS.md#size-against-clickhouse-1000000-orders)).
- Every result is compared with PostgreSQL's before it is timed, and the run stops on a difference. Elasticsearch counts distinct customers approximately (HyperLogLog++: its monthly counts were off by at most 0.02% here); every other result matched exactly or within rounding.
- Elasticsearch runs with its request cache off (a repeated aggregation would otherwise be answered from memory); with it on, the medians are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md#analytics-an-online-store).
- ClickHouse is a column store built for exactly these aggregations and Elasticsearch for exactly this search; PostgreSQL (heap, btree and GIN indexes) is what most teams run today.
- Citus 14.2-1 (PostgreSQL 18.4) is measured as one node with the table in its `columnar` access method and its default settings (chunk_group_row_limit=10000, stripe_row_limit=150000, compression=zstd, compression_level=3), a primary key and no other index (columnar tables take btree and hash indexes only, so the three searches scan the title column). Columnar tables are append-only, so Citus is not in the update table.
- pg_sift was the fastest system, or within 5% of it, on every question.
- **10,000,000 orders were not measured for the 3.1.0 pass** (`bench/bench final --size standard` measures this size only; `--size full` adds 10,000,000). The last 10,000,000 run (history), on 2026-10-18 with a 3.0 development build, is in the git history of `docs/results/store.json`: there the data no longer fit the cache, ClickHouse was faster than pg_sift `sift_col` on 7 of the 10 questions (one of them a tie; the top 10 categories 17x, the distinct customers per month 4.0x) and `sift_table` failed on the two search questions with facets and the newest orders (a later check of the 3.0 build at 10,000,000 orders loaded in 1,000,000-row transactions answered all ten questions as PostgreSQL does). Do not read the 1,000,000-order ranking above as the ranking at ten times the size.

Reproduce with `bench/bench store all` (docker; `--docs` rewrites this section and docs/BENCHMARKS.md from the new numbers); the data generator, the table layout of every system and the ten queries are in `bench/sql/store/`.
<!-- bench:store:end -->

#### Updates and deletes

<!-- bench:store-updates:begin (generated by `bench/bench store`; do not edit by hand) -->
On the 1,000,000-order table, after the read tests, each system changes its rows the way its documentation recommends for the job (below); the state after each step is compared with PostgreSQL's. Query: revenue per month, timed while a second connection updates rows at the rate shown. Medians; the best of a row in bold.

| 1,000,000 orders | PostgreSQL | pg_sift `sift_col` | pg_sift `sift_table` | Elasticsearch | ClickHouse, lightweight UPDATE | ClickHouse, ALTER UPDATE | ClickHouse, ReplacingMergeTree + FINAL |
|---|---:|---:|---:|---:|---:|---:|---:|
| update 1% of the rows (rows/s, higher is better) | 27,421 | 32,206 | **62,397** | 8,862 | 17,455 | 1,342 | 22,876 |
| update 10% of the rows (rows/s) | 83,372 | **127,676** | 82,102 | 23,905 | 22,372 | 13,315 | 19,608 |
| delete 5% of the rows (rows/s) | **520,644** | 435,423 | 163,192 | 77,471 | 147 | 7,214 | 16,240 |
| single-row change: seen by the next query (ms after the acknowledgement) | **0.128** | 7.43 | 14.8 | 1,002 | 7.09 | 2,156 | 24.1 |
| revenue per month while 0 rows/s are updated: p50 (ms) | 170 | 31 | 53.6 | **6.41** | 7.23 | n/a | 26.2 |
| revenue per month while 1,000 rows/s are updated: p50 (ms) | 169 | 34 | 53.2 | **7.11** | 28.9 | n/a | 77.8 |
| revenue per month while 10,000 rows/s are updated: p50 (ms) | 167 | 34.6 | 53.4 | **8.17** | 27.3 | n/a | 73.2 |
| size before | 159 MiB | 143 MiB | 193 MiB (150 free) | 112 MiB | **38 MiB** | **38 MiB** | **38 MiB** |
| size after the changes | 176 MiB | 168 MiB | 198 MiB (134 free) | 138 MiB | 39 MiB | **36 MiB** | 41 MiB |
| size after VACUUM / merge / OPTIMIZE | 176 MiB | 176 MiB | 56 MiB (8 free) | 138 MiB | **37 MiB** | **36 MiB** | 39 MiB |

- Updating 10% of the rows, in rows per second: pg_sift `sift_col` 127,676, PostgreSQL 83,372, pg_sift `sift_table` 82,102, Elasticsearch 23,905, ClickHouse, lightweight UPDATE 22,372, ClickHouse, ReplacingMergeTree + FINAL 19,608, ClickHouse, ALTER UPDATE 13,315.
- Read-after-write (median of the single-row changes): Elasticsearch showed a change to a query 1,002 ms after the acknowledgement with the default refresh; a ClickHouse `ALTER UPDATE` 2,156 ms after it returned; PostgreSQL and pg_sift 0.128-14.8 ms.
- Size after the changes against before: PostgreSQL 1.11x, pg_sift `sift_col` 1.17x, pg_sift `sift_table` 1.03x, Elasticsearch 1.24x, ClickHouse, lightweight UPDATE 1.03x, ClickHouse, ALTER UPDATE 0.96x, ClickHouse, ReplacingMergeTree + FINAL 1.10x (after the maintenance step: PostgreSQL 1.11x, pg_sift `sift_col` 1.23x, pg_sift `sift_table` 0.29x, Elasticsearch 1.24x, ClickHouse, lightweight UPDATE 0.98x, ClickHouse, ALTER UPDATE 0.96x, ClickHouse, ReplacingMergeTree + FINAL 1.03x).
- **Where pg_sift loses:** deleting 5% of the rows: PostgreSQL 1.2x faster than pg_sift `sift_col` (520,644 against 435,423 rows/s).
- Method, the update path of each system and why, the first-run and latency spread tables, and the official pages: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#updates-and-deletes).
<!-- bench:store-updates:end -->

Every table, the cold-cache runs, the losses and the method: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Compared with

Feature facts checked against each project's own documentation on 2026-10-18 (speed is in [Benchmarks](#benchmarks)); `?` is not verified. Full tables, a note per row, sources, and the inferred and unverified cells: [docs/COMPARISON.md](docs/COMPARISON.md).

| Search | pg_sift | PG FTS | pg_search | pg_textsearch | rum | pgx-bm25 | TIN | Elasticsearch |
|---|---|---|---|---|---|---|---|---|
| BM25 ranking | Yes | No | Yes | Yes | No | Yes | Yes | Yes |
| Prefix, wildcard | Yes | Partly | Partly | Partly | Partly | Partly | Yes | Yes |
| Regex, fuzzy | Yes | No | Yes | No | No | No | Yes | Yes |
| Field weights | Partly | Yes | Partly | No | Partly | Yes | Partly | Yes |
| Facets | Yes | No | Yes | No | No | No | No | Yes |
| Filters inside the ranked scan | Yes | Partly | Yes | No | Partly | No | Partly | Yes |
| Top-N with other filters | Yes | No | Yes | Partly | Yes | Partly | Yes | Yes |
| `count(*)` from the index | Yes | No | Partly | No | No | No | Yes | Yes |
| MVCC, same-transaction reads | Yes | Yes | Yes | Yes | Yes | Yes | Yes | No |
| Physical replication, hot standby | Yes | Yes | No | Yes | Yes | Yes | Yes | n/a |
| Inside PostgreSQL, no extra service | Yes | Yes | Yes | Yes | Yes | Yes | Yes | No |
| Immutable segments, background merge | Yes | No | Yes | Partly | No | Partly | Yes | Yes |
| Stemmer languages | 1 | 29 | 20 | PG configs | PG configs | PG dictionaries | 18 | 36 analyzers |
| License | PostgreSQL | PostgreSQL | AGPL-3.0 | PostgreSQL | PostgreSQL | PostgreSQL | ? | AGPL-3.0, SSPL, ELv2 |

| Analytics | pg_sift `sift_col` | pg_sift `sift_table` | PostgreSQL | Citus columnar | ClickHouse | Elasticsearch | DuckDB | TimescaleDB columnstore |
|---|---|---|---|---|---|---|---|---|
| Columnar storage | Yes (as an index; the table stays a heap) | Yes | No | Yes | Yes | Yes | Yes | Partly |
| Aggregates on column data | Yes | Partly | No | No | Yes | Yes | Yes | Yes |
| `GROUP BY` expressions | Yes | Partly | Yes | Yes | Yes | Yes | ? | Yes |
| Joins | Yes | Yes | Yes | Yes | Yes | Partly | Yes | Yes |
| Pre-aggregates | Yes | No | Partly | Partly | Yes | Partly | ? | Yes |
| MVCC, transactions | Yes | Yes | Yes | Partly | Partly | No | Yes | Yes |
| `UPDATE`, `DELETE` | Yes | Partly | Yes | No | Partly | Yes | Yes | Yes |
| On an existing heap table via an index | Yes | No | n/a | No | No | No | No | No |
| Exact `count(DISTINCT)` | Yes | Yes | Yes | Yes | Yes | No | Yes | Yes |
| PostgreSQL SQL | Yes | Yes | Yes | Yes | No | No | Partly | Yes |
| Inside PostgreSQL, no extra server | Yes | Yes | Yes | Yes | No | No | No | Yes |
| License | PostgreSQL | PostgreSQL | PostgreSQL | AGPL-3.0 | Apache-2.0 | AGPL-3.0, SSPL, ELv2 | MIT | Apache-2.0; columnstore under the Timescale License |

pg_sift is weaker in text analysis (English stemming only, one key per index) and where its column engine is narrow (joins are accelerated for small tables only, `sift_table` scans the superblocks that hold a tombstone or a write buffer row instead of reading their pre-aggregates): [docs/COMPARISON.md](docs/COMPARISON.md#where-pg_sift-is-weaker).

## What is covered

<!-- coverage:start (generated by `scripts/coverage.sh --gen` from docs/coverage.tsv; do not edit by hand) -->
[docs/COVERAGE.md](docs/COVERAGE.md) lists 232 query shapes with a sample each (174 Yes, 22 Partly, 26 No, 10 Error), checked on a test cluster by `scripts/coverage.sh --check`. The analytics shapes by family:

| query shape | covered | engine |
|---|---|---|
| `count(*)`, `count(DISTINCT x)` | Yes | sift_col |
| `sum`, `avg`, `min`, `max` | Yes | sift_col |
| `stddev`, `percentile_*`, `string_agg`, `FILTER (WHERE ...)` | Yes | sift_col |
| `WHERE x = 1`, `IN (...)`, `LIKE 'a%'`, `~ 're'`, `IS NULL` | Yes | sift_col |
| `GROUP BY x`, `GROUP BY expr`, `ROLLUP`, `CUBE` | Yes | sift_col |
| `JOIN` a small table, `EXISTS`, `IN (SELECT ...)` | Yes | sift_col |
| `HAVING`, `ORDER BY ... LIMIT n` of the groups | Yes | sift_col |
| `WHERE ... ORDER BY x LIMIT n` of rows | Partly | sift_col |
| pre-aggregated `count`, `sum`, `avg`, `min`, `max` | Yes | sift_col |
| `jsonb` paths: `col->>'k'`, `jsonb_to_recordset(col)` | Yes | sift_col |
| `numeric(p>18)`, `money`, `interval` | Yes | sift_col |
| `time`, `timetz`, enums, `uuid`, `bytea`, `inet`, `macaddr`, `bit`, arrays, ranges | Yes | sift_col |
| `RIGHT JOIN`, `SELECT DISTINCT`, volatile functions | Partly | sift_col |

<!-- coverage:end -->

- Search: `@@> 'a b'`, `"a b"`, `batt*`, `sq?eaky`, `/re/`, `frendly~1`, `ORDER BY score LIMIT k`, `count(*)` and `AND` filters run on the `sift` index.
- Everything else keeps the normal plan (correct, only not accelerated), and a few cases raise an error that names the rewrite.

## SQLite

The search engine as a loadable extension, for apps that want search without a server.

- **Needs:** Linux x86-64, SQLite 3.38 or newer.
- **Queries:** exact BM25, phrase, prefix, wildcard, regex and fuzzy; highlights, snippets, facets.
- **Speed:** 89x, 242x and 10x the throughput of FTS5 on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles).
- **Install, options and numbers:** [sqlite/README.md](sqlite/README.md).

```sql
.load ./sift
CREATE VIRTUAL TABLE docs USING sift(title, body);
SELECT rowid, rank FROM docs WHERE docs MATCH 'case OR clip' ORDER BY rank LIMIT 10;   -- BM25 top-10, best first
```

## MariaDB

`libsift_udf.so` and two SQL files bring the engine to MariaDB, with no storage engine and no server patch.

- **Needs:** MariaDB 10.5 to 13.x; results are identical to pg_sift.
- **Speed:** 183x to 794x faster than InnoDB FULLTEXT on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles), with an index 1.7x smaller.
- **Install and guide:** [mariadb/README.md](mariadb/README.md).

```sql
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);   -- index, queue, triggers
SELECT sift_count('news', 'power AND plant');
```

## Operations

- **Options:** index options go in `WITH (...)` (`k1`, `b`, `analyzer`, `stemmer`, `positions`, `prefix_index`, `write_buffer_limit`, `merge_fanin`, ...); `sift_table` is tuned with settings (`sift.table_*`), not per table.
- **Maintenance:** a background worker (`sift.async_maintenance`) promotes write buffers and merges segments; VACUUM clears dead rows.
- **Inspection:** `sift.index_info`, `sift.segments` and `sift.verify`.
- **Replication:** physical replicas work through generic WAL.
- **Cost of one library (3.0):** the BM25 index and the column engine share one library, about twice the size of 2.0's. Against 2.0.x (`bench/steps/cpu_gate.sh`, user instructions per operation) every cell is within 1.02 except three BM25 cells:
  - the first query of a session: about 1.04 to 1.05 (connection pooling amortizes it);
  - a `VACUUM` with nothing to free: about 1.04, on 3.9M instructions;
  - a ranked query on a one-segment index: 1.02 to 1.03.
- **More:** settings, options, maintenance, replication and limits in [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations); disk, memory and time of a large index in [docs/SIZING.md](docs/SIZING.md).

## Limits

- **Platforms:** PostgreSQL 17 or 18 (x86-64 tested); SQLite port on Linux x86-64 with SQLite 3.38 or newer; MariaDB port on 10.5 to 13.x.
- **Ports:** search only; the column engine stays PostgreSQL-only in 3.0 ([docs/COL-PORTS.md](docs/COL-PORTS.md)).
- **Replication:** publishing a `sift_table` under logical replication needs `shared_preload_libraries`.
- **Planner:** what is not accelerated keeps the normal plan, and a few cases raise an error that names the rewrite ([What is covered](#what-is-covered)).
- **Weaker than other systems:** text analysis and the narrow parts of the column engine ([Compared with](#compared-with)).
- **Types, options and limits of the column engine:** [docs/USAGE.md](docs/USAGE.md#limits).

## Vulkan (experimental)

Opt-in and off by default: build with `make VULKAN=1` and `SET sift.gpu = auto|on` (`auto`: grouped aggregates on indexes of at least `sift.gpu_min_rows`, <!-- gen:guc:sift.gpu_min_rows -->5,000,000<!-- /gen:guc:sift.gpu_min_rows --> rows; `on`: also ungrouped). It runs grouped and filtered integer and `numeric` counts and sums of a `sift_col` index on a Vulkan device, clean all-visible units on the device and the rest on the CPU in the same query; a missing or failed device falls back to the CPU. The compressed chunks stay on the device, so use it from persistent connections (each backend pays about 0.2 s to open it). Not eligible: floats, `min`/`max`, `count(DISTINCT)`, text filters, more than one key, `sift_table`. Details and limits: [docs/GPU.md](docs/GPU.md).

<!-- bench:gpu:begin (generated by `bench/bench extras`; do not edit by hand) -->
Vulkan aggregate path against the CPU on the store table (bench/gpu/bench-pg.sh ROWS 9), pg_sift v3.1.0 built with VULKAN=1, one backend, one core on the CPU side, NVIDIA GeForce RTX 3090, measured 2026-10-20 under the perf lock.

Queries on the table of the online-store benchmark, with one sift_col index over the columns they read: `q7` `SELECT count(*) FROM orders WHERE amount > 100 AND created_at >= '2025-12-02'`; `q8` `SELECT count(*) FROM orders WHERE quantity >= 4 AND amount < 50`; `q9` `SELECT count(*), sum(amount), sum(quantity) FROM orders WHERE amount > 20 AND quantity >= 2`; `g1` `SELECT country, count(*), sum(amount) FROM orders WHERE quantity >= 2 GROUP BY country`; `g2` `SELECT category, count(*), sum(quantity) FROM orders WHERE amount > 100 GROUP BY category`; `g3` `SELECT date_trunc('month', created_at)::date, sum(amount) FROM orders WHERE quantity >= 2 GROUP BY 1`.

| rows | query | CPU, first (ms) | CPU, median (ms) | device, first (ms) | device, median, resident (ms) | resident vs CPU |
|---|---|---:|---:|---:|---:|---:|
| 1,000,000 | `q7` | 2.19 | 1.27 | 5.18 | 0.36 | 3.55× |
| 1,000,000 | `q8` | 1.88 | 0.70 | 4.36 | 0.35 | 2.03× |
| 1,000,000 | `q9` | 4.68 | 3.62 | 4.28 | 0.39 | 9.25× |
| 1,000,000 | `g1` | 4.83 | 3.65 | 4.62 | 0.40 | 9.07× |
| 1,000,000 | `g2` | 3.54 | 2.55 | 4.24 | 0.52 | 4.93× |
| 1,000,000 | `g3` | 4.96 | 4.15 | 4.85 | 0.66 | 6.28× |
| 10,000,000 | `q7` | 19.2 | 10.4 | 26.6 | 1.96 | 5.30× |
| 10,000,000 | `q8` | 13.5 | 8.31 | 18.0 | 2.30 | 3.62× |
| 10,000,000 | `q9` | 28.1 | 24.6 | 16.2 | 1.71 | 14.4× |
| 10,000,000 | `g1` | 41.2 | 32.4 | 17.5 | 1.46 | 22.3× |
| 10,000,000 | `g2` | 25.8 | 18.9 | 18.4 | 2.01 | 9.40× |
| 10,000,000 | `g3` | 30.2 | 26.6 | 18.0 | 2.06 | 12.9× |

Reproduce: `bench/bench extras gpu` (a Vulkan device and `VULKAN=1`; results: [results/gpu.tsv](docs/results/gpu.tsv)).
<!-- bench:gpu:end -->

## Docs

- [docs/USAGE.md](docs/USAGE.md): install, queries, ranking, options, settings, maintenance, replication, limits.
- [docs/SIZING.md](docs/SIZING.md): disk, memory and time of a large index.
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md): every benchmark table, the losses and the method; [docs/COVERAGE.md](docs/COVERAGE.md): what the engine accelerates.
- [docs/COMPARISON.md](docs/COMPARISON.md): feature tables against other search and analytics systems, with sources and the date checked.
- [docs/COL.md](docs/COL.md): the column engine; [docs/COL-PLAN.md](docs/COL-PLAN.md) and [docs/AGG.md](docs/AGG.md): aggregate pushdown and its test suite.
- [docs/COL-TAM.md](docs/COL-TAM.md): `sift_table`; [docs/COL-SHRED.md](docs/COL-SHRED.md): `jsonb` paths; [docs/COL-CODEC.md](docs/COL-CODEC.md): chunk compression.
- [docs/FORMATS.md](docs/FORMATS.md): the on-disk format numbers (metapage minor, segment formats, flags) and the upgrade tests.
- [docs/GPU.md](docs/GPU.md): the experimental, opt-in Vulkan aggregate path.
- [docs/COL-PORTS.md](docs/COL-PORTS.md): the column engine on SQLite and MariaDB; [docs/SAFETY.md](docs/SAFETY.md): memory safety in the C code.
- [DESIGN.md](DESIGN.md): design and on-disk format; [CHANGELOG.md](CHANGELOG.md): changes per release.

## Testing, security, license

Tests: `make check`, and `make check-full` with sanitizers, soak, fuzz and mutants ([CONTRIBUTING.md](CONTRIBUTING.md)); the tree has <!-- gen:tests -->304 regression tests (`test/sql`), 30 isolation specs, 53 TAP tests (`test/t`), 744 column fixtures and 232 coverage rows<!-- /gen:tests -->. Report crashes, wrong results and index corruption privately as in [SECURITY.md](SECURITY.md).
pg_sift is released under the [PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
