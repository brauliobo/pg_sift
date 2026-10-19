# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml) [![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml) [![release](https://img.shields.io/badge/release-3.0.2-blue)](https://github.com/brauliobo/pg_sift/releases/latest) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](#install) [![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

pg_sift adds fast full-text search and column-store analytics to PostgreSQL with one `CREATE EXTENSION pg_sift`. No external search
server, no separate analytics database, no `shared_preload_libraries`. The search engine also runs as a SQLite extension and a MariaDB library.

[Features](#features) · [Install](#install) · [Search](#search) · [Analytics](#analytics) · Benchmarks · [Compared with](#compared-with) · What is covered · [SQLite](#sqlite) · [MariaDB](#mariadb) · [Operations](#operations) · [Limits](#limits) · [Vulkan (experimental)](#vulkan-experimental) · [Docs](#docs)

## Features

- **BM25 search index** (`USING sift`): boolean, phrase, prefix, wildcard, regex and fuzzy queries, exact BM25 ranking, top-k with filters applied inside the scan, counts from the index, facets, autocomplete.
- **Column engine** (`USING sift_col`, `USING sift_table`): `count`, `count(DISTINCT ...)`, `sum`, `avg`, `min`, `max` with filters and `GROUP BY` run on compressed columns as `Custom Scan (SiftAgg)`; `sift_table` stores the table itself in column format.
- **Vulkan (experimental, opt-in, 3.2):** `make VULKAN=1` and `SET sift.gpu = auto|on` run grouped and filtered integer counts and sums of a `sift_col` index on a GPU; off by default, the CPU path unchanged ([Vulkan](#vulkan-experimental), [docs/GPU.md](docs/GPU.md)).
- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, generic WAL (crash and replica safe), `pg_dump`; a query the planner cannot prove equal keeps its normal plan.
- **Ports:** [SQLite](#sqlite) and [MariaDB](#mariadb) run the same search core.

## Install

Needs PostgreSQL 17 or 18, `pg_config` on `PATH` and a C compiler (x86-64 tested). Packages for Arch (AUR), Debian, Ubuntu and RHEL-family systems: [docs/USAGE.md](docs/USAGE.md#install).

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

## Compared with

Feature facts checked against each project's own documentation on 2026-10-18 (speed is in Benchmarks); `?` is not verified. Full tables, a note per row, sources, and the inferred and unverified cells: [docs/COMPARISON.md](docs/COMPARISON.md).

| Search | pg_sift | PG FTS | pg_search | pg_textsearch | rum | pgx-bm25 | TIN | Elasticsearch |
|---|---|---|---|---|---|---|---|---|
| BM25 ranking | Yes | No | Yes | Yes | No | Yes | Yes | Yes |
| Prefix, wildcard | Yes | Partly | Partly | Partly | Partly | Partly | Yes | Yes |
| Regex, fuzzy | Yes | No | Yes | No | No | No | Yes | Yes |
| Field weights | Partly | Yes | Partly | No | Partly | Yes | Partly | Yes |
| Facets | Partly | No | Yes | No | No | No | No | Yes |
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
| Columnar storage | Partly | Yes | No | Yes | Yes | Yes | Yes | Partly |
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
- **Planner:** what is not accelerated keeps the normal plan, and a few cases raise an error that names the rewrite.
- **Weaker than other systems:** text analysis and the narrow parts of the column engine ([Compared with](#compared-with)).
- **Types, options and limits of the column engine:** [docs/USAGE.md](docs/USAGE.md#limits).

## Vulkan (experimental)

Opt-in and off by default: build with `make VULKAN=1` and `SET sift.gpu = auto|on` (`auto`: grouped aggregates on indexes of at least `sift.gpu_min_rows`, 5,000,000 rows; `on`: also ungrouped). It runs grouped and filtered integer and `numeric` counts and sums of a `sift_col` index on a Vulkan device, clean all-visible units on the device and the rest on the CPU in the same query; a missing or failed device falls back to the CPU. The compressed chunks stay on the device, so use it from persistent connections (each backend pays about 0.2 s to open it). Not eligible: floats, `min`/`max`, `count(DISTINCT)`, text filters, more than one key, `sift_table`. Details and limits: [docs/GPU.md](docs/GPU.md).

Online-store table, one backend, one CPU core, NVIDIA GeForce RTX 3090, measured once on 2026-10-19 (ms; CPU cells: first query / median of 9; cold: first query after the device is open; resident: median of 9). The 3.1 dense path is a 3.1 development build; the device binary predates three small fixes that do not touch the kernels.

| rows | query | CPU, one core | CPU, 3.1 dense path | device, cold | device, resident | resident vs dense path |
|---|---|---|---|---|---|---|
| 1M | q7 `count(*)`, 2 filters | 3.9 / 0.7 | 4.5 / 0.6 | 4.2 | 0.51 | 1.2x |
| 1M | q8 `count(*)`, 2 filters | 1.9 / 0.4 | 1.7 / 0.5 | 3.6 | 0.55 | 0.9x |
| 1M | q9 `count, sum, sum`, 2 filters | 4.5 / 2.3 | 4.3 / 2.7 | 3.3 | 0.51 | 5.4x |
| 1M | g1 `GROUP BY country` (30 groups), `count, sum` | 4.4 / 3.0 | 5.0 / 2.7 | 3.7 | 0.44 | 6.2x |
| 1M | g2 `GROUP BY category` (40), `count, sum` | 4.0 / 2.8 | 3.1 / 1.8 | 5.4 | 0.55 | 3.4x |
| 1M | g3 `GROUP BY month` (36), `sum` | 5.2 / 3.9 | 4.0 / 2.8 | 3.7 | 0.48 | 5.9x |
| 10M | q7 `count(*)`, 2 filters | 23.4 / 23.6 | 26.8 / 24.1 | 45.0 | 3.26 | 7.4x |
| 10M | q8 `count(*)`, 2 filters | 16.1 / 13.0 | 14.1 / 8.7 | 16.0 | 2.78 | 3.1x |
| 10M | q9 `count, sum, sum`, 2 filters | 37.0 / 32.0 | 30.1 / 24.8 | 15.7 | 3.02 | 8.2x |
| 10M | g1 `GROUP BY country` (30 groups), `count, sum` | 31.9 / 28.3 | 34.3 / 27.3 | 17.8 | 2.32 | 11.8x |
| 10M | g2 `GROUP BY category` (40), `count, sum` | 27.4 / 23.8 | 23.8 / 20.4 | 25.2 | 3.67 | 5.6x |
| 10M | g3 `GROUP BY month` (36), `sum` | 47.5 / 37.9 | 36.3 / 30.0 | 21.1 | 2.42 | 12.4x |

## Docs

- [docs/USAGE.md](docs/USAGE.md): install, queries, ranking, options, settings, maintenance, replication, limits.
- [docs/SIZING.md](docs/SIZING.md): disk, memory and time of a large index.
- [docs/COMPARISON.md](docs/COMPARISON.md): feature tables against other search and analytics systems, with sources and the date checked.
- [docs/GPU.md](docs/GPU.md): the experimental, opt-in Vulkan aggregate path.
- [docs/COL-PORTS.md](docs/COL-PORTS.md): the column engine on SQLite and MariaDB; [docs/SAFETY.md](docs/SAFETY.md): memory safety in the C code.

## Security and license

Tests: `make check`, and `make check-full` with sanitizers, soak, fuzz and mutants ([CONTRIBUTING.md](CONTRIBUTING.md)). Report crashes, wrong results and index corruption privately as in [SECURITY.md](SECURITY.md).
pg_sift is released under the [PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
