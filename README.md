# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml) [![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018%20%7C%2019-336791?logo=postgresql&logoColor=white)](docs/SUPPORT.md#postgresql-versions) [![release](https://img.shields.io/badge/release-3.4.0-blue)](https://github.com/brauliobo/pg_sift/releases/latest) [![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

pg_sift adds ranked full-text search and column-store analytics to PostgreSQL with one `CREATE EXTENSION pg_sift`: no search server, no separate analytics database, no `shared_preload_libraries`.
It is for applications that keep their data in PostgreSQL and serve dashboards, reports and search from the same rows. A `sift` index answers BM25 queries (boolean, phrase, prefix, regex, fuzzy, facets); a `sift_col` index
or a `sift_table` answers counts, sums, distinct counts, group-bys and top-N from compressed columns; a query the planner cannot prove equal keeps PostgreSQL's own plan. It supports PostgreSQL 17, 18 and 19[^pg19]; the search engine also runs as a SQLite extension and a MariaDB library.

[^pg19]: PostgreSQL 19 is tested on its release candidate (a server built from `REL_19_STABLE`, the same jobs as for 17 and 18); see [docs/SUPPORT.md](docs/SUPPORT.md#postgresql-versions).

[In five minutes](#in-five-minutes) · [At a glance](#at-a-glance) · [Quick start](#quick-start) · [Features](#features) · [Compared with](#compared-with) · [SQLite and MariaDB](#sqlite-and-mariadb) · [Operations and limits](#operations-and-limits) · [SQL reference](#sql-reference) · [Docs](#docs)


## In five minutes

Install from source (PostgreSQL 17, 18 or 19 with server headers and a C compiler; packages and Docker are under [Install](#install)):

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install
psql -d mydb -c 'CREATE EXTENSION pg_sift'      # as a superuser
```

Then, in `psql -d mydb`, a generated table of 1,000,000 orders, nothing to download:

```sql
CREATE TABLE orders AS
SELECT g AS id,
       1 + (random() * 50000)::int AS customer_id,
       (ARRAY['US','DE','BR','JP','IN','FR'])[1 + (random() * 5)::int] AS country,
       (ARRAY['books','toys','garden','music'])[1 + (random() * 3)::int] AS category,
       round((random() * 300)::numeric, 2)::numeric(10,2) AS amount,
       timestamp '2025-01-01' + random() * interval '365 days' AS created_at
FROM generate_series(1, 1000000) g;
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, amount, created_at);
VACUUM ANALYZE orders;

EXPLAIN (COSTS OFF) SELECT category, count(*), sum(amount), avg(amount) FROM orders GROUP BY 1;   -- Custom Scan (SiftAgg) on orders_col
SET sift.enable_agg = off;                                                                           -- the plan PostgreSQL has without pg_sift
```

Medians of six runs, warm, PostgreSQL 18.6, the same table and answers with `sift.enable_agg` on and off:

| query | pg_sift | PostgreSQL |
|---|---:|---:|
| `SELECT category, count(*), sum(amount), avg(amount) FROM orders GROUP BY 1` | 0.14 ms | 54 ms |
| `SELECT count(DISTINCT customer_id) FROM orders WHERE country = 'DE'` | 2.5 ms | 68 ms |
| `SELECT customer_id, sum(amount) FROM orders GROUP BY 1 ORDER BY 2 DESC LIMIT 5` | 12 ms | 177 ms |
| `SELECT country, category, count(*), sum(amount) FROM orders WHERE created_at >= '2025-06-01' AND created_at < '2025-07-01' GROUP BY 1, 2` | 20 ms | 30 ms |

The last row is a month of random dates, which no unit can skip: the gain is the smallest there. Declare `amount` as `numeric(10,2)`: with a bare `numeric` the scan is chosen and still wins, but 4 to 19 times less than with the precision ([known gap](#limitations-and-trade-offs)). Full-text search is [below](#try-it-full-text-search).

**Use it** when the data stays in PostgreSQL and you want dashboards, reports or ranked search from the same rows, with counts, sums, distinct counts, group-bys and top-N that do not scan the table. **Do not use it** for a primary-key lookup (the btree is as fast), for update-heavy tables through `sift_table` (a write costs more than a heap write), or for warehouse-size tables that need sharding; every measured loss is in [Limitations and trade-offs](#limitations-and-trade-offs). **Versions:** PostgreSQL 17, 18 and 19 on Linux, 17 and 18 on macOS ([docs/SUPPORT.md](docs/SUPPORT.md)). **Upgrades:** `ALTER EXTENSION pg_sift UPDATE`, and a rebuild only when the on-disk format changes ([docs/UPGRADING.md](docs/UPGRADING.md)). **License:** the [PostgreSQL License](LICENSE).

## At a glance

**Use it** when the question reads many rows and the answer is small, or when ranked search should sit next to the data:

- **Aggregates over a filtered table:** counts, sums, distinct counts, group-bys, time buckets, top-N and window reports over hundreds of thousands to millions of rows, answered from pre-aggregates and compressed columns; the time grows slower than the rows.
- **Ranked search next to the data:** BM25 with filters, counts and facets in the same transaction as the rows and no second system to keep in step; updates, deletes, VACUUM and replicas are PostgreSQL's.

**Do not use it** (every measured loss is in [Limitations and trade-offs](#limitations-and-trade-offs)) for:

- a lookup by key where a btree exists: the btree is as fast or faster, and the planner keeps it;
- a report whose whole query takes a few milliseconds over a small result: flat PostgreSQL tables are as fast or faster than the scan;
- tables that need a warehouse: it is one PostgreSQL node, without sharding or distributed execution; ClickHouse loads faster and stores the same table in less space, and ClickHouse and Elasticsearch are ahead on some filtered averages and range counts at 10,000,000 rows;
- text analysis beyond English stemming and one key per index, and the search classes where Elasticsearch is ahead;
- writes, spilling distinct counts and queries that keep a few rows: slower than PostgreSQL's own plan on some shapes;
- a shape the planner cannot prove equal: it keeps the normal plan.

### Limitations and trade-offs

**Known gap: a `numeric` without precision.** `GROUP BY` two columns (16 groups) with `count`, `sum` and `avg` of a bare `numeric` column takes the scan and, since the sum runs in workers, beats PostgreSQL's plan at every size and worker count measured: 6.8 against 11.5 ms at 100,000 rows (2 workers); 23.3 against 43.4 ms at 1,000,000 (2 workers; 22.7 against 43.4 with 4) and 46.6 against 109.9 ms in one process (693 against 2,110 million instructions); 160 against 389 ms at 10,000,000 (2 workers; 112 against 248 with 4). With `numeric(10,2)` the same query takes 5.8 ms at 1,000,000 rows and 20.6 ms at 10,000,000 (2 workers), 4 to 19 times faster than with a bare `numeric`, so declare the precision of money-like columns. What remains: a bare `numeric` column of almost unique values (2,000,000 rows, 6 groups, `round(random() * 100000, 2)`) took the scan with the default 2 workers in 79.6 ms against 61.7 ms for PostgreSQL's plan (29% slower), because every unit holds its own dictionary of values there; the planner now prices the dictionaries that the workers inflate and keeps PostgreSQL's plan with 2, 4 and 8 workers (79, 62 and 62 ms), and still takes the scan without workers.

## Quick start

### Install

PostgreSQL 17, 18 and 19 on Linux (x86-64 and arm64); 17 and 18 on macOS arm64 (Homebrew has no `postgresql@19` yet: build 19 from source). Release packages are built for PostgreSQL 18; for 17 and 19 build from source.

| Where | How |
|---|---|
| Debian 13, Ubuntu 24.04 (PGDG `postgresql-18`) | download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest), then `sudo apt install ./postgresql-18-pg-sift_*_amd64.deb` |
| RHEL family (PostgreSQL 18) | download the `.rpm` from the latest release, then `sudo dnf install ./pg_sift_18-*.rpm` |
| Arch Linux | the AUR package `pg_sift` (recipe in [packaging/](packaging/)) |
| Docker | `packaging/docker/build.sh 18` builds the image `pg_sift:pg18` on the official `postgres` image: [docs/QUICKSTART.md](docs/QUICKSTART.md), [docs/CONTAINERS.md](docs/CONTAINERS.md) (Docker Compose, CloudNativePG) |
| macOS arm64 | the Homebrew formula below |
| anything else | from source (the first snippet of [In five minutes](#in-five-minutes); needs server headers with `pg_config` on `PATH` and a C compiler; `PG_CONFIG=/path/to/pg_config` picks another installation) |

Then, as a superuser, `CREATE EXTENSION pg_sift` in each database (after an upgrade: `ALTER EXTENSION pg_sift UPDATE`). Details: [docs/USAGE.md](docs/USAGE.md#install), [docs/UPGRADING.md](docs/UPGRADING.md), [docs/SUPPORT.md](docs/SUPPORT.md).

<details>
<summary>macOS with Homebrew</summary>

The [formula](Formula/pg_sift.rb) builds `main` (HEAD-only) for Homebrew PostgreSQL 17 and 18 on macOS arm64:

```sh
brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git
brew install --HEAD brauliobo/pg_sift/pg_sift
brew test brauliobo/pg_sift/pg_sift
"$(brew --prefix postgresql@18)/bin/psql" -d mydb -c 'CREATE EXTENSION pg_sift;'   # a running server, as a superuser; postgresql@17 for 17
```

CI builds it and runs `brew test` (a BM25 smoke, an extension upgrade and a `sift_col` aggregate) on macOS arm64 with PostgreSQL 17 and 18, and runs a few regression tests there and the quick tier on Linux arm64; it was also tried by hand on Apple Silicon with 17.11 and 18.6.
`make check` needs bash 4 or newer (`brew install bash`; the stock bash 3.2 stops with a message). A stage the host cannot run (the Perl TAP tests, which need the server's `src/test/perl`; the compatibility tests that build releases older than 3.2.0; the Python 3.12 benchmark generators) is skipped with a message.

</details>

### Try it: full-text search

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

### Try it: analytics

```sql
CREATE TABLE orders (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), created_at timestamp);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, created_at);

SELECT category, count(*), sum(amount) FROM orders
WHERE country = 'DE' AND created_at >= '2025-06-01' GROUP BY category;          -- Custom Scan (SiftAgg) on orders_col
CREATE TABLE events (id bigint, kind text, price numeric(10,2), at timestamp) USING sift_table;   -- the table itself in column format
```

`SET sift.enable_agg = off` gives the plan PostgreSQL has without sift. Types, options and limits: [docs/USAGE.md](docs/USAGE.md#column-indexes-aggregates-and-sift_table).

## Features

- **BM25 search index** (`USING sift`): boolean, phrase, prefix, wildcard, regex and fuzzy queries, exact BM25 ranking, top-k with filters applied inside the scan, counts from the index, facets, autocomplete.
- **Column engine** (`USING sift_col`, `USING sift_table`): `count`, `count(DISTINCT ...)`, `sum`, `avg`, `min`, `max` with filters and `GROUP BY` on compressed columns as `Custom Scan (SiftAgg)`; `sift_table` stores the table itself in column format.
- **Pre-aggregates, top-N and lookups:** units whose rows are all visible answer from the exact count/sum/min/max stored with them (on by default); `ORDER BY ... LIMIT n` stops after the first rows of the units that can hold them; an equality or `IN` lookup on a key column reads only the units that hold it (`point_columns`, `point_tid_columns`; opt-in per index).
- **Faster paths of 3.4**:
  - *Top-N by a float key, or by a column that follows the table:* a float sort key goes through the same min/max and compare kernels as an integer one, so units whose best value cannot pass the n-th are not read: `ORDER BY value DESC LIMIT 10` over 1,000,000 readings 47.5 to 0.80 ms (`ts_top_value`, 60x); the newest 10 rows of a time column with no btree 80.6 to 0.22 ms (`hits_latest_time`, 367x).
  - *The first statement of a new backend:* the choices a statement needs several planner runs for are kept in shared memory (`sift.verdict_cache_kb`, [docs/USAGE.md](docs/USAGE.md)), so a new backend plans the winning form only: a report of 21 CTEs and 16 lateral sub-selects takes 29.1 ms on the first statement against 35.2 ms with the cache off, 20.0 ms on the second and 20.9 ms on PostgreSQL (`verdict_shared_report`, still 1.4x slower than PostgreSQL on its first run); 12 lateral aggregates take 45.9 ms first and 41.7 ms second against 3,507 ms on PostgreSQL (`lateral_flat_plan`, 76x).
  - *Where the planner is right:* the first rows of each group by a window function (`window_first_rows`: 22.2 ms on PostgreSQL's plan) and the newest 100 rows of one device by its btree (`ts_latest_device`, 0.064 ms) are decisions of the planner, not speed-ups. In 3.4.0 the planner takes the scan for the window shape and it is slower (26.2 ms); 3.4.1 restores PostgreSQL's plan.
- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, generic WAL (crash and replica safe), `pg_dump`; a query the planner cannot prove equal keeps its normal plan.
- **Vulkan (experimental, opt-in):** `make VULKAN=1` and `SET sift.gpu = auto|on` run grouped and filtered integer counts and sums of a `sift_col` index on a GPU ([below](#vulkan-experimental), [docs/GPU.md](docs/GPU.md)).
- **Ports:** [SQLite](#sqlite) and [MariaDB](#mariadb) run the same search core.

## Compared with

Feature facts checked against each project's own documentation on 2026-10-18; `?` is not verified. Search (pg_sift against PG FTS, pg_search, pg_textsearch, rum, pgx-bm25, TIN and Elasticsearch) and analytics (against PostgreSQL, Citus columnar, ClickHouse, Elasticsearch, DuckDB and TimescaleDB) tables, a note per row, sources and the unverified cells: [docs/COMPARISON.md](docs/COMPARISON.md).

pg_sift is weaker in text analysis (English stemming only, one key per index) and where its column engine is narrow: joins are accelerated for small tables only, and `sift_table` scans the superblocks that hold a tombstone or a write buffer row instead of reading their pre-aggregates ([where it is weaker](docs/COMPARISON.md#where-pg_sift-is-weaker)).

## SQLite and MariaDB

The search engine runs outside PostgreSQL too, with the same core and the same results; the column engine is PostgreSQL-only ([docs/COL-PORTS.md](docs/COL-PORTS.md)).

### SQLite

The search engine as a loadable extension, for apps that want search without a server (Linux x86-64, SQLite 3.38 or newer): exact BM25, phrase, prefix, wildcard, regex and fuzzy queries, highlights, snippets, facets; 89x, 242x and 10x the throughput of FTS5 on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles). Install, options and numbers: [sqlite/README.md](sqlite/README.md).

```sql
.load ./sift
CREATE VIRTUAL TABLE docs USING sift(title, body);
SELECT rowid, rank FROM docs WHERE docs MATCH 'case OR clip' ORDER BY rank LIMIT 10;   -- BM25 top-10, best first
```

### MariaDB

MariaDB 10.5 to 13.x, with results identical to pg_sift: `libsift_udf.so` and two SQL files, with no storage engine and no server patch; 183x to 794x faster than InnoDB FULLTEXT on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles), with an index 1.7x smaller. Install and guide: [mariadb/README.md](mariadb/README.md).

```sql
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);   -- index, queue, triggers
SELECT sift_count('news', 'power AND plant');
```

## Operations and limits

- **Options:** index options go in `WITH (...)` (`k1`, `b`, `analyzer`, `stemmer`, `positions`, `prefix_index`, `write_buffer_limit`, `merge_fanin`, ...); `sift_table` is tuned with settings (`sift.table_*`), not per table.
- **Maintenance** is on by default: a background worker (`sift.async_maintenance`) promotes write buffers and merges segments, `sift.col_auto_merge` vacuums and merges a `sift_col` index after churn, `sift.table_auto_defrag` clears the dead rows of a `sift_table`; VACUUM clears dead rows as usual.
- **Large `work_mem` and unusual planner costs:** a `seq_page_cost` above `random_page_cost` moves the planner's choice between the scan and the normal plan most; the results are the same as the normal plan's either way.
- **Inspection:** `sift.index_info`, `sift.segments` and `sift.verify`.
- **Replication:** physical replicas work through generic WAL; publishing a `sift_table` under logical replication needs `shared_preload_libraries`.
- **One library:** the BM25 index and the column engine share one library, about twice the size of the search-only build, at a cost of at most 2% of the instructions of an operation[^lib].
- **Platforms:** PostgreSQL 17, 18 or 19 (x86-64 tested; arm64 tested with 18); SQLite port on Linux x86-64 with SQLite 3.38 or newer; MariaDB port on 10.5 to 13.x.
- **Planner:** what is not accelerated keeps the normal plan, and a few cases raise an error that names the rewrite.
- **Column engine types, options and limits:** [docs/USAGE.md](docs/USAGE.md#limits); settings, maintenance and replication: [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations); disk, memory and time of a large index: [docs/SIZING.md](docs/SIZING.md).

[^lib]: Against 2.0.x (`bench/steps/cpu_gate.sh`, user instructions per operation) every cell is within 1.02 except three BM25 cells: the first query of a session (about 1.04 to 1.05; connection pooling amortizes it), a `VACUUM` with nothing to free (about 1.04, on 3.9M instructions) and a ranked query on a one-segment index (1.02 to 1.03).

<!-- sift-reference:start -->

## SQL reference

Schema `sift` has 52 functions, 5 views, 1 table, 2 operator classes, 1 operator. Signatures, results, the release that added each and a line on what it does are in [docs/REFERENCE.md](docs/REFERENCE.md), generated from the SQL scripts.

- **[Search and ranking](docs/REFERENCE.md#search-and-ranking)**: `@@>`, `approx_count_distinct`, `cluster_advice`, `facet_counts`, `facet_sql`, `facets`, `field_query`, `highlight`, `index_advice`, `index_info`, `key_cut_decision`, `match`, `match_idx`, `max_score`, `parse_query`, `score`, `sections`, `segment_postings`, `segments`, `suggest_paths`, `text_ops`, `tokenize`, `verify`
- **[Column index](docs/REFERENCE.md#column-index)**: `col_*_ops`, `col_clustering`, `col_index_info`, `col_index_status`, `col_leaves`, `col_maintenance_status`, `col_ops`, `col_sections`, `col_segments`, `col_unit_agg`, `col_unit_hist`, `col_verify`
- **[Tables](docs/REFERENCE.md#tables)**: `table_paths`, `table_paths_maintained`, `table_segments`, `table_set_paths`, `table_space`
- **[Maintenance and workers](docs/REFERENCE.md#maintenance-and-workers)**: `col_maintain`, `col_maintenance_due`, `col_merge`, `col_promote`, `col_refresh`, `col_rollup_counters`, `col_rollup_queue`, `col_rollup_refresh`, `col_rollup_requests`, `col_rollup_state`, `merge`, `promote`, `table_maintain`, `worker_state`, `workers`
- **[Diagnostics and upgrade](docs/REFERENCE.md#diagnostics-and-upgrade)**: `col_rollup_misses`, `explain_why`, `refusal_slugs`, `stats`, `stats_reset`, `upgrade_commands`, `version`

Settings (`sift.*`) are in the [settings table](docs/USAGE.md#maintenance-and-operations), fixed limits in [docs/LIMITS.md](docs/LIMITS.md).
<!-- sift-reference:end -->

<!-- sift-options:start -->

## Options

144 index options (`WITH (...)`), `sift.*` settings and automatic behaviors, each with its default, class (`default`, `opt-in`, `diagnostic`, `safety`, `deprecated`), measured gain and cost, and off switch, are in [docs/OPTIONS.md](docs/OPTIONS.md); types, ranges and contexts are in the tables of [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations).
<!-- sift-options:end -->

## Vulkan (experimental)

Opt-in and off by default: build with `make VULKAN=1` and `SET sift.gpu = auto|on` (`auto`: grouped aggregates on indexes of at least `sift.gpu_min_rows`, <!-- gen:guc:sift.gpu_min_rows -->5,000,000<!-- /gen:guc:sift.gpu_min_rows --> rows; `on`: also ungrouped). Grouped and filtered integer and `numeric` counts and sums of a `sift_col` index run on a Vulkan device (clean all-visible units on the device, the rest on the CPU in the same query); a missing or failed device falls back to the CPU. The compressed chunks stay on the device, so use persistent connections (each backend pays about 0.2 s to open it). Not eligible: floats, `min`/`max`, `count(DISTINCT)`, text filters, more than one key, `sift_table`. Measurements and limits: [docs/GPU.md](docs/GPU.md#when-it-helps).

## Docs

- [docs/USAGE.md](docs/USAGE.md): install, queries, ranking, options, settings, maintenance, replication, limits.
- [docs/SIZING.md](docs/SIZING.md): disk, memory and time of a large index.
- [docs/COMPARISON.md](docs/COMPARISON.md): feature tables against other search and analytics systems, with sources and the date checked.
- [docs/FORMATS.md](docs/FORMATS.md): the on-disk format numbers (metapage minor, segment formats, flags).
- [docs/UPGRADING.md](docs/UPGRADING.md): the upgrade preflight (`scripts/upgrade-check.sh`), `ALTER EXTENSION UPDATE`, `pg_upgrade` to a newer PostgreSQL major and going back to an older release.
- [docs/GPU.md](docs/GPU.md): the experimental, opt-in Vulkan aggregate path.
- [docs/COL-PORTS.md](docs/COL-PORTS.md): the column engine on SQLite and MariaDB; [docs/SAFETY.md](docs/SAFETY.md): memory safety in the C code.

## Security and license

Tests: `make check`, and `make check-full` with sanitizers, soak, fuzz and mutants ([CONTRIBUTING.md](CONTRIBUTING.md)); the tree has <!-- gen:tests -->529 regression tests (`test/sql`), 42 isolation specs, 91 TAP tests (`test/t`), 962 column fixtures and 252 coverage rows<!-- /gen:tests -->. Report crashes, wrong results and index corruption privately as in [SECURITY.md](SECURITY.md).
pg_sift is released under the [PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
