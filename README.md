# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml) [![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018%20%7C%2019-336791?logo=postgresql&logoColor=white)](docs/SUPPORT.md#postgresql-versions) [![release](https://img.shields.io/badge/release-3.3.0-blue)](https://github.com/brauliobo/pg_sift/releases/latest) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018%20%7C%2019-336791)](#install) [![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

pg_sift adds ranked full-text search and column-store analytics to PostgreSQL with one `CREATE EXTENSION pg_sift`: no search server, no separate analytics database, no `shared_preload_libraries`.
It is for applications that keep their data in PostgreSQL and serve dashboards, reports and search from the same rows. A `sift` index answers BM25 queries (boolean, phrase, prefix, regex, fuzzy, facets); a `sift_col` index
or a `sift_table` answers counts, sums, distinct counts, group-bys and top-N from compressed columns; a query the planner cannot prove equal keeps PostgreSQL's own plan. It supports PostgreSQL 17, 18 and 19[^pg19]; the search engine also runs as a SQLite extension and a MariaDB library.

[^pg19]: PostgreSQL 19 is tested on its release candidate (a server built from `REL_19_STABLE`, the same jobs as for 17 and 18); see [docs/SUPPORT.md](docs/SUPPORT.md#postgresql-versions).

[In five minutes](#in-five-minutes) · [At a glance](#at-a-glance) · [Quick start](#quick-start) · [Features](#features) · Benchmarks · [Compared with](#compared-with) · What is covered · [SQLite and MariaDB](#sqlite-and-mariadb) · [Operations and limits](#operations-and-limits) · [SQL reference](#sql-reference) · [Docs](#docs)

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

The last row is a month of random dates, which no unit can skip: the gain is the smallest there. Declare `amount` as `numeric(10,2)`: with a bare `numeric` the planner kept PostgreSQL's plan on this table, because the scan costs more than a parallel sequential scan. Full-text search is [below](#try-it-full-text-search).

**Use it** when the data stays in PostgreSQL and you want dashboards, reports or ranked search from the same rows, with counts, sums, distinct counts, group-bys and top-N that do not scan the table. **Do not use it** for a primary-key lookup (the btree is as fast), for update-heavy tables through `sift_table` (a write costs more than a heap write), or for warehouse-size tables that need sharding; every measured loss is in [Limitations and trade-offs](#limitations-and-trade-offs). **Versions:** PostgreSQL 17, 18 and 19 on Linux, 17 and 18 on macOS ([docs/SUPPORT.md](docs/SUPPORT.md)). **Upgrades:** `ALTER EXTENSION pg_sift UPDATE`, and a rebuild only when the on-disk format changes ([docs/UPGRADING.md](docs/UPGRADING.md)). **License:** the [PostgreSQL License](LICENSE).

## At a glance

### When pg_sift is the right tool, and when it is not

It wins where the question reads many rows and the answer is small:

- **Aggregates over a filtered table:** counts, sums, distinct counts, group-bys, time buckets, top-N and window reports over hundreds of thousands to millions of rows, answered from pre-aggregates and compressed columns, with the time growing slower than the rows (the rows above, Scale).
- **Ranked search next to the data:** BM25 with filters, counts and facets in the same transaction as the rows, with no pipeline that keeps a second system in step; updates, deletes, VACUUM and replicas are PostgreSQL's.

It does not win, and the table and charts show each case:

- **A lookup by key** where a btree exists: the btree is as fast or faster, and the planner keeps it.
- **A report of a few stores over a small result:** when the whole query takes a few milliseconds, PostgreSQL's flat tables are as fast or faster than the scan (Query shapes).
- **Large analytic tables that need a warehouse:** it is one PostgreSQL node, without sharding or distributed execution; ClickHouse loads faster and stores the same table in less space, and ClickHouse and Elasticsearch are ahead on some filtered averages and range counts at 10,000,000 rows.
- **Text analysis beyond English stemming and one key per index**, and the search classes where Elasticsearch is ahead (the Search table).
- **Writes, spilling distinct counts and queries that keep a few rows** are slower than PostgreSQL's own plan on some shapes, and the load and the disk are behind ClickHouse's: the list below is generated from the results.
- **A shape the planner cannot prove equal** keeps the normal plan.

### Limitations and trade-offs

**Known gap: a `numeric` without precision.** On a generated table of 1,000,000 rows, `GROUP BY` four values with `count`, `sum` and `avg` of a bare `numeric` column keeps PostgreSQL's plan: the scan is costed at 83,566 against 16,016 for the normal plan, and when forced it is slower (170 ms against 55 ms). With `numeric(10,2)` the scan is chosen and answers in 0.14 ms against 54 ms. Declare the precision of money-like columns until the cost model reads the bare type.

## Quick start

### Install

Supports PostgreSQL 17, 18 and 19 on Linux (x86-64 and arm64) and PostgreSQL 17 and 18 on macOS arm64 (Homebrew has no `postgresql@19` yet: build 19 from source). The release packages are built for PostgreSQL 18; for 17 and 19 build from source.

| Where | How |
|---|---|
| Debian 13, Ubuntu 24.04 (PGDG `postgresql-18`) | download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest), then `sudo apt install ./postgresql-18-pg-sift_*_amd64.deb` |
| RHEL family (PostgreSQL 18) | download the `.rpm` from the latest release, then `sudo dnf install ./pg_sift_18-*.rpm` |
| Arch Linux | the AUR package `pg_sift` (recipe in [packaging/](packaging/)) |
| Docker | `packaging/docker/build.sh 18` builds the image `pg_sift:pg18` on the official `postgres` image: [docs/QUICKSTART.md](docs/QUICKSTART.md), [docs/CONTAINERS.md](docs/CONTAINERS.md) (Docker Compose, CloudNativePG) |
| macOS arm64 | the Homebrew formula below |
| anything else | from source, below |

Then, as a superuser, `CREATE EXTENSION pg_sift` in each database (after an upgrade: `ALTER EXTENSION pg_sift UPDATE`). Install details, upgrade paths and the licence: [docs/USAGE.md](docs/USAGE.md#install), [docs/UPGRADING.md](docs/UPGRADING.md), [docs/SUPPORT.md](docs/SUPPORT.md).

From source (needs server headers with `pg_config` on `PATH` and a C compiler):

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
psql -d mydb -c 'CREATE EXTENSION pg_sift'          # as a superuser; after an upgrade: ALTER EXTENSION pg_sift UPDATE
```

<details>
<summary>macOS with Homebrew</summary>

The [formula](Formula/pg_sift.rb) in this repository builds for Homebrew PostgreSQL 17 and 18 (Homebrew has no PostgreSQL 19 formula yet):

```sh
brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git
brew install --HEAD brauliobo/pg_sift/pg_sift
brew test brauliobo/pg_sift/pg_sift
"$(brew --prefix postgresql@18)/bin/psql" -d mydb -c 'CREATE EXTENSION pg_sift;'   # a running server, as a superuser; postgresql@17 for 17
```

The HEAD-only formula builds `main` and supports macOS arm64. CI builds it and runs `brew test` (a BM25 smoke, an extension upgrade and a `sift_col` aggregate) on macOS arm64 with PostgreSQL 17 and 18; earlier it was tried by hand on Apple Silicon with 17.11 and 18.6.
`make check` also needs bash 4 or newer (`brew install bash`); the stock macOS bash 3.2 stops with a message. A stage the host cannot run (the Perl TAP tests, which need the server's `src/test/perl`; the compatibility tests that build releases older than 3.2.0; the Python 3.12 benchmark generators) is skipped with a message. CI runs a few of its regression tests on macOS, and the quick tier on Linux arm64.

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
- **Column engine** (`USING sift_col`, `USING sift_table`): `count`, `count(DISTINCT ...)`, `sum`, `avg`, `min`, `max` with filters and `GROUP BY` run on compressed columns as `Custom Scan (SiftAgg)`; `sift_table` stores the table itself in column format.
- **Vulkan (experimental, opt-in):** `make VULKAN=1` and `SET sift.gpu = auto|on` run grouped and filtered integer counts and sums of a `sift_col` index on a GPU; off by default, the CPU path unchanged ([Vulkan](#vulkan-experimental), [docs/GPU.md](docs/GPU.md)).
- **Pre-aggregates, top-N and lookups:** units whose rows are all visible answer from the exact count/sum/min/max stored with them (on by default), `ORDER BY ... LIMIT n` stops after the first rows of the units that can hold them, and an equality or `IN` lookup on a key column reads only the units that hold it (`point_columns`, `point_tid_columns`; opt-in per index).
- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, generic WAL (crash and replica safe), `pg_dump`; a query the planner cannot prove equal keeps its normal plan.
- **Ports:** [SQLite](#sqlite) and [MariaDB](#mariadb) run the same search core.

## Compared with

Feature facts checked against each project's own documentation on 2026-10-18 (speed is in Benchmarks); `?` is not verified. Full tables, a note per row, sources, and the inferred and unverified cells: [docs/COMPARISON.md](docs/COMPARISON.md).

<details>
<summary>Search: pg_sift against PG FTS, pg_search, pg_textsearch, rum, pgx-bm25, TIN and Elasticsearch</summary>

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

</details>

<details>
<summary>Analytics: pg_sift against PostgreSQL, Citus columnar, ClickHouse, Elasticsearch, DuckDB and TimescaleDB</summary>

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

</details>

pg_sift is weaker in text analysis (English stemming only, one key per index) and where its column engine is narrow (joins are accelerated for small tables only, `sift_table` scans the superblocks that hold a tombstone or a write buffer row instead of reading their pre-aggregates): [docs/COMPARISON.md](docs/COMPARISON.md#where-pg_sift-is-weaker).

## SQLite and MariaDB

The search engine runs outside PostgreSQL too, with the same core and the same results.

### SQLite

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

### MariaDB

`libsift_udf.so` and two SQL files bring the engine to MariaDB, with no storage engine and no server patch.

- **Needs:** MariaDB 10.5 to 13.x; results are identical to pg_sift.
- **Speed:** 183x to 794x faster than InnoDB FULLTEXT on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles), with an index 1.7x smaller.
- **Install and guide:** [mariadb/README.md](mariadb/README.md).

```sql
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);   -- index, queue, triggers
SELECT sift_count('news', 'power AND plant');
```

## Operations and limits

- **Options:** index options go in `WITH (...)` (`k1`, `b`, `analyzer`, `stemmer`, `positions`, `prefix_index`, `write_buffer_limit`, `merge_fanin`, ...); `sift_table` is tuned with settings (`sift.table_*`), not per table.
- **Maintenance:** on by default. A background worker (`sift.async_maintenance`) promotes write buffers and merges segments, `sift.col_auto_merge` vacuums and merges a `sift_col` index after churn, `sift.table_auto_defrag` clears the dead rows of a `sift_table`; VACUUM clears dead rows as usual.
- **Large `work_mem` and unusual planner costs:** a `seq_page_cost` above `random_page_cost` moves the planner's choice between the scan and the normal plan most; the results are the same as the normal plan's either way.
- **Inspection:** `sift.index_info`, `sift.segments` and `sift.verify`.
- **Replication:** physical replicas work through generic WAL; publishing a `sift_table` under logical replication needs `shared_preload_libraries`.
- **Cost of one library:** the BM25 index and the column engine share one library, about twice the size of the search-only build, at a cost of at most 2% of the instructions of an operation[^lib].
- **Platforms:** PostgreSQL 17, 18 or 19 (x86-64 tested; arm64 tested with 18); SQLite port on Linux x86-64 with SQLite 3.38 or newer; MariaDB port on 10.5 to 13.x.
- **Ports:** search only; the column engine is PostgreSQL-only ([docs/COL-PORTS.md](docs/COL-PORTS.md)).
- **Planner:** what is not accelerated keeps the normal plan, and a few cases raise an error that names the rewrite.
- **Weaker than other systems:** text analysis and the narrow parts of the column engine ([Compared with](#compared-with)).
- **Types, options and limits of the column engine:** [docs/USAGE.md](docs/USAGE.md#limits); settings, maintenance and replication in [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations); disk, memory and time of a large index in [docs/SIZING.md](docs/SIZING.md).

[^lib]: Against 2.0.x (`bench/steps/cpu_gate.sh`, user instructions per operation) every cell is within 1.02 except three BM25 cells: the first query of a session (about 1.04 to 1.05; connection pooling amortizes it), a `VACUUM` with nothing to free (about 1.04, on 3.9M instructions) and a ranked query on a one-segment index (1.02 to 1.03).

<!-- sift-reference:start -->

## SQL reference

Schema `sift` has 51 functions, 5 views, 1 table, 2 operator classes, 1 operator. Signatures, results, the release that added each and a line on what it does are in [docs/REFERENCE.md](docs/REFERENCE.md), generated from the SQL scripts.

- **[Search and ranking](docs/REFERENCE.md#search-and-ranking)**: `@@>`, `approx_count_distinct`, `cluster_advice`, `facet_counts`, `facet_sql`, `facets`, `field_query`, `highlight`, `index_advice`, `index_info`, `match`, `match_idx`, `max_score`, `parse_query`, `score`, `sections`, `segment_postings`, `segments`, `suggest_paths`, `text_ops`, `tokenize`, `verify`
- **[Column index](docs/REFERENCE.md#column-index)**: `col_*_ops`, `col_clustering`, `col_index_info`, `col_index_status`, `col_leaves`, `col_maintenance_status`, `col_ops`, `col_sections`, `col_segments`, `col_unit_agg`, `col_unit_hist`, `col_verify`
- **[Tables](docs/REFERENCE.md#tables)**: `table_paths`, `table_paths_maintained`, `table_segments`, `table_set_paths`, `table_space`
- **[Maintenance and workers](docs/REFERENCE.md#maintenance-and-workers)**: `col_maintain`, `col_maintenance_due`, `col_merge`, `col_promote`, `col_refresh`, `col_rollup_counters`, `col_rollup_queue`, `col_rollup_refresh`, `col_rollup_requests`, `col_rollup_state`, `merge`, `promote`, `table_maintain`, `worker_state`, `workers`
- **[Diagnostics and upgrade](docs/REFERENCE.md#diagnostics-and-upgrade)**: `col_rollup_misses`, `explain_why`, `refusal_slugs`, `stats`, `stats_reset`, `upgrade_commands`, `version`

Settings (`sift.*`) are in the [settings table](docs/USAGE.md#maintenance-and-operations), fixed limits in [docs/LIMITS.md](docs/LIMITS.md).
<!-- sift-reference:end -->

<!-- sift-options:start -->

## Options at a glance

Every index option (`WITH (...)`), every `sift.*` setting and every automatic behavior that has no setting (`auto(...)`), generated by `scripts/gen-options.sh` from the C sources and the design notes: the defaults are the ones the library is compiled with.
The class says whether it is automatic behaviour (`default`), an `opt-in`, a `diagnostic`, a `safety` limit or `deprecated`. The cells here are cut short; [docs/OPTIONS.md](docs/OPTIONS.md) has, for each, what it does, the measured gain with its benchmark, the impact on the index, the writes and the memory, when it applies, why its default is what it is, how to turn it off and what it means for an upgrade from 3.3.
The types, ranges and contexts are in the tables of [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations).

<details>
<summary>The table: 137 options, settings and automatic behaviors with their default, gain, impact and off switch</summary>

| Option | Applies to | Default | Class | Gain | Impact | Turn off | Details |
|---|---|---|---|---|---|---|---|
| `k1` | BM25 | 1.2 | default | - | REINDEX to change; no size effect | n/a: a score parameter. ALTER INDEX .. RESET (k1) returns 1.2 but the index fails queries and inserts until ... | [more](docs/OPTIONS.md#siftk1) |
| `b` | BM25 | 0.75 | default | - | REINDEX to change; no size effect | n/a: a score parameter (0 ignores document length). ALTER INDEX .. RESET ... | [more](docs/OPTIONS.md#siftb) |
| `analyzer` | BM25 | text | opt-in | keyword facet counts of broad queries on 26 values 1.9-4.5x faster than GROUP BY; selective queries or thousands of values lose | index -27.3% (450,560 to 327,680 bytes; 100,000 short keyword values of 2,000 distinct, keyword against text) ... | Leave the default text: CREATE INDEX without analyzer. On an existing index ALTER INDEX .. RESET ... | [more](docs/OPTIONS.md#siftanalyzer) |
| `stemmer` | BM25 | '' | opt-in | - | index -3.1% (8.78 to 8.51 MB; 100,000 documents of 30 words, en against none); write build +22.3% (497 to 609 ms), insert of 20,000 documents +74.7% ... | Leave unset or stemmer = 'none' at CREATE INDEX. On an existing index ALTER INDEX .. RESET ... | [more](docs/OPTIONS.md#siftstemmer) |
| `case_folding` | BM25 | true | default | - | index 0.0% (8.78 MB with and without; 100,000 documents, a quarter in capitals); write build +9.0% (446 to 486 ms), insert of 20,000 documents -2.1% ... | CREATE INDEX .. WITH (case_folding = false). On an existing index ALTER INDEX .. SET ... | [more](docs/OPTIONS.md#siftcase_folding) |
| `accent_folding` | BM25 | true | default | - | index 0.0% (8.78 MB with and without; 100,000 documents, a fifth with an accent); write build -3.8%, insert of 20,000 documents -5.6%: within noise ... | CREATE INDEX .. WITH (accent_folding = false). On an existing index ALTER INDEX .. SET ... | [more](docs/OPTIONS.md#siftaccent_folding) |
| `positions` | BM25 | true | default | phrase queries 4.1x and phrase counts 7.3x faster than positions = off (218,058 documents); word queries unchanged | index +149% of the index (52.5 to 130.9 MiB, 218,058 documents); v3.1: +44% (1M short rows), +138% (50,000 documents); write build about 30% slower ... | CREATE INDEX .. WITH (positions = off): smaller index, phrases rechecked in the heap. Existing index ... | [more](docs/OPTIONS.md#siftpositions) |
| `prefix_index` | BM25 | 0 | opt-in | 1-letter prefix on a million short rows: 10-15 ms first call in a backend (under 1 ms cached) without, under 1 ms with | index a million rows: 17 MB, 34 MB at n=3, 73 MB at n=15 (2 to 4x); 300,000 rows 8.4 to 20 MB; write n = 3 ... | Default 0. ALTER INDEX .. RESET (prefix_index) or SET (prefix_index = 0) then REINDEX ... | [more](docs/OPTIONS.md#siftprefix_index) |
| `fields` | BM25 | '' | opt-in | - | index two sections 15.6 MB against 8.3 MB for the first alone (1,000,000 rows) ... | Default '' (no sections). ALTER INDEX .. RESET (fields) then REINDEX ... | [more](docs/OPTIONS.md#siftfields) |
| `write_buffer_limit` | BM25 | 4096 | default | 1024 against 4096 kB (100,000 documents): top-10 OR 12.5 to 4.1 ms, AND 6.1 to 2.6 ms; promotion 37 to 13 ms but 3.7x as often | index +37% of the index (72 to 98 MB) and 17 to 78 segments at 1024 against 4096 kB (100,000 documents) ... | n/a: a size, not a switch (64 to 1048576 kB). ALTER INDEX .. RESET ... | [more](docs/OPTIONS.md#siftwrite_buffer_limit) |
| `vacuum_segments` | BM25 | 2 | default | 60 segments of 50,000 documents: count(*) 0.08 ms at 2 against 0.35 ms at 16; ranked top-10 0.18 against 0.33 ms | write VACUUM 0.19 s at 2 against 0.10 s at 16 (60 segments of 50,000 documents); 2 against 16 (60 segments of 50,000 documents) ... | n/a: a target (1 to 128). ALTER INDEX .. SET (vacuum_segments = 128) merges least; RESET returns 2 ... | [more](docs/OPTIONS.md#siftvacuum_segments) |
| `merge_fanin` | BM25 | 8 | default | none measured: query after 20 inserts of 2,000 documents 1.49 ms at fanin 2, 1.40 ms at 8 (140,000 documents) | index 26.7 MB at fanin 2, 25.8 MB at 8 (-3.2%); write 20 inserts of 2,000 documents 844 ms at fanin 2, 703 ms at 8 (-16.7%); lower merges more often ... | n/a: a tuning value (2 to 32). ALTER INDEX .. RESET (merge_fanin) returns 8 at the next merge ... | [more](docs/OPTIONS.md#siftmerge_fanin) |
| `build_segments` | BM25 | 0 | diagnostic | - | write serial build (parallel workers off); a serial build | Leave 0 or ALTER INDEX .. RESET (build_segments); it only acts at the next CREATE INDEX or REINDEX, which ... | [more](docs/OPTIONS.md#siftbuild_segments) |
| `dead_rewrite_pct` | BM25 | 50 | default | 5 against 50, after deletes (400,000 documents, 10 rounds of 6%): count(*) 3.5 to 0.15 ms, top-10 4.1 to 1.5 ms | index 5 against 50: file 133 against 92 MB after updates, 89 against 67 MB after deletes; write 5 against 50: VACUUM 37.8 against 20.2 s ... | SET (dead_rewrite_pct = 100) never rewrites a segment on its own ... | [more](docs/OPTIONS.md#siftdead_rewrite_pct) |
| `vacuum_truncate` | BM25 | true | default | - | index index after churn and VACUUM 8.1 MiB against 34.7 before the space step (1,000,000 rows); v2.0 example 722 to 83 blocks ... | ALTER INDEX .. SET (vacuum_truncate = false); RESET returns it on ... | [more](docs/OPTIONS.md#siftvacuum_truncate) |
| `write_buffer_limit` | sift_col | 4096 | default | 1024 against 4096 kB: ranked queries 2 to 4 times faster (sift text index, 100,000 rows; no sift_col figure) | index 1024 against 4096 kB: index 72 to 98 MB (+37%), 4.5 times the segments (sift text index, 100,000 rows); write 1024 against 4096 kB ... | n/a: a size, not a switch. ALTER INDEX .. SET (write_buffer_limit = N) applies from the next seal ... | [more](docs/OPTIONS.md#sift_colwrite_buffer_limit) |
| `dead_rewrite_pct` | sift_col | 10 | default | GROUP BY after three rounds of 30% updates and VACUUM: 0.82 ms (never rewritten) to 0.16 ms (10) (100,000 rows) | index 4.05 MB (never rewritten) to 2.15 MB (10) (-46.8%, 100,000 rows after three rounds); write VACUUM of the third round 61 ms ... | ALTER INDEX .. SET (dead_rewrite_pct = 100) never rewrites a segment for dead rows; RESET returns to 10 ... | [more](docs/OPTIONS.md#sift_coldead_rewrite_pct) |
| `vacuum_truncate` | sift_col | true | default | Q02 after VACUUM 11.49 to 7.75 ms (median, 1,000,000 rows after update/delete churn) | index smaller: free MiB inside the column file after VACUUM 16.055 to 0.008; table and indexes 200.3 to 154.5 MiB (1,000,000 rows after churn) ... | ALTER INDEX .. SET (vacuum_truncate = false), no REINDEX ... | [more](docs/OPTIONS.md#sift_colvacuum_truncate) |
| `paths` | sift_col | '' | default | GROUP BY of an array element key 192 to 4.9 ms (100,000 documents); jsonb_each over a map 338 to 9.5 ms (1,000,000 rows) | index index 2.3 MB against 56 MB heap and 6.2 MB for a GIN on the same documents (100,000 documents); the declared leaves are stored once more ... | n/a: the layout of a jsonb column; leave the column out of the index to shred nothing. SET ... | [more](docs/OPTIONS.md#sift_colpaths) |
| `compression` | sift_col | '' | default | - | index zstd:3 -7.0% and lz4 -6.2% of the index against auto, none +111.6% (2.12 MB auto; 6 columns, 100,000 rows) ... | SET (compression = 'none') stores plain chunks (index +111.6%, fastest scans); 'auto' or RESET ... | [more](docs/OPTIONS.md#sift_colcompression) |
| `preagg` | sift_col | '' | default | GROUP BY one key 8.0 to 2.1 ms, by month 35.0 to 1.4 ms, top 10 groups 46.8 to 3.6 ms (10,000,000 rows, units scanned against pre-aggregates) | index +1.1% to +1.2% of the index (300,000 and 3,000,000 rows); off costs 0.07%; preagg = 'auto' adds 5.7% to 9.0% ... | SET (preagg = 'off') keeps leaf sums only (index -8.0% on a 13-column index) ... | [more](docs/OPTIONS.md#sift_colpreagg) |
| `rollup` | sift_col | '' | default | GROUP BY of an array element key 1,888 ms normal plan, 9.2 scanned, 2.0 ms from rollups (1,000,000 documents); 151 to 0.08 ms at 100,000 | index +2.0% of the index (100,000 and 1,000,000 documents, default rollups against rollup = 'off'); write +3.4% to +4.2% build instructions ... | SET (rollup = 'off') removes the rollups and keeps the cube ... | [more](docs/OPTIONS.md#sift_colrollup) |
| `rollup_on_demand` | sift_col | true | default | every unit answers from the rollups 1.9 to 3.1 s after ALTER INDEX SET rollup (1,000,000 documents, 276 units, worker on) | write background rewrite of the segment (about 0.7 s for an 8 MB segment); the query itself writes nothing; a background rewrite of the segment ... | ALTER INDEX .. SET (rollup_on_demand = false) for one index ... | [more](docs/OPTIONS.md#sift_colrollup_on_demand) |
| `cluster_advice` | sift_col | true | diagnostic | advised sort: a key equality reads 196 units in a random heap, 18 sorted by date, 1 sorted by the key (200,000 rows) | none; maintenance only, not part of the format | ALTER INDEX .. SET (cluster_advice = off) silences it for this index ... | [more](docs/OPTIONS.md#sift_colcluster_advice) |
| `dict_lookup` | sift_col | auto | default | first query of a new backend, 300,000 rows: uuid equality 12.1 to 0.43 ms, text equality 29.3 to 0.54, text prefix 36.7 to 6.5 | index the order of the strings is 4 bytes a string: +10% to +70% of an index of one column ... | SET (dict_lookup = off) stores no order (a lookup parses the dictionary whole ... | [more](docs/OPTIONS.md#sift_coldict_lookup) |
| `cube_minmax` | sift_col | true | default | GROUP BY with min/max of an integer 5.5 to 0.34 ms (200,000 rows); 7.4 to 0.20 ms against 115 ms normal plan (1,000,000 rows) | index +0.17% of the index alone, +1.5% with superunit 16 (200,000 rows, 13 columns); +2.6% of a 13-column wide index ... | SET (cube_minmax = false) writes what 3.2 wrote; RESET restores true. Units written afterwards ... | [more](docs/OPTIONS.md#sift_colcube_minmax) |
| `rollup_dims` | sift_col | '' | opt-in | GROUP BY of two columns 171.8 ms normal plan, 8.0 scanned, 3.5 ms from the rollup (1,000,000 rows); 104 to 0.63 ms at 200,000 | index +4.6% of the index for two low-cardinality dims (96 cells a unit), +19% for three (200,000 rows); auto +8.5% to +11% (300,000 rows) ... | Off is the default: RESET (rollup_dims) or 'off' writes no rollup of rows in new units ... | [more](docs/OPTIONS.md#sift_colrollup_dims) |
| `calendar_columns` | sift_col | '' | default | GROUP BY date_trunc('month', d) 2.4 to 0.54 ms (200,000 rows); 7.7 to 0.74 ms against 320 ms normal plan (1,000,000 rows) | index +1.0% of the index for one column, +1.8% for two; beside jsonb paths only when named: +9.3% to +12.1% of a 13-column wide index ... | SET (calendar_columns = 'off') or a list of the columns you group by ... | [more](docs/OPTIONS.md#sift_colcalendar_columns) |
| `key_columns` | sift_col | '' | default | count(DISTINCT fk) over whole units 16.1 to 1.11 ms (1,000,000 rows, 123 units); 2.0x to 8.6x on three 200,000-row tables | index auto 0.0% on a column of 100 distinct values and a named column of 5,000 random keys 0.0% (2.12 MB; 100,000 rows) ... | SET (key_columns = 'off') then REINDEX (the build NOTICE names it) ... | [more](docs/OPTIONS.md#sift_colkey_columns) |
| `point_columns` | sift_col | '' | opt-in | integer equality 0.119 to 0.020 ms, IN of 5 keys 0.193 to 0.028 ms (200,000 rows; a b-tree 0.006 and 0.009) | index index 29.1 to 93.7 MB (+222%) for five leaves at 1,000,000 rows, 170 MB for six b-trees ... | RESET (point_columns): new segments get no directory; older ones keep theirs until a merge or REINDEX. ... | [more](docs/OPTIONS.md#sift_colpoint_columns) |
| `point_tid_columns` | sift_col | '' | opt-in | integer equality 0.013 ms flat from 26,700 to 1,000,000 rows (point_columns 0.015-0.022, b-tree 0.006); 100-id IN 45.7 to 0.25 ms (1,000,000 rows) | index within 3% of point_columns: 91.4 against 93.7 MB, 29.1 MB without a directory, 170 MB for six b-trees (5 leaves, 1,000,000 rows) ... | RESET (point_tid_columns); a leaf kept in point_columns then has the rows form. New segments only ... | [more](docs/OPTIONS.md#sift_colpoint_tid_columns) |
| `key_cut` | sift_col | '' | opt-in | count/sum for one key 33.4 to 0.30 ms (1,000,000 rows, no b-tree); other columns slower: an id range 8.2 against 0.15 ms (800,000 rows) | index index 17% smaller to 16% larger for narrow rows, +0.2% for wide rows (200,000 to 2,000,000 rows); write build CPU 3.4 to 4.1 times ... | ALTER INDEX .. RESET (key_cut) for units written afterwards, then REINDEX re-cuts older units by superblock ... | [more](docs/OPTIONS.md#sift_colkey_cut) |
| `key_cut_rows` | sift_col | 0 | default | none at 100,000 rows: a key range of 40 values 0.20 ms with 256 rows and with the default, 0.22 against 0.23 ms with 4,096 | index +69.0% of the index at 256 rows, -9.4% at 4,096 (100,000 rows, default 0); write build +6.8% at 256, -0.5% at 4,096 ... | n/a: a size, not a switch. RESET (key_cut_rows) or 0 returns to superblock-sized units ... | [more](docs/OPTIONS.md#sift_colkey_cut_rows) |
| `order_columns` | sift_col | '' | opt-in | ORDER BY an integer LIMIT 100 37.9 ms normal plan, 3.5 without runs, 1.4 with (1,000,000 rows); LIMIT 10 0.128 to 0.051 ms with heads (800,000) | index 2 bytes a row per unordered leaf (+50.8% of a 10-column index with 8 unordered leaves, 1,000,000 rows); heads add 0.6% to 2.4% ... | RESET (order_columns): new units get no run; older ones keep theirs until a merge or REINDEX. ... | [more](docs/OPTIONS.md#sift_colorder_columns) |
| `hll_columns` | sift_col | '' | opt-in | 5.4 ms against 63 ms for the same aggregate by the normal plan (2 leaves, 1,600,000 rows); bench case approx_distinct_sketch 109 ms against 6.4 ms ... | index +23% of the index for four columns of a million rows (169 B to 12,307 B a unit and column, by distinct values) ... | RESET (hll_columns): new units get no sketch; older units keep theirs until a merge or REINDEX, and ... | [more](docs/OPTIONS.md#sift_colhll_columns) |
| `return_columns` | sift_col | '' | opt-in | slower on a table that fits in memory: SELECT id of 100 rows 0.066 to 0.158 ms, 10,000 rows 1.49 to 1.81 ms (300,000 rows) ... | index no extra storage (reads the segment dictionary); dict_lookup auto adds the string order, +10% to +70% of a one-column index; no extra storage ... | RESET (return_columns) or 'off': no format change, no REINDEX ... | [more](docs/OPTIONS.md#sift_colreturn_columns) |
| `superunit` | sift_col | 16 | default | whole-table count/sum 60.6 ms normal plan, 4.8 scanned, 0.11 ms with superunits (1,000,000 rows); 0.26-0.37 to 0.05-0.08 ms at 200,000 | index +1.0% of the index (groups of 64: +0.8%, of 4 to 8: +1.3% to +1.5%); +2.6% with cube_minmax off (13-column wide index) ... | SET (superunit = 0) writes the bytes 3.2 wrote (no minor 12) ... | [more](docs/OPTIONS.md#sift_colsuperunit) |
| `sift.max_expansions` | BM25 | 512 | safety | - | above it a prefix matches all its terms but scores as one term | n/a: a limit (1 to 1000000). Raise it (postgresql.conf, ALTER SYSTEM, ALTER DATABASE / ROLE .. SET ... | [more](docs/OPTIONS.md#siftmax_expansions) |
| `sift.prefix_cache_size` | BM25 | 8192 kB | default | 1-letter prefix on a million short rows: 10-15 ms first call in a backend, under 1 ms from the cache | index as `sift.max_expansions`; write as `sift.max_expansions`; up to this much memory per backend; 0 keeps none | SET sift.prefix_cache_size = 0 keeps no cache (superuser ... | [more](docs/OPTIONS.md#siftprefix_cache_size) |
| `sift.enable_scan` | BM25 | on | diagnostic | no consistent difference at 200,000 rows (sub-millisecond: first 10 ids of a match 0.32 ms off, 0.42 on for rare terms ... | index n/a: a planner switch; it stores nothing; write n/a: a planner switch; it adds no write work; none ... | SET sift.enable_scan = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) ... | [more](docs/OPTIONS.md#siftenable_scan) |
| `sift.enable_count` | BM25 | on | diagnostic | count(*) of a match 0.37 to 0.29 ms (rare terms) and 0.40 to 0.33 ms (frequent terms), 200,000 documents: about 20% faster, sub-millisecond | index as `sift.enable_scan`; write as `sift.enable_scan`; none; off gives the plan PostgreSQL has without them | SET sift.enable_count = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf); RESET returns on. | [more](docs/OPTIONS.md#siftenable_count) |
| `sift.filter_pushdown` | BM25 | on | default | 0.1% filter on 218,058 documents: 23.6 to 0.22 ms (clustered), 15.0 to 0.86 ms (scattered) | none; off gives the plan PostgreSQL has without them | SET sift.filter_pushdown = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) keeps the post-filter ... | [more](docs/OPTIONS.md#siftfilter_pushdown) |
| `sift.prefetch` | BM25 | on | default | none with a warm cache: 4.41 ms off, 4.49 ms on (100,000 documents, unordered scan); the read-ahead is for a cold heap, which this host cannot make ... | none; off gives the plan PostgreSQL has without them | SET sift.prefetch = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf), only to compare plans ... | [more](docs/OPTIONS.md#siftprefetch) |
| `sift.parallel_scan` | BM25 | off | opt-in | unordered scan with a heavy residual qual 1.8x (218,058 documents); other shapes lose to the Gather cost of milliseconds | off because a Gather costs milliseconds: only an unordered scan with a heavy residual qual gained (1.8x on 218,058 articles) | Already off. SET sift.parallel_scan = on to try it; RESET (or off) returns the serial plans ... | [more](docs/OPTIONS.md#siftparallel_scan) |
| `sift.rls_scores` | BM25 | off | opt-in | ranked top-10, 100,000 documents, 50% / 5% / 0.5% visible: 0.15 to 0.9 ms; the plan the role had before took 5 to 31 ms, unranked | scores use the statistics of the whole index, hidden rows included | Already off: the role gets no ranked scan and sift.score() raises an error. Turn on with ALTER ROLE app SET ... | [more](docs/OPTIONS.md#siftrls_scores) |
| `sift.standby_conflicts` | all | on | safety | - | write replay can wait up to max_standby_streaming_delay for a standby query; replay can wait up to max_standby_streaming_delay | SET sift.standby_conflicts = off (superuser: postgresql.conf, ALTER SYSTEM) ... | [more](docs/OPTIONS.md#siftstandby_conflicts) |
| `sift.log_min_duration` | all | 1000 ms | diagnostic | - | none; a log line | SET sift.log_min_duration = -1 (superuser: postgresql.conf, ALTER SYSTEM, ALTER DATABASE .. SET) ... | [more](docs/OPTIONS.md#siftlog_min_duration) |
| `sift.simd` | all | avx512 | diagnostic | column kernels against scalar, 4096-row vectors (x86-64): reductions 4x to 27x, 2x to 10x under a 50% mask | none; a lower level only slows the kernels (auto is the best level of the CPU, shown as avx512 on x86-64 and as neon on AArch64, whose levels are ... | SET sift.simd = scalar to run the specification (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) ... | [more](docs/OPTIONS.md#siftsimd) |
| `sift.async_maintenance` | all | on | default | 20 inserts of 2,000 documents into a text index that seals every 64 kB: 696 ms with the writer merging, 385 ms with the worker (-44.7%) | a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O) | SET sift.async_maintenance = off (superuser: postgresql.conf, ALTER SYSTEM, ALTER DATABASE .. SET) ... | [more](docs/OPTIONS.md#siftasync_maintenance) |
| `sift.maintenance_naptime` | all | 2000 ms | default | after a bulk load the worker promotes the tail seals: queries 1.3 ms instead of 8-16 ms on unpromoted segments | index as `sift.async_maintenance`; write as `sift.async_maintenance`; a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms ... | n/a: a period (200 to 60000 ms). Maintenance itself is turned off with sift.async_maintenance = off. Change ... | [more](docs/OPTIONS.md#siftmaintenance_naptime) |
| `sift.maintenance_cost_delay` | all | 0 | safety | - | index as `sift.async_maintenance`; write worker merge of a 10 MB index: 0.27 s at 0, 5.3 s at 20 ms (at most 6.4 MB/s of I/O) ... | Already 0 (no sleep). Set a few ms (superuser: ALTER DATABASE .. SET or postgresql.conf) to throttle ... | [more](docs/OPTIONS.md#siftmaintenance_cost_delay) |
| `sift.maintenance_wal_rate` | all | 0 | safety | - | index as `sift.async_maintenance`; write as `sift.async_maintenance`; a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms ... | Already 0 (no limit). Set kB/s (superuser: postgresql.conf, ALTER DATABASE .. SET) to cap; RESET returns 0. | [more](docs/OPTIONS.md#siftmaintenance_wal_rate) |
| `sift.vacuum_rewrite_limit` | sift_col, sift_table | 128 MB | safety | - | index with vacuum_truncate: table and indexes after VACUUM 200.3 to 154.5 MiB (1,000,000 rows after churn) ... | SET sift.vacuum_rewrite_limit = 0 (session, ALTER ROLE/DATABASE, postgresql.conf) ... | [more](docs/OPTIONS.md#siftvacuum_rewrite_limit) |
| `sift.enable_col` | sift_col | on | diagnostic | column scans against the normal plan, 1,000,000 rows: count/sum of the table 60.6 to 0.11 ms, GROUP BY month 320 to 0.74 ms, count(DISTINCT) 108 to ... | none; off gives the plan PostgreSQL has without them | SET sift.enable_col = off for a session (ALTER ROLE/DATABASE or postgresql.conf for more) ... | [more](docs/OPTIONS.md#siftenable_col) |
| `sift.enable_col_scan` | sift_col | off | opt-in | against a b-tree: count of a 20% range 13.6 to 1.1 ms, DISTINCT/GROUP BY 82.6 to 7.5 ms, but point lookups 5x to 480x slower (1,000,000 rows) | index one sift_col index 29.1 MB against 170.2 MB for six b-trees (1,000,000 rows, 7 columns); write n/a: a planner switch for reads ... | Off is the default: SET sift.enable_col_scan = off (or RESET) in a session, or ALTER ROLE/DATABASE, ... | [more](docs/OPTIONS.md#siftenable_col_scan) |
| `sift.vis_page_cache` | sift_col | on | default | none at 100,000 rows: a key-cut range query 0.20 ms off, 0.23 ms on (within noise) | index about 48 bytes for each heap page the scan visits (at most 1,048,576 pages), freed with the scan; write n/a: a read-side cache of one scan ... | SET sift.vis_page_cache = off (session, ALTER ROLE/DATABASE, postgresql.conf) ... | [more](docs/OPTIONS.md#siftvis_page_cache) |
| `sift.col_rollup_on_demand` | sift_col | on | default | every unit answers from the rollups 1.9 to 3.1 s after ALTER INDEX SET rollup (1,000,000 documents, 276 units) | write background rewrites of segments (about 0.7 s for an 8 MB segment); the query itself writes nothing; background rewrites of segments ... | SET sift.col_rollup_on_demand = off (session, ALTER ROLE/DATABASE, postgresql.conf) ... | [more](docs/OPTIONS.md#siftcol_rollup_on_demand) |
| `sift.col_rollup_worker` | sift_col | on | default | as `sift.col_rollup_on_demand` | index as `sift.col_rollup_on_demand`; write as `sift.col_rollup_on_demand`; background rewrites of segments ... | SET sift.col_rollup_worker = off (session, ALTER ROLE/DATABASE, postgresql.conf) ... | [more](docs/OPTIONS.md#siftcol_rollup_worker) |
| `sift.cluster_advice` | sift_col | on | diagnostic | n/a: it reports advice and changes no plan; the gain is that of the order or key_cut it recommends (see sift_col(key_cut)) | none; maintenance only | SET sift.cluster_advice = off (session, ALTER ROLE/DATABASE, postgresql.conf) ... | [more](docs/OPTIONS.md#siftcluster_advice) |
| `sift.col_format_minor` | sift_col, sift_table | 15 | default | - | the options that write a minor above the setting are ignored; a 3.2 library refuses segments of minor 7 and above | SET sift.col_format_minor = 6 (3.2), 4 (3.1) or 3 (3.0) before writing. It is superuser-only and workers ... | [more](docs/OPTIONS.md#siftcol_format_minor) |
| `sift.col_auto_merge` | sift_col | on | default | 5% of 200,000 rows updated: grouped sum 10.8 to 0.24 ms (2.6 VACUUM only); answerable units 32% to 51% (wide rows), 33% to 83% (mixed), 1,000,000 rows | index file up to twice its live size after a rewrite until later VACUUMs; live index +3% over VACUUM only after 10 cycles (200,000 rows) ... | Server or database only, the worker reads no session value ... | [more](docs/OPTIONS.md#siftcol_auto_merge) |
| `sift.col_auto_merge_dirty_pct` | sift_col | 30 | default | as `sift.col_auto_merge` | index as `sift.col_auto_merge`; write as `sift.col_auto_merge`; background WAL and I/O ... | n/a: a threshold. 100 never fires this reason (it is 'above') ... | [more](docs/OPTIONS.md#siftcol_auto_merge_dirty_pct) |
| `sift.col_auto_merge_fragmentation` | sift_col | 2 | default | as `sift.col_auto_merge` | index as `sift.col_auto_merge`; write as `sift.col_auto_merge`; background WAL and I/O ... | n/a: a threshold. Raise it (up to 1000) to rewrite later ... | [more](docs/OPTIONS.md#siftcol_auto_merge_fragmentation) |
| `sift.col_auto_merge_buffer_pct` | sift_col | 1 | default | as `sift.col_auto_merge` | index as `sift.col_auto_merge`; write as `sift.col_auto_merge`; background WAL and I/O ... | n/a: a threshold. 100 never fires it; lower it to promote the buffer sooner. sift.col_auto_merge = off stops ... | [more](docs/OPTIONS.md#siftcol_auto_merge_buffer_pct) |
| `sift.col_auto_merge_wal_pct` | sift_col | 5 | safety | - | write rewrites kept within 5% of the server WAL: total WAL per round equal to VACUUM only (299.0 / 304.7 MB with full-page images ... | SET sift.col_auto_merge_wal_pct = 0 (ALTER SYSTEM or ALTER DATABASE) removes the wait ... | [more](docs/OPTIONS.md#siftcol_auto_merge_wal_pct) |
| `sift.col_auto_merge_max_active` | sift_col | 0 | safety | - | write as `sift.col_auto_merge`; background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at ... | 0 (the default) never waits: SET sift.col_auto_merge_max_active = 0 or RESET at server or database level ... | [more](docs/OPTIONS.md#siftcol_auto_merge_max_active) |
| `sift.col_auto_merge_partition_kb` | sift_col | 0 | opt-in | 1,000,000 rows, limit 8 MB: units answerable 42% to 100% (uniform churn, grouped sum 7.4 to 0.10 ms) and 71% to 100% (8.4 to 0.16 ms) | write VACUUM plus worker WAL equals the whole-index rewrite (15.3 against 15.05 MB uniform, 12.8 against 12.7 MB scraper a round) ... | 0 (the default) rewrites the index whole, in partitions of the vacuum_rewrite_limit size above it ... | [more](docs/OPTIONS.md#siftcol_auto_merge_partition_kb) |
| `sift.col_auto_merge_max_workers` | sift_col | 0 | safety | - | write as `sift.col_auto_merge`; background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at ... | 0 (the default) is no limit: SET sift.col_auto_merge_max_workers = 0 or RESET at server or database level ... | [more](docs/OPTIONS.md#siftcol_auto_merge_max_workers) |
| `sift.enable_agg` | sift_col, sift_table | on | default | GROUP BY of 1,000,000 rows, enable_agg off to on: 60.6 to 0.11 ms (totals), 320 to 0.74 ms (month totals), 207 to 30 ms (grouping sets) | none; off gives the plan PostgreSQL has without sift | SET sift.enable_agg = off (session), ALTER ROLE/DATABASE .. SET, or postgresql.conf ... | [more](docs/OPTIONS.md#siftenable_agg) |
| `sift.agg_left_lateral` | sift_col, sift_table | on | default | LEFT JOIN LATERAL over a row's list, three captured queries: lateral part 27.1 to 10.1, 33.4 to 6.8, 30.9 to 6.8 ms (small data, median of 3) | none; off keeps the normal plan of such a query (the way to compare it and to isolate a wrong result) | SET sift.agg_left_lateral = off (session), ALTER ROLE/DATABASE .. SET, or postgresql.conf ... | [more](docs/OPTIONS.md#siftagg_left_lateral) |
| `sift.agg_preagg` | sift_col, sift_table | on | default | 1,000,000 rows, preagg off to on: min/max per group 7.4 to 0.20 ms, month totals 7.7 to 0.74 ms; 3,000,000 rows month totals 12.7 to 0.65 ms | index +1.1% to +1.2% of the index (cube, 2.4 kB per unit; 300,000 and 3,000,000 rows); write serial build +3.9% CPU (3,000,000 rows, quiet window) ... | SET sift.agg_preagg = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_preagg) |
| `sift.agg_preagg_distinct` | sift_col, sift_table | on | default | count(DISTINCT) of an 8-value column, 25,000-200,000 rows: 0.25-1.47 ms to 0.10-0.52 ms; a 40,000-value key, 1,000,000 rows ... | index as `sift.agg_preagg`; write as `sift.agg_preagg`; none on the index; off scans every unit | SET sift.agg_preagg_distinct = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_preagg_distinct) |
| `sift.agg_super` | sift_col, sift_table | on | default | GROUP BY a 20-value key, count and sum, 400,000 rows (391 units, groups of 64): 3.09 ms from the units to 0.22 ms from the superunit summaries | index +1.0% of the index at the default superunit = 16 (200,000 rows, 53 units); +0.8% at 64, +1.3% to +1.5% at 4 to 8 ... | SET sift.agg_super = off (session, role, database or postgresql.conf) reads each unit's own pre-aggregate. ... | [more](docs/OPTIONS.md#siftagg_super) |
| `sift.enable_topn` | sift_col, sift_table | on | default | ORDER BY .. LIMIT 10 with a search, 1M / 10M rows: 7.1 to 2.4 ms / 191 to 19.5 ms; a page of rows with totals, 1,000,000 rows: 85.1 to 4.4 ms | none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns | SET sift.enable_topn = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftenable_topn) |
| `sift.topn_walk` | sift_col, sift_table | on | default | ORDER BY key LIMIT 10 or 1000, key in blocks of 3,000 to 12,000 rows, 3,996,000 rows: 2.3 to 6.4x faster; a random key LIMIT 10: 3.1 to 3.4x faster | index as `sift.enable_topn`; write as `sift.enable_topn`; none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs ... | SET sift.topn_walk = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#sifttopn_walk) |
| `sift.topn_zone_keys` | sift_col, sift_table | on | default | ORDER BY g DESC, id DESC LIMIT 10, 25,000 to 200,000 rows: units read 20 to 157 down to 1 to 2; 1,000,000 rows, 5 statuses: 5.7 ms vs 5.5 ms off ... | index as `sift.enable_topn`; write as `sift.enable_topn`; none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs ... | SET sift.topn_zone_keys = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#sifttopn_zone_keys) |
| `sift.topn_runs` | sift_col, sift_table | on | default | ORDER BY k LIMIT 100 with order_columns, 1,000,000 rows: 3.5 to 1.4 ms (runs off, on) ... | index +50.8% of a ten-column index with order_columns on every column (1,000,000 rows); heads +0.6% (three columns, 800,000 rows) ... | SET sift.topn_runs = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#sifttopn_runs) |
| `sift.agg_lookup_pushdown` | sift_col, sift_table | on | default | semi join of a group of 1,000 keys, 3,000,000 rows: generic lookup join 70.8 to 22.9 ms, byte table 24.8 to 5.5 ms | memory: up to 256 MB a backend, and shared memory of 5% of shared_buffers by default; the memory limits stop a larger table from being loaded | SET sift.agg_lookup_pushdown = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_lookup_pushdown) |
| `sift.agg_lookup_cache_kb` | sift_col, sift_table | 262144 kB | safety | an unchanged 200,000-row joined table: 27 ms to load, 0.47 ms to check; 25,000 rows 3.3 ms and 0.09 ms | index 12 MB per kept table of 500,000 rows with three text keys (76 MB before); at most 256 MB a backend; write as `sift.agg_lookup_pushdown` ... | SET sift.agg_lookup_cache_kb = 0 (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_lookup_cache_kb) |
| `sift.agg_lookup_shared_kb` | sift_col, sift_table | -1 | safety | first query of a fresh backend, 200,000 mapping rows, one integer key: 35.6 ms to load, 17.0 ms to read shared ... | index 32 sessions joining a 200,000-row table: 254 MB private, 88 MB shared in all; write the backend that writes the entry pays +16 ms once ... | SET sift.agg_lookup_shared_kb = 0 (superuser; ALTER ROLE/DATABASE .. SET or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_lookup_shared_kb) |
| `sift.agg_lookup_max_kb` | sift_col, sift_table | 262144 kB | safety | spine of 1,000 / 10,000 / 100,000 rows over 20,000 facts: 60 / 425 / 693 ms (100,000 refused) to 10.6 / 17 / 130 ms (spine matched by a hash) | index as `sift.agg_lookup_pushdown`; write as `sift.agg_lookup_pushdown`; a larger sub-select, VALUES list or function keeps the normal join | n/a: a memory limit, not a switch; raise it to admit larger sets. A non-negative sift.agg_lookup_max_rows ... | [more](docs/OPTIONS.md#siftagg_lookup_max_kb) |
| `sift.agg_lookup_max_rows` | sift_col, sift_table | -1 | deprecated | - | index as `sift.agg_lookup_pushdown`; write as `sift.agg_lookup_pushdown`; the older form of agg_lookup_max_kb; -1 lets the memory decide | SET sift.agg_lookup_max_rows = -1 (or RESET) returns to the memory limit ... | [more](docs/OPTIONS.md#siftagg_lookup_max_rows) |
| `sift.agg_share_lookups` | sift_col, sift_table | on | default | as `sift.agg_lookup_pushdown` | index as `sift.agg_lookup_pushdown`; write as `sift.agg_lookup_pushdown`; memory ... | SET sift.agg_share_lookups = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_share_lookups) |
| `sift.agg_semi_lut` | sift_col, sift_table | on | default | IN or EXISTS over a group of 1,000 keys, 3,000,000 rows: 66.0 ms normal plan, 27.0 generic lookup join, 6.0 byte table; NOT EXISTS 85.2 to 10.4 ms | memory of that table, 4096 kB by default per scan and per worker; keys that span more keep the generic join | SET sift.agg_semi_lut = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_semi_lut) |
| `sift.agg_semi_lut_size` | sift_col, sift_table | 4096 kB | safety | as `sift.agg_semi_lut` | index as `sift.agg_semi_lut`; write as `sift.agg_semi_lut`; memory of that table, 4096 kB by default per scan and per worker ... | n/a: a memory limit; lower it to cap memory. 0 sends every semi or anti join with a key to the generic join ... | [more](docs/OPTIONS.md#siftagg_semi_lut_size) |
| `sift.agg_param_set_size` | sift_col, sift_table | 1000 | default | col = ANY($1) in a generic plan, 3,000,000 rows, 1,000 / 10,000 / 100,000 keys ... | none; planning only (0 keeps the planner's guess of 10) | SET sift.agg_param_set_size = 0 gives back the planner's own guess of 10 elements (the bitmap scan is kept) ... | [more](docs/OPTIONS.md#siftagg_param_set_size) |
| `sift.agg_key_skew` | sift_col, sift_table | on | default | counted lookup join of 5 heavy keys that hold a third of 200,000 rows: normal plan 34 ms, scan 8.6 ms ... | none; planning only | SET sift.agg_key_skew = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_key_skew) |
| `sift.agg_rollup_cost` | sift_col, sift_table | 1 | default | GROUP BY kind with count, 13,800 elements in 1,440 rollup tuples: 14.6M instructions from the rollup, 7.0M from the scan (100,000 documents) | none; planning only (0 always, large never) | SET sift.agg_rollup_cost = 0 answers every unit from its rollup (the older behavior, used by tests) ... | [more](docs/OPTIONS.md#siftagg_rollup_cost) |
| `sift.agg_dense` | sift_col, sift_table | on | default | count(*) FILTER per flag over 387,593 elements: 34.0M to 10.9M instructions; GROUP BY kind with count and avg: 41.1M to 26.9M | none on the index; off gives the generic path with the same rows | SET sift.agg_dense = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_dense) |
| `sift.agg_two_level` | sift_col, sift_table | on | default | UNION ALL arms grouped by a row id, 1,000,000 rows: 428 ms normal plan, 90 ms with the scan (case two_level_spend; the switch alone is not isolated) | index as `sift.agg_dense`; write as `sift.agg_dense`; none on the index; off gives the generic path with the same rows | SET sift.agg_two_level = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_two_level) |
| `sift.agg_dist_stream` | sift_col, sift_table | on | default | as `sift.agg_dense` | index as `sift.agg_dense`; write as `sift.agg_dense`; none on the index; off gives the generic path with the same rows | SET sift.agg_dist_stream = off (session, role, database or postgresql.conf) restores the plain scan. No ... | [more](docs/OPTIONS.md#siftagg_dist_stream) |
| `sift.agg_fuse` | sift_col, sift_table | on | default | three UNION ALL arms over a lookup join, 100,000 rows: 18.7 ms arm by arm, 8.6 ms in one pass | index as `sift.agg_dense`; write as `sift.agg_dense`; none on the index; off gives the generic path with the same rows | SET sift.agg_fuse = off (session, role, database or postgresql.conf): each arm runs alone. No REINDEX. | [more](docs/OPTIONS.md#siftagg_fuse) |
| `sift.agg_join_filter` | sift_col, sift_table | on | default | as `sift.agg_dense` | index as `sift.agg_dense`; write as `sift.agg_dense`; none on the index; off gives the generic path with the same rows | SET sift.agg_join_filter = off (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftagg_join_filter) |
| `sift.agg_rows` | sift_col, sift_table | on | default | count(DISTINCT id) over per-row aggregates joined to a spine, 109,000 rows, work_mem 4MB: 753 ms (sort and GroupAggregate) to 444 ms | index as `sift.agg_dense`; write as `sift.agg_dense`; none on the index; off gives the generic path with the same rows | SET sift.agg_rows = off (session, role, database or postgresql.conf) restores PostgreSQL's sort and ... | [more](docs/OPTIONS.md#siftagg_rows) |
| `sift.agg_combo_cache` | sift_col, sift_table | 1048576 | safety | - | index n/a: a per-scan memory cache; nothing is stored in the index; write n/a: a per-scan memory cache; it adds no write work ... | n/a: a bound on the cache, not a switch (16 to 16,777,216); a small value only makes it restart often. | [more](docs/OPTIONS.md#siftagg_combo_cache) |
| `sift.explain_refusals` | sift_col, sift_table | off | diagnostic | - | none when off; the plans are the same on or off | Default off. SET sift.explain_refusals = off or RESET (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftexplain_refusals) |
| `sift.count_refusals` | sift_col, sift_table | off | diagnostic | as `sift.explain_refusals` | index as `sift.explain_refusals`; write as `sift.explain_refusals`; a few microseconds a query that reaches a candidate path | Default off. SET sift.count_refusals = off or RESET (session, role, database or postgresql.conf) ... | [more](docs/OPTIONS.md#siftcount_refusals) |
| `sift.gpu` | sift_col, sift_table | off | opt-in | device vs one CPU core, chunks resident, 6 integer count/sum queries: 0.77x to 1.07x at 1,000,000 rows, 0.99x to 1.00x at 10,000,000 ... | needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says | Default off. SET sift.gpu = off (superuser setting; ALTER ROLE/DATABASE .. SET or postgresql.conf). Without a ... | [more](docs/OPTIONS.md#siftgpu) |
| `sift.gpu_min_rows` | sift_col, sift_table | 5000000 | opt-in | as `sift.gpu` | index as `sift.gpu`; write as `sift.gpu`; needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says | n/a: a threshold (0 to INT_MAX); sift.gpu = off is the switch. 0 lets auto take every eligible grouped ... | [more](docs/OPTIONS.md#siftgpu_min_rows) |
| `sift.gpu_device` | sift_col, sift_table | '' | opt-in | as `sift.gpu` | index as `sift.gpu`; write as `sift.gpu`; needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says | n/a: it chooses a device. RESET sift.gpu_device (superuser) gives the best-device rule back ... | [more](docs/OPTIONS.md#siftgpu_device) |
| `sift.gpu_cache_mb` | sift_col, sift_table | 256 MB | safety | as `sift.gpu` | index as `sift.gpu`; write as `sift.gpu`; device memory per backend for chunks that stay on the device | n/a: a memory cap (16 MB minimum, plus 32 MB staging); sift.gpu = off uses no device memory. | [more](docs/OPTIONS.md#siftgpu_cache_mb) |
| `sift.table_write_buffer_limit` | sift_table | 4096 kB | default | none: query 1.24 ms at 2 MB and at 8 MB (100,000 rows) | index table 3.52 MB at 2 MB, 2.05 MB at 8 MB (-41.9%): fewer, larger segments (100,000 rows loaded in 20 statements) ... | n/a: a size, not a switch. RESET sift.table_write_buffer_limit returns 4096 ... | [more](docs/OPTIONS.md#sifttable_write_buffer_limit) |
| `sift.table_wide_rows` | sift_table | on | default | - | write 5 / 50 / 200 MB rows insert in 45 / 403 / 1376 ms; none for rows that fit ... | SET sift.table_wide_rows = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) ... | [more](docs/OPTIONS.md#sifttable_wide_rows) |
| `sift.table_inline_promote` | sift_table | on | default | none: query 1.29 ms off, 1.42 ms on (within noise; 100,000 rows) | index 0.0% (2.10 MB both; 100,000 rows); write the writer that closes a buffer pays a 25-31 ms promotion ... | SET sift.table_inline_promote = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) ... | [more](docs/OPTIONS.md#sifttable_inline_promote) |
| `sift.table_dict_lookup` | sift_table | off | opt-in | first lookup of a new backend: uuid equality (300,000 rows) 12.1 to 0.43 ms, text prefix 36.7 to 6.5 ms; warm statements unchanged | index +14.1% of a 6-column table and +10.3% of a table of an integer and a text column, 100,000 distinct 32-character strings (4 bytes a string ... | Already off. After SET sift.table_dict_lookup = on, SET it off or RESET ... | [more](docs/OPTIONS.md#sifttable_dict_lookup) |
| `sift.table_async_promote` | sift_table | off | opt-in | statements that close a buffer 25-31 ms to at most 2.8 ms; median 65 us, p99 190 us, p99.9 0.6 ms unchanged (single-row INSERTs, 100,000 rows) | write 4 clients of single-row INSERT: tps and p99 equal within noise; off by default: 4 clients of single-row INSERT, tps and p99 equal within noise | Already off. SET sift.table_async_promote = on to use it ... | [more](docs/OPTIONS.md#sifttable_async_promote) |
| `sift.table_bulk_groups` | sift_table | on | default | COPY of 200,000 rows (13 columns): CPU 0.52 to 0.39 s; 20 loads of 2,000 rows: WAL 8.8 to 0.3 MB | index relation 1.2 against 1.2 MB (200,000 rows); 20 loads of 2,000 rows 6.1 to 2.5 MB; write COPY of 200,000 rows: WAL 44.2 to 1.1 MB ... | SET sift.table_bulk_groups = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) ... | [more](docs/OPTIONS.md#sifttable_bulk_groups) |
| `sift.table_backpressure` | sift_table | 0 | opt-in | n/a: it acts only when the directory is nearly full, which a focused run reaches only with the test setting sift.table_max_segments | write a writer past the percent waits for the maintenance lock and merges up to 16 steps instead of 4; the wait ... | Already 0 (a writer waits only when the directory is nearly full). SET sift.table_backpressure = 0 or RESET ... | [more](docs/OPTIONS.md#sifttable_backpressure) |
| `sift.table_compression` | sift_table | '' | default | - | index zstd:3 -4.7% of the table against auto (2.10 to 2.00 MB, 100,000 rows); none 30 MiB against 13 MiB with auto (1,000,000 rows) ... | SET sift.table_compression = 'none' for no compression (scans fastest at 2x bytes) ... | [more](docs/OPTIONS.md#sifttable_compression) |
| `sift.table_dead_rewrite_pct` | sift_table | 30 | default | - | index file near 1.4x the live rows instead of up to 2x at 30 against 50 (scattered deletes and updates) ... | SET sift.table_dead_rewrite_pct = 100 never rewrites a lone segment ... | [more](docs/OPTIONS.md#sifttable_dead_rewrite_pct) |
| `sift.table_auto_defrag` | sift_table | on | default | update/insert/delete mix, 4 clients, 200,000 rows: 7,536 against 5,092 tx/s, range read p99 16.8 against 34.2 ms | index file 10.7 against 18.1 MB (200,000 rows); 19.2 to 8.2 MB after 12 cycles of 4% churn; write WAL +60% on the mix ... | SET sift.table_auto_defrag = off (server or database; postgresql.conf, ALTER DATABASE .. SET) restores the ... | [more](docs/OPTIONS.md#sifttable_auto_defrag) |
| `sift.table_auto_defrag_indexes` | sift_table | on | default | as `sift.table_auto_defrag` | index as `sift.table_auto_defrag`; write as `sift.table_auto_defrag`; background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and ... | SET sift.table_auto_defrag_indexes = off (server or database) ... | [more](docs/OPTIONS.md#sifttable_auto_defrag_indexes) |
| `sift.table_auto_defrag_free_pct` | sift_table | 25 | default | as `sift.table_auto_defrag` | index as `sift.table_auto_defrag`; write as `sift.table_auto_defrag`; background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and ... | n/a: a threshold (1 to 100). table_auto_defrag = off stops the whole pass; RESET returns 25. | [more](docs/OPTIONS.md#sifttable_auto_defrag_free_pct) |
| `sift.table_auto_defrag_delay` | sift_table | 10 ms | safety | as `sift.table_auto_defrag` | index as `sift.table_auto_defrag`; write as `sift.table_auto_defrag`; background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and ... | SET sift.table_auto_defrag_delay = 0: the pass never waits (full speed). RESET returns 10. | [more](docs/OPTIONS.md#sifttable_auto_defrag_delay) |
| `sift.table_auto_defrag_dead_pct` | sift_table | 5 | default | read after 16% churn 35.8 ms at 20, 7.6 ms at 5, 6.7 ms at 2 (200,000 rows; 0.5 ms fresh) | index as `sift.table_auto_defrag`; write mix at 20 / 5: 3,206 / 3,452 tx/s, WAL 164 / 157 MB (200,000 rows, 4 clients) ... | SET sift.table_auto_defrag_dead_pct = 100 never asks for dead rows (the free-space trigger remains) ... | [more](docs/OPTIONS.md#sifttable_auto_defrag_dead_pct) |
| `sift.table_auto_defrag_max_active` | sift_table | 0 | safety | no throughput gain beyond the run-to-run spread (10-30%) in 3 rounds (200,000 rows, 4 clients) | index as `sift.table_auto_defrag`; write as `sift.table_auto_defrag`; background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and ... | Already 0 (never waits). SET sift.table_auto_defrag_max_active = n to wait; RESET returns 0. | [more](docs/OPTIONS.md#sifttable_auto_defrag_max_active) |
| `sift.table_auto_defrag_wal_rate` | sift_table | 0 | safety | no throughput gain beyond the run-to-run spread (10-30%) in 3 rounds (200,000 rows, 4 clients) | index as `sift.table_auto_defrag`; write 5.2 MB pass: 368 ms unlimited, 1.6 s at 2 MB/s ... | Already 0 (no limit). SET sift.table_auto_defrag_wal_rate = n kB/s to cap; RESET returns 0. | [more](docs/OPTIONS.md#sifttable_auto_defrag_wal_rate) |
| `sift.table_auto_defrag_coalesce` | sift_table | on | default | after 6 cycles of churn: 2 groups against 4, aggregate 20-21 against 28-32 ms, range scan 9.5-14 against 14-18 ms; none after 12 cycles (200,000 rows) | index file 8.7 against 10.1 MB after 6 cycles (200,000 rows); write WAL 50.3 against 49.3 MB (6 cycles, 200,000 rows) ... | SET sift.table_auto_defrag_coalesce = off (server or database) ... | [more](docs/OPTIONS.md#sifttable_auto_defrag_coalesce) |
| `sift.table_auto_defrag_budget` | sift_table | 128 MB | safety | as `sift.table_auto_defrag` | index as `sift.table_auto_defrag`; write as `sift.table_auto_defrag`; background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and ... | n/a: a bound; 0 removes the cap. The pass itself is off with sift.table_auto_defrag = off. RESET returns 128. | [more](docs/OPTIONS.md#sifttable_auto_defrag_budget) |
| `sift.plan_scale` | sift_col, sift_table | 1 | diagnostic | - | none at 1; a diagnostic | SET sift.plan_scale = 1 (or RESET) in the session; planning only, nothing to rebuild. | [more](docs/OPTIONS.md#siftplan_scale) |
| `sift.agg_force` | sift_col, sift_table | off | diagnostic | 27 nested-list statements, 31.7M rows, 32 keys, work_mem 4GB: 136.8 s plain, 117.4 s planner's choice, 6.3 s forced ... | none; off; a diagnostic: the scan is priced as free and an inline shared CTE with a scan is kept | Default off. SET sift.agg_force = off or RESET; ALTER ROLE .. RESET sift.agg_force undoes a role setting. The ... | [more](docs/OPTIONS.md#siftagg_force) |
| `sift.agg_scale_horizon` | sift_col, sift_table | 100 | default |  | none at 1; two more plans of the statement where the scans cost less than the shared work but not by the margin | SET sift.agg_scale_horizon = 1 (minimum; session, role, database or postgresql.conf) keeps the choice at the ... | [more](docs/OPTIONS.md#siftagg_scale_horizon) |
| `auto(lateral-decorrelation)` | sift_col, sift_table | automatic | default | 1,000,000 orders, 2,000 outer rows: lateral count and sum 470 to 13.3 ms (35x), lateral that groups 598 to 29.8 ms (20x) ... | No index, write or WAL effect. A statement of the shape is planned twice (grouped and as written), about 2-3 ms more on star queries ... | SET sift.enable_agg = off (the whole aggregate scan; the query is planned as written). There is no setting ... | [more](docs/OPTIONS.md#autolateral-decorrelation) |
| `auto(top-n-per-group)` | sift_col, sift_table | automatic | default | 3 columns, N = 3, one month: 321 to 176 ms and 213 to 171 ms; a year, scan forced: 4,578 to 960 ms. Bench window_first_rows ... | No index, write or WAL effect. Planned twice (window and arrays), the cheaper kept. The arrays hold every row of a group before the cut, so memory is ... | SET sift.enable_agg = off (the whole aggregate scan). There is no setting for this rewrite alone. | [more](docs/OPTIONS.md#autotop-n-per-group) |
| `auto(shared-cte-choice)` | sift_col, sift_table | automatic | default | Planning -23% over a 624-statement replay (7,684 to 5,942 ms; -29% where the scans win), -38% on a 21-CTE family (891 to 550 Minstr) ... | Memory under 1 MB a backend (4,096 entries of about 200 bytes). Every 8th hit plans in full ... | SET sift.enable_agg = off (no CTE or lateral planning by the hook). The cache has no setting ... | [more](docs/OPTIONS.md#autoshared-cte-choice) |
| `auto(cte-preinline)` | sift_col, sift_table | automatic | default | Planning instructions -23.0% (10,772 to 8,294 Minstr, 156 replay statements, steady backend), -21.8% for a backend's first planning, -23.3% on 237 ... | Nothing stored, no index, write or WAL effect. Saves planning time; the plan and its costs are equal ... | SET sift.enable_agg = off (the hook plans nothing). There is no setting for it alone. | [more](docs/OPTIONS.md#autocte-preinline) |
| `auto(point-directory-aggregate)` | sift_col | automatic | default | 51-key IN over 16,384 units 23.7 to 3.6 ms (slope 0.80 to 0.20), 51-row lookup join 28.6 to 2.7 ms; point_keys_agg (1,000,000 rows) 80.9 to 2.85 ms ... | No index growth or write cost of its own (the directory is the one the options write). The directory is asked once per segment and source and kept ... | No setting for the guard. ALTER INDEX .. RESET (point_columns) and RESET ... | [more](docs/OPTIONS.md#autopoint-directory-aggregate) |
| `auto(run-heads)` | sift_col | automatic | default | ORDER BY k LIMIT 10 0.128 to 0.051 ms (800,000 rows; b-tree 0.010), executor 645k to 245k instructions ... | index +0.60% (94,044,160 against 93,478,912 bytes, three order columns, 800,000 rows); +2.4% (narrow index, 353,029 rows) ... | SET sift.col_format_minor = 14 (runs are written without heads ... | [more](docs/OPTIONS.md#autorun-heads) |
| `auto(top-n-zone-probe)` | sift_col, sift_table | automatic | default | ORDER BY rnd LIMIT 10, 100,000 rows with order_columns: 617,524 to 524,572 instructions a statement (executor 288k to 192k ... | Nothing stored, no index growth or write cost. Saves about 94k instructions a statement; the begin of each superblock still loads its write buffers. | SET sift.enable_topn = off turns off the top-N scan; the probe has no setting of its own. | [more](docs/OPTIONS.md#autotop-n-zone-probe) |
| `auto(parallel-aggregate-scan)` | sift_col, sift_table | automatic | default | 1,600,000 rows, calendar LEFT JOIN facts: 522 to 79 ms with 8 workers; count(DISTINCT) of a joined column, 800,000 rows: 29.9 to 17.7 ms (4) ... | Each process keeps its own tables and the leader merges them; lookup tables are written to shared memory once and mapped by the workers. Worker start ... | SET max_parallel_workers_per_gather = 0 (PostgreSQL's setting ... | [more](docs/OPTIONS.md#autoparallel-aggregate-scan) |
| `auto(preagg-exceptions)` | sift_col | automatic | default | count and avg by a key WHERE stamp IS NULL, 353,029 rows: 6.1 to 2.5 ms, 171 of 171 units from pre-aggregates (5 before) ... | index +1.41% (6,381,568 to 6,471,680 bytes; 353,029 rows, 655 stamped, 166 units with exceptions); write not measured; format minor 19 ... | SET sift.agg_preagg = off (no unit is answered from pre-aggregates) or SET sift.col_format_minor = 14 ... | [more](docs/OPTIONS.md#autopreagg-exceptions) |
| `auto(index-pick)` | sift_col, sift_table | automatic | default | 400,000 rows, a page-cut and a key_cut index: month count 0.86 to 0.036 ms; index_pick, 1,000,000 rows: 23.8 ms normal plan to 1.05 ms | Planning: about one single-index plan (0.05 to 0.1 ms) more for each extra index, growing with the index directory ... | No setting for the pick. Dropping the extra index leaves one index, planned as before ... | [more](docs/OPTIONS.md#autoindex-pick) |
| `auto(distinct-bigint)` | sift_col, sift_table | automatic | default | 1,000,000 rows: a 100,000-valued column 85.7 to 20.6 ms, a unique id 499 to 70 ms, 100 groups 448 to 27 ms ... | No index, write or WAL effect. The exact set lives in the scan's group table, bounded by hash memory ... | SET sift.enable_agg = off. There is no setting for this path alone. | [more](docs/OPTIONS.md#autodistinct-bigint) |
| `auto(histogram-aggregates)` | sift_col, sift_table | automatic | default | 40 groups, 1,000,000 float8 rows: median 193 to 76 ms; percentile_select 606 to 121 ms; decimal median, 400,000 rows, 168 to 70 ms ... | No index, write or WAL effect. The histogram of (group, value, count) is held in memory ... | SET sift.enable_agg = off. There is no setting for these aggregates alone. | [more](docs/OPTIONS.md#autohistogram-aggregates) |
| `auto(order-by-aggregate-limit)` | sift_col, sift_table | automatic | default | GROUP BY id ORDER BY sum(a * b) DESC LIMIT 10: 400,000 groups 96.6 to 14.6 ms (normal plan 334); group_by_id_top 1,000,000 groups 174 to 61 ms ... | No index, write or WAL effect. A bounded heap over the accumulators; the node's row estimate is lowered to about 2n. A NaN decimal sum or a float sum ... | SET sift.enable_agg = off. There is no setting for it alone. | [more](docs/OPTIONS.md#autoorder-by-aggregate-limit) |
| `auto(exists-decorrelation)` | sift_col, sift_table | automatic | default | 1,000,000 orders, 2,000 outer rows: exists_decor 259 to 11.7 ms (22x); 6,000 outer rows over 200,000 ... | No index, write or WAL effect. A statement of the shape is planned twice (grouped and as written), the cheaper kept ... | SET sift.enable_agg = off. There is no setting for this rewrite alone. | [more](docs/OPTIONS.md#autoexists-decorrelation) |
| `auto(top-n-run-probes)` | sift_col | automatic | default | 40 scans ORDER BY a key column LIMIT 10..49, 1,000,000 orders: 48 to 24.8 ms; ORDER BY rnd LIMIT 10, 800,000 rows: 113 to 82 us a scan; LIMIT 1000 ... | Nothing stored: the segment format does not change, no index growth or write cost. | SET sift.topn_runs = off (units are tested row by row) or SET sift.enable_topn = off. | [more](docs/OPTIONS.md#autotop-n-run-probes) |

</details>
<!-- sift-options:end -->

## Vulkan (experimental)

Opt-in and off by default: build with `make VULKAN=1` and `SET sift.gpu = auto|on` (`auto`: grouped aggregates on indexes of at least `sift.gpu_min_rows`, <!-- gen:guc:sift.gpu_min_rows -->5,000,000<!-- /gen:guc:sift.gpu_min_rows --> rows; `on`: also ungrouped). It runs grouped and filtered integer and `numeric` counts and sums of a `sift_col` index on a Vulkan device, clean all-visible units on the device and the rest on the CPU in the same query; a missing or failed device falls back to the CPU. The compressed chunks stay on the device, so use it from persistent connections (each backend pays about 0.2 s to open it). Not eligible: floats, `min`/`max`, `count(DISTINCT)`, text filters, more than one key, `sift_table`. Details and limits: [docs/GPU.md](docs/GPU.md).

Measured against the CPU on the store table: [docs/GPU.md](docs/GPU.md#when-it-helps).

## Docs

- [docs/USAGE.md](docs/USAGE.md): install, queries, ranking, options, settings, maintenance, replication, limits.
- [docs/SIZING.md](docs/SIZING.md): disk, memory and time of a large index.
- [docs/COMPARISON.md](docs/COMPARISON.md): feature tables against other search and analytics systems, with sources and the date checked.
- [docs/FORMATS.md](docs/FORMATS.md): the on-disk format numbers (metapage minor, segment formats, flags).
- [docs/UPGRADING.md](docs/UPGRADING.md): the upgrade preflight (`scripts/upgrade-check.sh`), `ALTER EXTENSION UPDATE`, `pg_upgrade` to a newer PostgreSQL major and going back to an older release.
- [docs/GPU.md](docs/GPU.md): the experimental, opt-in Vulkan aggregate path.
- [docs/COL-PORTS.md](docs/COL-PORTS.md): the column engine on SQLite and MariaDB; [docs/SAFETY.md](docs/SAFETY.md): memory safety in the C code.

## Security and license

Tests: `make check`, and `make check-full` with sanitizers, soak, fuzz and mutants ([CONTRIBUTING.md](CONTRIBUTING.md)); the tree has <!-- gen:tests -->484 regression tests (`test/sql`), 42 isolation specs, 89 TAP tests (`test/t`), 962 column fixtures and 252 coverage rows<!-- /gen:tests -->. Report crashes, wrong results and index corruption privately as in [SECURITY.md](SECURITY.md).
pg_sift is released under the [PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
